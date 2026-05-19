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
    struct kv_waiter  *w;
    unsigned int       bkt;

    mutex_lock(&kvstore_mutex);

    /*
     * Issue 2 — defensive wake of per-key waiters:
     * Module unload while a kv_wait() caller is sleeping is prevented in
     * practice by THIS_MODULE refcounting (an open fd keeps the module
     * pinned).  This loop is purely defensive: if that invariant ever breaks,
     * sleeping callers are woken so they re-check and unwind cleanly rather
     * than touching freed memory.  We do NOT free the entries here — the
     * woken kv_wait() callers own their refcount decrements and will free
     * entries themselves in their cleanup path.
     */
    list_for_each_entry(w, &kvstore_waiters, list) {
        w->ready = 1;
        wake_up_interruptible(&w->wq);
    }

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
 * Per-key wake (Issue 2): on new-key insertion, wakes only kv_waiter entries
 * registered for this exact key (O(waiters-for-key) instead of O(all-waiters)).
 *
 * Goto unwind (Issue 7 from prior review): allocation failures use labelled
 * goto instead of nested if blocks.
 */
int kv_set(const char *key, const char *value)
{
    struct kv_node   *node;
    struct kv_waiter *w;
    char             *new_key, *new_val;
    u32               h;
    unsigned int      gen;
    int               ret;

    /* Issue 4: self-enforcing API contract; handle_cmd already validates,
     * but direct callers (future ioctl, kernel-internal use) must not bypass. */
    if (!key || key[0] == '\0' || !value)
        return -EINVAL;
    if (strlen(key) > (size_t)kvstore_max_key_len)
        return -ENAMETOOLONG;
    if (strlen(value) > (size_t)kvstore_max_value_len)
        return -EMSGSIZE;

    h = jhash(key, strlen(key), 0);

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
    hash_add(kvstore_table, &node->hnode, h);
    kvstore_num_entries++;
    atomic_inc(&kvstore_gen);

    /*
     * Wake only the pending-waiter entry for this exact key.  Setting
     * ready = 1 before wake_up ensures the condition is visible to
     * wait_event_interruptible before it re-checks.
     *
     * Issue 5: safe to use list_for_each_entry (non-_safe variant) here
     * because we only set ready and wake; entry removal happens in kv_wait's
     * cleanup path, never inside this loop.  If a future change ever removes
     * an entry inside this loop, switch to list_for_each_entry_safe and add
     * a temporary cursor.
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
    u32 h;
    int ret;

    /* Issue 4: self-enforcing API contract. */
    if (!key || key[0] == '\0')
        return -EINVAL;
    if (strlen(key) > (size_t)kvstore_max_key_len)
        return -ENAMETOOLONG;

    h = jhash(key, strlen(key), 0);
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
    u32 h;
    int ret;

    /* Issue 4: self-enforcing API contract. */
    if (!key || key[0] == '\0')
        return -EINVAL;
    if (strlen(key) > (size_t)kvstore_max_key_len)
        return -ENAMETOOLONG;

    h = jhash(key, strlen(key), 0);
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
 * Block until <key> is present in the table and its value has been
 * successfully observed by this caller, then copy it into out[0..out_len).
 * Returns 0 on success, -EINTR/-ERESTARTSYS if interrupted by a signal.
 *
 * Issue 1 — loop-back on SET-then-DEL race:
 *   The caller's contract is "block until the key is present and give me its
 *   value."  If the key is inserted and then deleted before this caller can
 *   observe it (i.e., kv_find returns NULL after the wake), we re-sleep rather
 *   than returning -ENOENT.  Returning -ENOENT from a function whose entire
 *   purpose is "wait for existence" would break the contract.
 *
 *   Before re-sleeping, pending->ready is reset to 0 under the mutex so that
 *   wait_event_interruptible does not spin: it checks the condition before
 *   sleeping, so a stale ready == 1 would cause an immediate spurious return.
 *
 *   Race on reset: between setting ready = 0 and re-entering
 *   wait_event_interruptible, another kv_set() may fire and set ready = 1
 *   again.  That is safe — wait_event_interruptible checks the condition
 *   before sleeping and returns immediately if it is already true.
 *
 * Per-key wait queues (from prior review):
 *   Each unique missing key gets one kv_waiter entry in kvstore_waiters.
 *   Multiple callers for the same key share it via refcount.  kv_set() wakes
 *   only the matching entry, eliminating the O(N*M) thundering-herd of a
 *   global wait queue.
 *
 * Mutex note: mutex_lock() (not mutex_lock_interruptible) is used in the
 * post-sleep path.  Once wait_event_interruptible returns for any reason, the
 * caller MUST decrement refcount (and potentially free the entry).  Using the
 * interruptible variant here could leak the entry on a second signal.  The
 * lock is held only briefly in this bounded cleanup section.
 */
int kv_wait(const char *key, char *out, size_t out_len)
{
    struct kv_node   *node;
    struct kv_waiter *w, *pending = NULL;
    u32               h;
    int               ret;

    /* Issue 4: self-enforcing API contract. */
    if (!key || key[0] == '\0')
        return -EINVAL;
    if (strlen(key) > (size_t)kvstore_max_key_len)
        return -ENAMETOOLONG;

    h = jhash(key, strlen(key), 0);

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

    for (;;) {
        ret = wait_event_interruptible(pending->wq, pending->ready);

        /* Must hold mutex for both the value read and the refcount update. */
        mutex_lock(&kvstore_mutex);

        if (ret != 0)
            goto cleanup;   /* signal: unwind and return the error */

        node = kv_find(key, h);
        if (node) {
            strscpy(out, node->value, out_len);
            goto cleanup;   /* success */
        }

        /*
         * SET-then-DEL race: key was inserted then removed before we
         * re-acquired the mutex.  Reset ready so wait_event_interruptible
         * sleeps on the next iteration rather than returning immediately.
         * See function comment for the concurrent-kv_set race analysis.
         */
        pending->ready = 0;
        mutex_unlock(&kvstore_mutex);
    }

cleanup:
    pending->refcount--;
    if (pending->refcount == 0) {
        list_del(&pending->list);
        kfree(pending->key);
        kfree(pending);
    }
    mutex_unlock(&kvstore_mutex);
    return ret;
}
