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

int max_entries  = 64;
int max_key_len  = 32;
int max_value_len = 128;

module_param(max_entries,   int, 0444);
MODULE_PARM_DESC(max_entries,   "Max number of key/value pairs (default 64)");
module_param(max_key_len,   int, 0444);
MODULE_PARM_DESC(max_key_len,   "Max key length in bytes (default 32)");
module_param(max_value_len, int, 0444);
MODULE_PARM_DESC(max_value_len, "Max value length in bytes (default 128)");

/* ------------------------------------------------------------------ */
/* Per-open-file state                                                  */
/* ------------------------------------------------------------------ */

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

/* Store a response string (value + newline) in the per-file buffer. */
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
    while (p > line && (*(p - 1) == '\n' || *(p - 1) == '\r' || *(p - 1) == ' '))
        *(--p) = '\0';

    /* Split into verb, key, value. */
    verb = line;
    p = strchr(line, ' ');
    if (p) {
        *p = '\0';
        key = p + 1;
    } else {
        key = NULL;
    }

    value = NULL;
    if (key) {
        p = strchr(key, ' ');
        if (p) {
            *p = '\0';
            value = p + 1;
        }
    }

    /* Validate lengths before touching the store. */
    if (key && strlen(key) > (size_t)max_key_len)
        return -EINVAL;
    if (value && strlen(value) > (size_t)max_value_len)
        return -EINVAL;

    if (strcmp(verb, "SET") == 0) {
        if (!key || !value)
            return -EINVAL;
        return kv_set(key, value);

    } else if (strcmp(verb, "GET") == 0) {
        if (!key)
            return -EINVAL;
        valbuf = kmalloc((size_t)max_value_len + 1, GFP_KERNEL);
        if (!valbuf)
            return -ENOMEM;
        ret = kv_get(key, valbuf, (size_t)max_value_len + 1);
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
        valbuf = kmalloc((size_t)max_value_len + 1, GFP_KERNEL);
        if (!valbuf)
            return -ENOMEM;
        ret = kv_wait(key, valbuf, (size_t)max_value_len + 1);
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
    /* "SET " (4) + key + " " (1) + value + "\n" (1) */
    size_t max_cmd = (size_t)(4 + 1 + max_key_len + 1 + max_value_len + 2);
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
            max_entries, max_key_len, max_value_len);
    return 0;
}

static void __exit kvstore_exit(void)
{
    misc_deregister(&kvstore_miscdev);
    kv_cleanup();
    /*
     * Wake any processes still blocked in kv_wait() or kv_set().
     * They will receive -EINTR / -ERESTARTSYS and should exit cleanly.
     * NOTE: ensure no processes are blocked before running rmmod.
     */
    wake_up_interruptible(&kv_wq);
    pr_info("kvstore: unloaded\n");
}

module_init(kvstore_init);
module_exit(kvstore_exit);
