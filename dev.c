#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include "kvstore.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lorenzo Leoncini");
MODULE_DESCRIPTION("Bounded in-memory key/value store kernel module");
MODULE_VERSION("1.0");

/* Fix 4: kvstore_ prefix on module parameters to avoid symbol-namespace pollution */
int kvstore_max_entries  = 64;
int kvstore_max_key_len  = 32;
int kvstore_max_value_len = 128;

module_param(kvstore_max_entries,   int, 0444);
MODULE_PARM_DESC(kvstore_max_entries,   "Max number of key/value pairs (default 64)");
module_param(kvstore_max_key_len,   int, 0444);
MODULE_PARM_DESC(kvstore_max_key_len,   "Max key length in bytes (default 32)");
module_param(kvstore_max_value_len, int, 0444);
MODULE_PARM_DESC(kvstore_max_value_len, "Max value length in bytes (default 128)");

/* ------------------------------------------------------------------ */
/* Per-open-file state                                                  */
/* ------------------------------------------------------------------ */

/*
 * Fix 6 — response-buffer overwrite semantics:
 *
 * Each open file descriptor has exactly one response buffer.  Issuing a
 * second command that produces a response (GET, WAIT) before read()ing the
 * first response silently discards the earlier response; set_response()
 * calls kfree() on the old buffer before installing the new one.
 *
 * read() returns 0 (EOF) immediately when no response is pending.
 */
struct kvstore_file {
    char   *resp;       /* Response buffer populated by GET / WAIT    */
    size_t  resp_len;   /* Bytes of valid data in resp                 */
    size_t  resp_off;   /* Bytes already returned to user space        */
};

static int kvstore_open(struct inode *inode, struct file *filp)
{
    struct kvstore_file *kf = kzalloc(sizeof(*kf), GFP_KERNEL);

    if (!kf)
        return -ENOMEM;
    filp->private_data = kf;
    return 0;
}

