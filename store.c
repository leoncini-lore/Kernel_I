#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/jhash.h>
#include "kvstore.h"

DEFINE_HASHTABLE(kv_table, KVSTORE_BITS);
struct mutex      kv_mutex;
wait_queue_head_t kv_wq;
unsigned int      num_entries;
atomic_t          kv_gen;

void kv_init(void)
{
    mutex_init(&kv_mutex);
    init_waitqueue_head(&kv_wq);
    hash_init(kv_table);
    num_entries = 0;
    atomic_set(&kv_gen, 0);
}

void kv_cleanup(void)
{
    struct kv_node    *node;
    struct hlist_node *tmp;
    unsigned int       bkt;

    mutex_lock(&kv_mutex);
    hash_for_each_safe(kv_table, bkt, tmp, node, hnode) {
        hash_del(&node->hnode);
        kfree(node->key);
        kfree(node->value);
        kfree(node);
    }
    num_entries = 0;
    mutex_unlock(&kv_mutex);
}

/* Caller must hold kv_mutex. */
static struct kv_node *kv_find(const char *key)
{
    struct kv_node *node;
    u32 h = jhash(key, strlen(key), 0);

    hash_for_each_possible(kv_table, node, hnode, h) {
        if (strcmp(node->key, key) == 0)
            return node;
    }
    return NULL;
}

/*
 * Insert or update <key, value>.  Blocks if the table is full and the key
 * does not already exist, waiting until a kv_del() creates a free slot.
 */
int kv_set(const char *key, const char *value)
{
    struct kv_node *node;
    char           *new_val;
    u32             h;
    unsigned int    gen;
    int             ret;

retry:
    ret = mutex_lock_interruptible(&kv_mutex);
    if (ret)
        return ret;

    node = kv_find(key);
    if (node) {
        /* Update: always allowed regardless of capacity. */
        new_val = kstrdup(value, GFP_KERNEL);
        if (!new_val) {
            mutex_unlock(&kv_mutex);
            return -ENOMEM;
        }
        kfree(node->value);
        node->value = new_val;
        atomic_inc(&kv_gen);
        mutex_unlock(&kv_mutex);
        wake_up_interruptible(&kv_wq);
        return 0;
    }

    if (num_entries >= (unsigned int)max_entries) {
        /*
         * Table full.  Snapshot the generation counter before releasing
         * the lock, then sleep until any modification occurs (guaranteed
         * to be noticed because we read gen while still locked).
         */
        gen = (unsigned int)atomic_read(&kv_gen);
        mutex_unlock(&kv_mutex);
        ret = wait_event_interruptible(kv_wq,
                (unsigned int)atomic_read(&kv_gen) != gen);
        if (ret)
            return ret;
        goto retry;
    }

    /* Insert new entry. */
    node = kmalloc(sizeof(*node), GFP_KERNEL);
    if (!node) {
        mutex_unlock(&kv_mutex);
        return -ENOMEM;
    }
    node->key = kstrdup(key, GFP_KERNEL);
    if (!node->key) {
        kfree(node);
        mutex_unlock(&kv_mutex);
        return -ENOMEM;
    }
    node->value = kstrdup(value, GFP_KERNEL);
    if (!node->value) {
        kfree(node->key);
        kfree(node);
        mutex_unlock(&kv_mutex);
        return -ENOMEM;
    }
    h = jhash(key, strlen(key), 0);
    hash_add(kv_table, &node->hnode, h);
    num_entries++;
    atomic_inc(&kv_gen);
    mutex_unlock(&kv_mutex);
    wake_up_interruptible(&kv_wq);
    return 0;
}

/* Copy the value for <key> into out[0..out_len).  Returns -ENOENT if missing. */
int kv_get(const char *key, char *out, size_t out_len)
{
    struct kv_node *node;
    int ret;

    ret = mutex_lock_interruptible(&kv_mutex);
    if (ret)
        return ret;

    node = kv_find(key);
    if (!node) {
        mutex_unlock(&kv_mutex);
        return -ENOENT;
    }

    strncpy(out, node->value, out_len - 1);
    out[out_len - 1] = '\0';
    mutex_unlock(&kv_mutex);
    return 0;
}

/* Delete <key>.  Returns -ENOENT if the key does not exist. */
int kv_del(const char *key)
{
    struct kv_node *node;
    int ret;

    ret = mutex_lock_interruptible(&kv_mutex);
    if (ret)
        return ret;

    node = kv_find(key);
    if (!node) {
        mutex_unlock(&kv_mutex);
        return -ENOENT;
    }

    hash_del(&node->hnode);
    num_entries--;
    kfree(node->key);
    kfree(node->value);
    kfree(node);
    atomic_inc(&kv_gen);
    mutex_unlock(&kv_mutex);
    wake_up_interruptible(&kv_wq);
    return 0;
}

/*
 * Block until <key> is present in the table, then copy its value into
 * out[0..out_len).  Returns -EINTR/-ERESTARTSYS if interrupted by a signal.
 *
 * Design note: we snapshot kv_gen while holding the mutex before sleeping.
 * This guarantees we do not miss a SET that happened between our "key not
 * found" check and the sleep: if such a SET occurred, kv_gen has already
 * changed and wait_event_interruptible returns immediately.
 */
int kv_wait(const char *key, char *out, size_t out_len)
{
    struct kv_node *node;
    unsigned int    gen;
    int             ret;

    while (1) {
        ret = mutex_lock_interruptible(&kv_mutex);
        if (ret)
            return ret;

        node = kv_find(key);
        if (node) {
            strncpy(out, node->value, out_len - 1);
            out[out_len - 1] = '\0';
            mutex_unlock(&kv_mutex);
            return 0;
        }

        gen = (unsigned int)atomic_read(&kv_gen);
        mutex_unlock(&kv_mutex);

        ret = wait_event_interruptible(kv_wq,
                (unsigned int)atomic_read(&kv_gen) != gen);
        if (ret)
            return ret;
        /* Re-check: the change might have been a DEL on a different key. */
    }
}
