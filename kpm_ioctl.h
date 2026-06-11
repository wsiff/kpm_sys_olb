/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kpm_ioctl.h — Shared definitions for the KPM kernel module and
 *               its userspace control utility.
 *
 * Include this header in both kmodule.c and user.c so that ioctl
 * command numbers, mode constants, and device metadata stay in sync.
 */
#ifndef KPM_IOCTL_H
#define KPM_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

/* ── Device ────────────────────────────────────────────────────── */
#define KPM_DEVICE_NAME  "kpm_device"
#define KPM_DEVICE_PATH  "/dev/" KPM_DEVICE_NAME
#define KPM_MAJOR        100

/* ── Operating modes ───────────────────────────────────────────── */
#define KPM_MODE_OFF     0   /* Module disabled, no-op            */
#define KPM_MODE_LOG     1   /* Log matching syscalls via printk  */
#define KPM_MODE_BLOCK   2   /* Block a syscall for a target PID  */

/* ── Target syscall identifiers ────────────────────────────────── */
#define KPM_SYSCALL_OPEN  0
#define KPM_SYSCALL_READ  1
#define KPM_SYSCALL_WRITE 2

/* ── IOCTL commands ────────────────────────────────────────────── */
#define KPM_IOCTL_SET_MODE      _IOW(KPM_MAJOR, 0, int)
#define KPM_IOCTL_SET_SYSCALL   _IOW(KPM_MAJOR, 1, int)
#define KPM_IOCTL_SET_PID       _IOW(KPM_MAJOR, 2, int)
#define KPM_IOCTL_GET_OBSERVE   _IOR(KPM_MAJOR, 3, int)
#define KPM_IOCTL_RESET_OBSERVE _IO(KPM_MAJOR, 4)

#endif /* KPM_IOCTL_H */