static int kvstore_release(struct inode *inode, struct file *filp)
{
    struct kvstore_file *kf = filp->private_data;

    kfree(kf->resp);
    kfree(kf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* read() — deliver the pending GET / WAIT response to user space      */
/* ------------------------------------------------------------------ */

static ssize_t kvstore_read(struct file *filp, char __user *ubuf,
                            size_t count, loff_t *ppos)
{
    struct kvstore_file *kf = filp->private_data;
    size_t avail;
    ssize_t n;

    if (!kf->resp)
        return 0;   /* Nothing pending — signal EOF to the caller. */

    avail = kf->resp_len - kf->resp_off;
    if (avail == 0) {
        /* All bytes were already delivered; clear and report EOF. */
        kfree(kf->resp);
        kf->resp     = NULL;
        kf->resp_len = 0;
        kf->resp_off = 0;
        return 0;
    }

    n = (ssize_t)min(count, avail);
    if (copy_to_user(ubuf, kf->resp + kf->resp_off, (unsigned long)n))
        return -EFAULT;

    kf->resp_off += (size_t)n;
    return n;
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Store a response string (value + newline) in the per-file buffer.
 * Overwrites any previous unread response — see struct kvstore_file comment.
 */
static int set_response(struct kvstore_file *kf, const char *val)
{
    size_t len = strlen(val);
    char  *s   = kmalloc(len + 2, GFP_KERNEL); /* val + '\n' + NUL */

    if (!s)
        return -ENOMEM;

    memcpy(s, val, len);
    s[len]     = '\n';
    s[len + 1] = '\0';

    kfree(kf->resp);
    kf->resp     = s;
    kf->resp_len = len + 1; /* exclude NUL from the readable byte count */
    kf->resp_off = 0;
    return 0;
}

/*
 * Parse and execute one command line already in kernel memory.
 * Supported commands:
 *   SET <key> <value>
 *   GET <key>
 *   DEL <key>
 *   WAIT <key>
 *
 * Fix 3 — tokenisation:
 *   Tokens are separated by runs of spaces and/or tabs.  Leading whitespace
 *   is stripped before parsing.  strsep() splits on any single space or tab;
 *   we manually skip runs of whitespace between tokens to handle multiple
 *   consecutive delimiters (e.g. "SET  foo bar" is valid).
 *
 *   Value capture: everything after the second whitespace run is taken as the
 *   value, preserving embedded spaces (e.g. "SET key hello world" stores the
 *   value "hello world").  Empty keys and empty verbs are rejected (-EINVAL).
 *
 * Returns 0 on success, negative errno on error.
 * For GET and WAIT the result is stored in kf->resp for the next read().
 */
static int handle_cmd(struct kvstore_file *kf, char *line)
{
    char *verb, *key, *value, *p;
    char *valbuf;
    int   ret;

    /* Strip trailing whitespace and newlines. */
    p = line + strlen(line);
    while (p > line && (*(p-1) == '\n' || *(p-1) == '\r' ||
                        *(p-1) == ' '  || *(p-1) == '\t'))
        *(--p) = '\0';

    /* Strip leading whitespace (Fix 3). */
    while (*line == ' ' || *line == '\t')
        line++;

    /*
     * Tokenise: strsep splits on one delimiter character; we skip runs of
     * whitespace between tokens manually so consecutive spaces/tabs work.
     */
    p    = line;
    verb = strsep(&p, " \t");
    while (p && (*p == ' ' || *p == '\t'))
        p++;

    key = strsep(&p, " \t");
    while (p && (*p == ' ' || *p == '\t'))
        p++;

    /* Capture the rest of the line as value (preserves embedded spaces). */
    value = (p && *p) ? p : NULL;

    /* Reject empty verb or key (Fix 3). */
    if (!verb || verb[0] == '\0')
        return -EINVAL;
    if (key && key[0] == '\0')
        key = NULL;

    /* Validate lengths before touching the store. */
    if (key && strlen(key) > (size_t)kvstore_max_key_len)
        return -EINVAL;
    if (value && strlen(value) > (size_t)kvstore_max_value_len)
        return -EINVAL;

    if (strcmp(verb, "SET") == 0) {
        if (!key || !value)
            return -EINVAL;
        return kv_set(key, value);

    } else if (strcmp(verb, "GET") == 0) {
        if (!key)
            return -EINVAL;
        valbuf = kmalloc((size_t)kvstore_max_value_len + 1, GFP_KERNEL);
        if (!valbuf)
            return -ENOMEM;
        ret = kv_get(key, valbuf, (size_t)kvstore_max_value_len + 1);
        if (ret == -ENOENT)
            ret = set_response(kf, "(not found)");
        else if (ret == 0)
            ret = set_response(kf, valbuf);
        kfree(valbuf);
        return ret;

    } else if (strcmp(verb, "DEL") == 0) {
        if (!key)
            return -EINVAL;
        return kv_del(key);

    } else if (strcmp(verb, "WAIT") == 0) {
        if (!key)
            return -EINVAL;
        valbuf = kmalloc((size_t)kvstore_max_value_len + 1, GFP_KERNEL);
        if (!valbuf)
            return -ENOMEM;
        ret = kv_wait(key, valbuf, (size_t)kvstore_max_value_len + 1);
        if (ret == 0)
            ret = set_response(kf, valbuf);
        kfree(valbuf);
        return ret;

    } else {
        return -EINVAL;
    }
}

/* ------------------------------------------------------------------ */
/* write() — accept one command per write() call                       */
/* ------------------------------------------------------------------ */

static ssize_t kvstore_write(struct file *filp, const char __user *ubuf,
                             size_t count, loff_t *ppos)
{
    struct kvstore_file *kf = filp->private_data;
    /*
     * Fix 5 — correct size bound:
     *   <verb (≤4 chars)> ' ' <key (≤max_key_len)> ' ' <value (≤max_value_len)>
     *   '\n' '\0'
     * "WAIT" is the longest verb (4 chars + 1 space = 5 prefix bytes).
     * SET is the longest command overall because it carries both key and value.
     */
    size_t max_cmd = (size_t)(5 + kvstore_max_key_len + 1 + kvstore_max_value_len + 2);
    char  *kbuf;
    int    ret;

    if (count == 0)
        return 0;
    if (count > max_cmd)
        return -EINVAL;

    kbuf = kmalloc(count + 1, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, ubuf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }
    kbuf[count] = '\0';

    ret = handle_cmd(kf, kbuf);
    kfree(kbuf);

    /* Return the original byte count on success so callers do not retry. */
    return ret < 0 ? (ssize_t)ret : (ssize_t)count;
}

/* ------------------------------------------------------------------ */
/* Device registration and module life-cycle                           */
/* ------------------------------------------------------------------ */

static const struct file_operations kvstore_fops = {
    .owner   = THIS_MODULE,
    .open    = kvstore_open,
    .release = kvstore_release,
    .read    = kvstore_read,
    .write   = kvstore_write,
};

static struct miscdevice kvstore_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "kvstore",
    .fops  = &kvstore_fops,
};

static int __init kvstore_init(void)
{
    int ret;

    kv_init();

    ret = misc_register(&kvstore_miscdev);
    if (ret) {
        pr_err("kvstore: misc_register failed (%d)\n", ret);
        return ret;
    }

    pr_info("kvstore: loaded (max_entries=%d, max_key_len=%d, max_value_len=%d)\n",
            kvstore_max_entries, kvstore_max_key_len, kvstore_max_value_len);
    return 0;
}

static void __exit kvstore_exit(void)
{
    /*
     * Shutdown order (Issue 1 from prior review, comment refined by Issue 2):
     *
     * 1. Deregister the device: prevents new open() calls and new write()
     *    commands on existing fds.
     *
     * 2. Wake kvstore_wq: this queue is used exclusively by kv_set() callers
     *    blocked in the table-full capacity-wait path.  Those callers do not
     *    hold a dedicated wait entry — they sleep on the global kvstore_wq —
     *    so waking it here lets them re-check kvstore_gen and return cleanly
     *    with -ERESTARTSYS before the table is torn down.
     *
     *    Note: kv_wait() callers sleep on per-key kv_waiter.wq entries, NOT
     *    on kvstore_wq, so this wake does NOT reach them.  No separate wake is
     *    needed for per-key entries: THIS_MODULE refcounting guarantees no
     *    kv_wait() caller can be sleeping at this point (see kv_cleanup()).
     *
     *    In practice both kv_set() and kv_wait() callers hold an open fd while
     *    sleeping, and fops.owner = THIS_MODULE plus the file_operations refcount
     *    mean rmmod cannot succeed while any fd is open.  Therefore no sleeper
     *    can be present at this point.  The wake is harmless and matches the
     *    "wake before tear down" defensive pattern.
     *
     * 3. Tear down the hash table only after all sleepers have been woken.
     *
     * Previous (wrong) order: misc_deregister → kv_cleanup → wake_up.
     * A task woken after kv_cleanup would dereference freed kv_node memory.
     */
    misc_deregister(&kvstore_miscdev);
    wake_up_interruptible(&kvstore_wq);
    kv_cleanup();
    pr_info("kvstore: unloaded\n");
}

module_init(kvstore_init);
module_exit(kvstore_exit);
