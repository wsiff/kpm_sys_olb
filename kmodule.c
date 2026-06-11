// SPDX-License-Identifier: GPL-2.0
/*
 * kmodule.c — Kprobe-based syscall logger / blocker kernel module.
 *
 * Three operating modes controlled at runtime via ioctl:
 *
 *   OFF   – all probe handlers return immediately (no-op).
 *   LOG   – logs process info + syscall arguments whenever the
 *           configured target syscall (open/read/write) fires.
 *           Increments an observation counter readable from
 *           userspace so the FSM can detect transitions.
 *   BLOCK – uses kretprobes to override the return value of the
 *           configured target syscall with -EPERM, but only for
 *           the configured target PID.
 *
 * Probed functions (x86_64):
 *   open  → do_sys_openat2   (kernel 5.6+; change the define below
 *                              to "do_sys_open" for older kernels)
 *   read  → vfs_read
 *   write → vfs_write
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/version.h>

#include "kpm_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kpm_sys_olb");
MODULE_DESCRIPTION("Kprobe-based syscall logger/blocker");

/*
 * The open entry-point symbol varies by kernel version:
 *   5.6+  → do_sys_openat2
 *   4.x   → do_sys_open
 * Adjust if registration fails on your kernel.
 */
#define KPM_OPEN_SYMBOL "do_sys_openat2"

/* ================================================================
 * Module state — atomics for lock-free access from probe context
 * ================================================================ */
static atomic_t kpm_mode     = ATOMIC_INIT(KPM_MODE_OFF);
static atomic_t kpm_syscall  = ATOMIC_INIT(KPM_SYSCALL_READ);
static atomic_t kpm_pid      = ATOMIC_INIT(0);
static atomic_t kpm_observed = ATOMIC_INIT(0);

/* ================================================================
 * LOGGING — kprobe pre-handlers
 *
 * x86_64 calling convention (System V ABI):
 *   arg1 = RDI  (regs->di)
 *   arg2 = RSI  (regs->si)
 *   arg3 = RDX  (regs->dx)
 *   arg4 = RCX  (regs->cx)
 *
 * vfs_read / vfs_write(struct file *, char __user *buf, size_t cnt, loff_t *pos)
 * do_sys_openat2(int dfd, const char __user *filename, struct open_how *how)
 * ================================================================ */

static int log_open_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct task_struct *task;
	char filename[256];
	long copied;

	if (atomic_read(&kpm_mode) != KPM_MODE_LOG)
		return 0;
	if (atomic_read(&kpm_syscall) != KPM_SYSCALL_OPEN)
		return 0;

	task = current;
	copied = strncpy_from_user(filename,
				   (const char __user *)regs->si,
				   sizeof(filename));
	if (copied < 0)
		snprintf(filename, sizeof(filename), "(inaccessible)");

	printk(KERN_INFO "[kpm] LOG: process=%s pid=%d syscall=open dfd=%d filename=%s\n",
	       task->comm, task->pid, (int)regs->di, filename);
	atomic_inc(&kpm_observed);
	return 0;
}

static int log_read_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct task_struct *task;

	if (atomic_read(&kpm_mode) != KPM_MODE_LOG)
		return 0;
	if (atomic_read(&kpm_syscall) != KPM_SYSCALL_READ)
		return 0;

	task = current;
	printk(KERN_INFO "[kpm] LOG: process=%s pid=%d syscall=read file_ptr=0x%lx buf=0x%lx count=%lu\n",
	       task->comm, task->pid,
	       (unsigned long)regs->di,
	       (unsigned long)regs->si,
	       (unsigned long)regs->dx);
	atomic_inc(&kpm_observed);
	return 0;
}

static int log_write_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct task_struct *task;

	if (atomic_read(&kpm_mode) != KPM_MODE_LOG)
		return 0;
	if (atomic_read(&kpm_syscall) != KPM_SYSCALL_WRITE)
		return 0;

	task = current;
	printk(KERN_INFO "[kpm] LOG: process=%s pid=%d syscall=write file_ptr=0x%lx buf=0x%lx count=%lu\n",
	       task->comm, task->pid,
	       (unsigned long)regs->di,
	       (unsigned long)regs->si,
	       (unsigned long)regs->dx);
	atomic_inc(&kpm_observed);
	return 0;
}

/* Logging kprobe structs */
static struct kprobe kp_open = {
	.symbol_name = KPM_OPEN_SYMBOL,
	.pre_handler = log_open_pre,
};
static struct kprobe kp_read = {
	.symbol_name = "vfs_read",
	.pre_handler = log_read_pre,
};
static struct kprobe kp_write = {
	.symbol_name = "vfs_write",
	.pre_handler = log_write_pre,
};

