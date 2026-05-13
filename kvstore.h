#ifndef KVSTORE_H
#define KVSTORE_H

#include <linux/hashtable.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/types.h>

#define KVSTORE_BITS 6          /* 2^6 = 64 hash buckets */

/* Module parameters (defined in dev.c) */
extern int max_entries;
extern int max_key_len;
extern int max_value_len;

/* Hash table and synchronisation primitives (defined in store.c) */
extern struct hlist_head kv_table[1 << KVSTORE_BITS];
extern struct mutex      kv_mutex;
extern wait_queue_head_t kv_wq;
extern unsigned int      num_entries;

/*
 * Monotonically-increasing generation counter.  Incremented by every
 * kv_set() and kv_del().  Used as a lock-free condition variable in
 * wait_event_interruptible() to avoid calling kv_find() outside the mutex.
 */
extern atomic_t kv_gen;

/* One entry in the hash table. */
struct kv_node {
    char              *key;
    char              *value;
    struct hlist_node  hnode;
};

/* Store life-cycle */
void kv_init(void);
void kv_cleanup(void);

/* Store operations — all block-interruptibly as needed. */
int kv_set(const char *key, const char *value);
int kv_get(const char *key, char *out, size_t out_len);
int kv_del(const char *key);
int kv_wait(const char *key, char *out, size_t out_len);

#endif /* KVSTORE_H */
