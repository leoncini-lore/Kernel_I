#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/jhash.h>
#include <linux/list.h>
#include "kvstore.h"

/* Fix 4: kvstore_ prefix on all exported globals to avoid symbol-namespace pollution */
DEFINE_HASHTABLE(kvstore_table, KVSTORE_BITS);
struct mutex      kvstore_mutex;
wait_queue_head_t kvstore_wq;
unsigned int      kvstore_num_entries;
atomic_t          kvstore_gen;

/*
 * Fix 2: pending-waiter list for kv_wait() callers blocked on missing keys.
 * Protected by kvstore_mutex.  Static — only store.c touches this list.
 */
static LIST_HEAD(kvstore_waiters);

void __init kv_init(void)
{
    mutex_init(&kvstore_mutex);
    init_waitqueue_head(&kvstore_wq);
    hash_init(kvstore_table);
    kvstore_num_entries = 0;
    atomic_set(&kvstore_gen, 0);
}

void __exit kv_cleanup(void)
{
    struct kv_node    *node;
    struct hlist_node *tmp;
    unsigned int       bkt;

    mutex_lock(&kvstore_mutex);
    hash_for_each_safe(kvstore_table, bkt, tmp, node, hnode) {
        hash_del(&node->hnode);
        kfree(node->key);
        kfree(node->value);
        kfree(node);
    }
    kvstore_num_entries = 0;
    mutex_unlock(&kvstore_mutex);
}

/* Caller must hold kvstore_mutex. */
static struct kv_node *kv_find(const char *key, u32 h)
{
    struct kv_node *node;

    hash_for_each_possible(kvstore_table, node, hnode, h) {
        if (strcmp(node->key, key) == 0)
            return node;
    }
    return NULL;
}

/*
 * Insert or update <key, value>.  Blocks if the table is full and the key
 * does not already exist, waiting until a kv_del() creates a free slot.
 *
 * Fix 2: on new-key insertion, wakes only kv_waiter entries registered for
 * this exact key (O(waiters-for-key) instead of O(all-waiters)).
 *
 * Fix 7: allocation failures use goto unwind instead of nested if blocks.
 */
int kv_set(const char *key, const char *value)
{
    struct kv_node   *node;
    struct kv_waiter *w;
    char             *new_key, *new_val;
    u32               h = jhash(key, strlen(key), 0);
    unsigned int      gen;
    int               ret;

retry:
    ret = mutex_lock_interruptible(&kvstore_mutex);
    if (ret)
        return ret;

    node = kv_find(key, h);
    if (node) {
        /* Update: always allowed regardless of capacity. */
        new_val = kstrdup(value, GFP_KERNEL);
        if (!new_val) {
            mutex_unlock(&kvstore_mutex);
            return -ENOMEM;
        }
        kfree(node->value);
        node->value = new_val;
        atomic_inc(&kvstore_gen);
        wake_up_interruptible(&kvstore_wq);
        mutex_unlock(&kvstore_mutex);
        return 0;
    }

    if (kvstore_num_entries >= (unsigned int)kvstore_max_entries) {
        /*
         * Table full.  Snapshot the generation counter before releasing
         * the lock, then sleep until any modification occurs (guaranteed
         * to be noticed because we read gen while still locked).
         */
        gen = (unsigned int)atomic_read(&kvstore_gen);
        mutex_unlock(&kvstore_mutex);
        ret = wait_event_interruptible(kvstore_wq,
                (unsigned int)atomic_read(&kvstore_gen) != gen);
        if (ret)
            return ret;
        goto retry;
    }

    /* Insert new entry — goto unwind on allocation failure (Fix 7). */
    node = kmalloc(sizeof(*node), GFP_KERNEL);
    if (!node) {
        ret = -ENOMEM;
        goto err_unlock;
    }
    new_key = kstrdup(key, GFP_KERNEL);
    if (!new_key) {
        ret = -ENOMEM;
        goto err_free_node;
    }
    new_val = kstrdup(value, GFP_KERNEL);
    if (!new_val) {
        ret = -ENOMEM;
        goto err_free_key;
    }

    node->key   = new_key;
    node->value = new_val;
    init_waitqueue_head(&node->wq);
    hash_add(kvstore_table, &node->hnode, h);
    kvstore_num_entries++;
    atomic_inc(&kvstore_gen);

    /*
     * Fix 2: wake only the pending-waiter entry for this exact key.
     * setting ready = 1 before wake_up ensures the condition is visible
     * to wait_event_interruptible before it re-checks.
     */
    list_for_each_entry(w, &kvstore_waiters, list) {
        if (strcmp(w->key, key) == 0) {
            w->ready = 1;
            wake_up_interruptible(&w->wq);
        }
    }

    wake_up_interruptible(&kvstore_wq);   /* wake kv_set() capacity waiters */
    mutex_unlock(&kvstore_mutex);
    return 0;

err_free_key:
    kfree(new_key);
err_free_node:
    kfree(node);
err_unlock:
    mutex_unlock(&kvstore_mutex);
    return ret;
}