/* ================================================================
 * BLOCKING — kretprobes
 *
 * entry_handler checks mode / syscall / pid and marks whether we
 * should block.  Returning non-zero from entry_handler skips the
 * ret_handler entirely (fast path for non-matching calls).
 *
 * ret_handler overrides regs->ax (RAX = return value on x86_64)
 * with -EPERM so the caller sees an error.
 * ================================================================ */

struct block_data {
	int should_block;
};

/* --- open -------------------------------------------------------- */
static int block_open_entry(struct kretprobe_instance *ri,
			    struct pt_regs *regs)
{
	struct block_data *d = (struct block_data *)ri->data;

	d->should_block = 0;
	if (atomic_read(&kpm_mode) != KPM_MODE_BLOCK)
		return 1;
	if (atomic_read(&kpm_syscall) != KPM_SYSCALL_OPEN)
		return 1;
	if (current->pid != (pid_t)atomic_read(&kpm_pid))
		return 1;

	d->should_block = 1;
	return 0;
}

static int block_open_ret(struct kretprobe_instance *ri,
			  struct pt_regs *regs)
{
	struct block_data *d = (struct block_data *)ri->data;

	if (d->should_block) {
		regs->ax = (unsigned long)(-EPERM);
		printk(KERN_INFO "[kpm] BLOCK: blocked open for pid=%d\n",
		       current->pid);
	}
	return 0;
}

/* --- read -------------------------------------------------------- */
static int block_read_entry(struct kretprobe_instance *ri,
			    struct pt_regs *regs)
{
	struct block_data *d = (struct block_data *)ri->data;

	d->should_block = 0;
	if (atomic_read(&kpm_mode) != KPM_MODE_BLOCK)
		return 1;
	if (atomic_read(&kpm_syscall) != KPM_SYSCALL_READ)
		return 1;
	if (current->pid != (pid_t)atomic_read(&kpm_pid))
		return 1;

	d->should_block = 1;
	return 0;
}

static int block_read_ret(struct kretprobe_instance *ri,
			  struct pt_regs *regs)
{
	struct block_data *d = (struct block_data *)ri->data;

	if (d->should_block) {
		regs->ax = (unsigned long)(-EPERM);
		printk(KERN_INFO "[kpm] BLOCK: blocked read for pid=%d\n",
		       current->pid);
	}
	return 0;
}

/* --- write ------------------------------------------------------- */
static int block_write_entry(struct kretprobe_instance *ri,
			     struct pt_regs *regs)
{
	struct block_data *d = (struct block_data *)ri->data;

	d->should_block = 0;
	if (atomic_read(&kpm_mode) != KPM_MODE_BLOCK)
		return 1;
	if (atomic_read(&kpm_syscall) != KPM_SYSCALL_WRITE)
		return 1;
	if (current->pid != (pid_t)atomic_read(&kpm_pid))
		return 1;

	d->should_block = 1;
	return 0;
}

static int block_write_ret(struct kretprobe_instance *ri,
			   struct pt_regs *regs)
{
	struct block_data *d = (struct block_data *)ri->data;

	if (d->should_block) {
		regs->ax = (unsigned long)(-EPERM);
		printk(KERN_INFO "[kpm] BLOCK: blocked write for pid=%d\n",
		       current->pid);
	}
	return 0;
}

/* Blocking kretprobe structs */
static struct kretprobe krp_open = {
	.kp.symbol_name = KPM_OPEN_SYMBOL,
	.handler        = block_open_ret,
	.entry_handler  = block_open_entry,
	.data_size      = sizeof(struct block_data),
	.maxactive      = 20,
};
static struct kretprobe krp_read = {
	.kp.symbol_name = "vfs_read",
	.handler        = block_read_ret,
	.entry_handler  = block_read_entry,
	.data_size      = sizeof(struct block_data),
	.maxactive      = 20,
};
static struct kretprobe krp_write = {
	.kp.symbol_name = "vfs_write",
	.handler        = block_write_ret,
	.entry_handler  = block_write_entry,
	.data_size      = sizeof(struct block_data),
	.maxactive      = 20,
};

/* ================================================================
 * IOCTL — character device interface
 * ================================================================ */

