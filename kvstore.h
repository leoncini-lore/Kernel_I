#ifndef KVSTORE_H
#define KVSTORE_H

#include <linux/hashtable.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/list.h>

#define KVSTORE_BITS 6          /* 2^6 = 64 hash buckets */

/* Module parameters (defined in dev.c); kvstore_ prefix avoids symbol-namespace collision. */
extern int kvstore_max_entries;
extern int kvstore_max_key_len;
extern int kvstore_max_value_len;

/* Hash table and synchronisation primitives (defined in store.c). */
extern struct hlist_head kvstore_table[1 << KVSTORE_BITS];
extern struct mutex      kvstore_mutex;
extern wait_queue_head_t kvstore_wq;
extern unsigned int      kvstore_num_entries;

/*
 * Monotonically-increasing generation counter.  Incremented by every
 * kv_set() and kv_del().  Used as a lock-free condition variable in
 * wait_event_interruptible() so kv_set() capacity waiters detect any change.
 */
extern atomic_t kvstore_gen;

/* One entry in the hash table. */
struct kv_node {
    char              *key;
    char              *value;
    struct hlist_node  hnode;
    /* No speculative fields: add wait-for-update support when actually needed. */
};

/*
 * Per-key pending-waiter entry, used by kv_wait() for keys not yet present.
 *
 * The first kv_wait() caller for a missing key allocates one of these and adds
 * it to kvstore_waiters.  Subsequent callers for the same key increment its
 * refcount and share the same entry.  When kv_set() inserts the key it sets
 * ready = 1 and wakes the entry's wait queue.  Each woken caller decrements
 * refcount; the last one removes the entry from the list and frees it.
 *
 * Protected by kvstore_mutex.
 */
struct kv_waiter {
    char              *key;
    wait_queue_head_t  wq;
    int                refcount;
    int                ready;   /* set to 1 by kv_set() on key insertion */
    struct list_head   list;
};

/* Store life-cycle */
void __init kv_init(void);
void __exit kv_cleanup(void);

/* Store operations — all block-interruptibly as needed. */
int kv_set(const char *key, const char *value);
int kv_get(const char *key, char *out, size_t out_len);
int kv_del(const char *key);
int kv_wait(const char *key, char *out, size_t out_len);

#endif /* KVSTORE_H */