/* Copy the value for <key> into out[0..out_len).  Returns -ENOENT if missing. */
int kv_get(const char *key, char *out, size_t out_len)
{
    struct kv_node *node;
    u32 h = jhash(key, strlen(key), 0);
    int ret;

    ret = mutex_lock_interruptible(&kvstore_mutex);
    if (ret)
        return ret;

    node = kv_find(key, h);
    if (!node) {
        mutex_unlock(&kvstore_mutex);
        return -ENOENT;
    }

    strscpy(out, node->value, out_len);
    mutex_unlock(&kvstore_mutex);
    return 0;
}

/* Delete <key>.  Returns -ENOENT if the key does not exist. */
int kv_del(const char *key)
{
    struct kv_node *node;
    u32 h = jhash(key, strlen(key), 0);
    int ret;

    ret = mutex_lock_interruptible(&kvstore_mutex);
    if (ret)
        return ret;

    node = kv_find(key, h);
    if (!node) {
        mutex_unlock(&kvstore_mutex);
        return -ENOENT;
    }

    hash_del(&node->hnode);
    kvstore_num_entries--;
    kfree(node->key);
    kfree(node->value);
    kfree(node);
    atomic_inc(&kvstore_gen);
    mutex_unlock(&kvstore_mutex);
    wake_up_interruptible(&kvstore_wq);   /* wake kv_set() capacity waiters */
    return 0;
}

/*
 * Block until <key> is present in the table, then copy its value into
 * out[0..out_len).  Returns -EINTR/-ERESTARTSYS if interrupted by a signal.
 *
 * Fix 2 — per-key wait queues to eliminate thundering herd:
 *   The previous design used a single global kvstore_wq, so every kv_set()
 *   or kv_del() woke ALL waiters regardless of which key they waited on,
 *   causing O(N*M) wake/check/sleep cycles for N waiters and M unrelated
 *   writes.
 *
 *   Now each unique missing key gets one kv_waiter entry in kvstore_waiters.
 *   Multiple callers waiting on the same key share that entry via refcount.
 *   kv_set() walks kvstore_waiters and wakes only entries matching the
 *   inserted key.  On wake, each caller decrements refcount; the last one
 *   removes and frees the entry.
 *
 * Mutex note: mutex_lock() (not mutex_lock_interruptible) is used in the
 * post-sleep cleanup path.  This is intentional: once wait_event_interruptible
 * returns (for any reason), the caller MUST decrement refcount and potentially
 * free the kv_waiter entry.  A signal arriving in cleanup would leak the entry.
 * The lock is held only briefly in this bounded cleanup section.
 */
int kv_wait(const char *key, char *out, size_t out_len)
{
    struct kv_node   *node;
    struct kv_waiter *w, *pending = NULL;
    u32               h = jhash(key, strlen(key), 0);
    int               ret;

    ret = mutex_lock_interruptible(&kvstore_mutex);
    if (ret)
        return ret;

    /* Fast path: key already present. */
    node = kv_find(key, h);
    if (node) {
        strscpy(out, node->value, out_len);
        mutex_unlock(&kvstore_mutex);
        return 0;
    }

    /* Find or create a shared pending-waiter entry for this key. */
    list_for_each_entry(w, &kvstore_waiters, list) {
        if (strcmp(w->key, key) == 0) {
            pending = w;
            break;
        }
    }
    if (!pending) {
        pending = kmalloc(sizeof(*pending), GFP_KERNEL);
        if (!pending) {
            mutex_unlock(&kvstore_mutex);
            return -ENOMEM;
        }
        pending->key = kstrdup(key, GFP_KERNEL);
        if (!pending->key) {
            kfree(pending);
            mutex_unlock(&kvstore_mutex);
            return -ENOMEM;
        }
        init_waitqueue_head(&pending->wq);
        pending->refcount = 0;
        pending->ready    = 0;
        list_add(&pending->list, &kvstore_waiters);
    }
    pending->refcount++;
    mutex_unlock(&kvstore_mutex);

    /* Sleep until kv_set() sets pending->ready = 1 for this key. */
    ret = wait_event_interruptible(pending->wq, pending->ready);

    /*
     * Cleanup: runs regardless of ret.  See mutex note in function comment.
     */
    mutex_lock(&kvstore_mutex);
    pending->refcount--;
    if (ret == 0) {
        node = kv_find(key, h);
        if (node)
            strscpy(out, node->value, out_len);
        else
            ret = -ENOENT;  /* key was set then deleted before we re-locked */
    }
    if (pending->refcount == 0) {
        list_del(&pending->list);
        kfree(pending->key);
        kfree(pending);
    }
    mutex_unlock(&kvstore_mutex);
    return ret;
}