static long kpm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int val;

	switch (cmd) {
	case KPM_IOCTL_SET_MODE:
		if (copy_from_user(&val, (int __user *)arg, sizeof(int)))
			return -EFAULT;
		if (val < KPM_MODE_OFF || val > KPM_MODE_BLOCK)
			return -EINVAL;
		atomic_set(&kpm_mode, val);
		printk(KERN_INFO "[kpm] Mode set to %s (%d)\n",
		       val == KPM_MODE_OFF  ? "off"   :
		       val == KPM_MODE_LOG  ? "log"   : "block", val);
		return 0;

	case KPM_IOCTL_SET_SYSCALL:
		if (copy_from_user(&val, (int __user *)arg, sizeof(int)))
			return -EFAULT;
		if (val < KPM_SYSCALL_OPEN || val > KPM_SYSCALL_WRITE)
			return -EINVAL;
		atomic_set(&kpm_syscall, val);
		printk(KERN_INFO "[kpm] Target syscall set to %s (%d)\n",
		       val == KPM_SYSCALL_OPEN  ? "open"  :
		       val == KPM_SYSCALL_READ  ? "read"  : "write", val);
		return 0;

	case KPM_IOCTL_SET_PID:
		if (copy_from_user(&val, (int __user *)arg, sizeof(int)))
			return -EFAULT;
		atomic_set(&kpm_pid, val);
		printk(KERN_INFO "[kpm] Target PID set to %d\n", val);
		return 0;

	case KPM_IOCTL_GET_OBSERVE:
		val = atomic_read(&kpm_observed);
		if (copy_to_user((int __user *)arg, &val, sizeof(int)))
			return -EFAULT;
		return 0;

	case KPM_IOCTL_RESET_OBSERVE:
		atomic_set(&kpm_observed, 0);
		return 0;

	default:
		return -EINVAL;
	}
}

static const struct file_operations kpm_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = kpm_ioctl,
};

/* ================================================================
 * MODULE INIT / EXIT
 * ================================================================ */

static int __init kpm_init(void)
{
	int ret;

	/* Character device for ioctl control */
	ret = register_chrdev(KPM_MAJOR, KPM_DEVICE_NAME, &kpm_fops);
	if (ret < 0) {
		printk(KERN_ERR "[kpm] Failed to register chrdev (major=%d): %d\n",
		       KPM_MAJOR, ret);
		return ret;
	}

	/* Logging kprobes */
	ret = register_kprobe(&kp_open);
	if (ret < 0) {
		printk(KERN_ERR "[kpm] kprobe open (%s) failed: %d\n",
		       KPM_OPEN_SYMBOL, ret);
		goto err_chrdev;
	}

	ret = register_kprobe(&kp_read);
	if (ret < 0) {
		printk(KERN_ERR "[kpm] kprobe read (vfs_read) failed: %d\n", ret);
		goto err_kp_open;
	}

	ret = register_kprobe(&kp_write);
	if (ret < 0) {
		printk(KERN_ERR "[kpm] kprobe write (vfs_write) failed: %d\n", ret);
		goto err_kp_read;
	}

	/* Blocking kretprobes */
	ret = register_kretprobe(&krp_open);
	if (ret < 0) {
		printk(KERN_ERR "[kpm] kretprobe open (%s) failed: %d\n",
		       KPM_OPEN_SYMBOL, ret);
		goto err_kp_write;
	}

	ret = register_kretprobe(&krp_read);
	if (ret < 0) {
		printk(KERN_ERR "[kpm] kretprobe read (vfs_read) failed: %d\n", ret);
		goto err_krp_open;
	}

	ret = register_kretprobe(&krp_write);
	if (ret < 0) {
		printk(KERN_ERR "[kpm] kretprobe write (vfs_write) failed: %d\n", ret);
		goto err_krp_read;
	}

	printk(KERN_INFO "[kpm] Module loaded – 3 kprobes + 3 kretprobes registered\n");
	return 0;

err_krp_read:
	unregister_kretprobe(&krp_read);
err_krp_open:
	unregister_kretprobe(&krp_open);
err_kp_write:
	unregister_kprobe(&kp_write);
err_kp_read:
	unregister_kprobe(&kp_read);
err_kp_open:
	unregister_kprobe(&kp_open);
err_chrdev:
	unregister_chrdev(KPM_MAJOR, KPM_DEVICE_NAME);
	return ret;
}

static void __exit kpm_exit(void)
{
	unregister_kretprobe(&krp_write);
	unregister_kretprobe(&krp_read);
	unregister_kretprobe(&krp_open);
	unregister_kprobe(&kp_write);
	unregister_kprobe(&kp_read);
	unregister_kprobe(&kp_open);
	unregister_chrdev(KPM_MAJOR, KPM_DEVICE_NAME);
	printk(KERN_INFO "[kpm] Module unloaded\n");
}

module_init(kpm_init);
module_exit(kpm_exit);
