#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/jhash.h>
#include <linux/list.h>
#include "kvstore.h"

/* Store globals — kvstore_ prefix keeps them out of the flat kernel symbol namespace. */
DEFINE_HASHTABLE(kvstore_table, KVSTORE_BITS);
struct mutex      kvstore_mutex;
wait_queue_head_t kvstore_wq;
unsigned int      kvstore_num_entries;
atomic_t          kvstore_gen;

/*
 * List of kv_waiter entries for keys not yet present in the table.
 * Protected by kvstore_mutex.  Static — only store.c touches this list.
 */
static LIST_HEAD(kvstore_waiters);

/* Initialise store globals.  Called once from kvstore_init() before device registration. */
void __init kv_init(void)
{
    mutex_init(&kvstore_mutex);
    init_waitqueue_head(&kvstore_wq);
    hash_init(kvstore_table);
    kvstore_num_entries = 0;
    atomic_set(&kvstore_gen, 0);
}

/* Free all hash-table entries.  Called from kvstore_exit() after device deregistration. */
void __exit kv_cleanup(void)
{
    struct kv_node    *node;
    struct hlist_node *tmp;
    unsigned int       bkt;

    mutex_lock(&kvstore_mutex);

    /*
     * No wake of kvstore_waiters needed here.  A kv_wait() caller can only
     * sleep while holding an open file descriptor, and fops.owner = THIS_MODULE
     * prevents module unload while any fd is open — so kvstore_waiters is
     * always empty at this point.  A "defensive" wake would be actively unsafe:
     * woken waiters would block on kvstore_mutex (still held here), then
     * acquire it only after kv_cleanup returns and module memory is freed,
     * causing use-after-free on the mutex and on any kv_waiter entries.
     */

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
 * On new-key insertion, only the kv_waiter entry for this exact key is woken
 * (O(waiters-for-key) rather than O(all-waiters)).  Allocation failures unwind
 * via labelled goto.
 */
int kv_set(const char *key, const char *value)
{
    struct kv_node   *node;
    struct kv_waiter *w;
    char             *new_key, *new_val;
    u32               h;
    unsigned int      gen;
    int               ret;

    /* Self-enforcing API contract: handle_cmd validates too, but direct callers
     * (future ioctl, kernel-internal) must not bypass the check at this layer. */
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
        /* Update path: existing-key updates are not a kv_wait trigger; no per-key wake. */
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

    /* Insert new entry; goto labels unwind allocations on failure. */
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
     * Wake only the pending-waiter entry for this exact key.  WRITE_ONCE
     * on ready before wake_up_interruptible provides the ordering guarantee
     * that wait_event_interruptible needs to observe the updated condition.
     *
     * list_for_each_entry (non-_safe) is safe here: we only set ready and
     * wake; removal happens in kv_wait's cleanup path, never in this loop.
     * If a future change removes entries inside this loop, switch to
     * list_for_each_entry_safe with a temporary cursor.
     */
    list_for_each_entry(w, &kvstore_waiters, list) {
        if (strcmp(w->key, key) == 0) {
            WRITE_ONCE(w->ready, 1);
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

    /* Self-enforcing API contract; see kv_set() for rationale. */
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

    /* Self-enforcing API contract; see kv_set() for rationale. */
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
 * Loop-back on SET-then-DEL race:
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
 * Forward-progress note:
 *   Under adversarial workloads (rapid SET/DEL churn on the same key, where
 *   every kv_set wake is followed by a kv_del before this caller can acquire
 *   the mutex), this loop can iterate without making observable progress.
 *   The only termination guarantee in that case is signal delivery: a signal
 *   to the waiting task causes wait_event_interruptible to return
 *   -ERESTARTSYS, the loop exits via goto cleanup, and the caller can retry
 *   or give up.  This matches the semantics of the original implementation
 *   and is acceptable for a blocking wait primitive — userspace owns its
 *   own scheduling fairness.
 *
 * Per-key wait queues:
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

    /* Self-enforcing API contract; see kv_set() for rationale. */
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
        WRITE_ONCE(pending->ready, 0);
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
        WRITE_ONCE(pending->ready, 0);
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
