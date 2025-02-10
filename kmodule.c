#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/time.h>
#include <linux/string.h>

// this module...
// blocks any process from making 'write' syscalls
// logs any read sys call 
// does nothing too

#define DEVICE_NAME "kp_device"
#define MAJOR_NUM 100

#define IOCTL_SET_PID _IOW(MAJOR_NUM, 0, pid_t)
#define IOCTL_ENABLE_LOG _IO(MAJOR_NUM, 1)
#define IOCTL_DISABLE_MODULE _IO(MAJOR_NUM, 2)

static struct kprobe kp_write;
static struct kprobe kp_read;
static struct kprobe kp_idle;

static pid_t target_pid = 0; // Default to no PID set
static bool log_enabled = false; // Flag to enable logging

// Returns time in string
const char *get_time_string(void)
{
    static char time_str[9]; // Static buffer: "HH:MM:SS" (8 chars + null terminator)
    struct timespec64 ts;
    struct tm tm;
    // Get current real time
    ktime_get_real_ts64(&ts);
    time64_to_tm(ts.tv_sec, 0, &tm);
    
    // Format time as HH:MM:SS
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return time_str; // Return the static buffer
}


int handler_idle(struct kprobe *p, struct pt_regs *regs)
{
    // No operation, effectively keeping the device idle
    return 0;
}

int last_pid = -1;  // To store the PID of the last logged process
int last_syscall = -1;  // To store the last syscall ID (for example, sys_open)

/* This function will be called when sys_read is triggered */
int handler_read(struct kprobe *p, struct pt_regs *regs)
{
    if(log_enabled){
        struct task_struct *task = current;
        int current_pid = task->pid;
        if (current_pid != last_pid || last_syscall != 1){ 
            printk(KERN_INFO "Process %s (PID %d) called sys_read at %s.\n", task->comm, task->pid, get_time_string());
            last_pid = current_pid;  // Update the last logged PID
            last_syscall = 1;
            return 0;
        }
        return 0;
    }
    else
        return 0;
}

/* This function will be called when vfs_read block is triggered */
static int pre_handler(struct kprobe *p, struct pt_regs *regs)
{
    pid_t pid = current->pid;

    if (pid == target_pid) {
        printk(KERN_INFO "Blocking write() for process with PID %d\n", pid);
        regs->di = -EPERM;  // Set return value to -EPERM
        return 0;  // Block the syscall
    }

    return 0;  // Allow the syscall to proceed for other processes
}

static long kp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
        case IOCTL_SET_PID:
            if (copy_from_user(&target_pid, (pid_t *)arg, sizeof(pid_t))) {
                return -EFAULT;
            }
            printk(KERN_INFO "Target PID set to %d\n", target_pid);
            return 0;
        case IOCTL_ENABLE_LOG:
            log_enabled = true;
            printk(KERN_INFO "Logging processes using read syscall enabled\n");
            return 0;
        case IOCTL_DISABLE_MODULE:
            log_enabled = false;
            printk(KERN_INFO "Logging processes using read syscall disabled\n");
            return 0;
        default:
            return -EINVAL;
    }
}

static struct file_operations fops = {
    .unlocked_ioctl = kp_ioctl,
};

static int __init kprobe_init(void)
{
    int ret;
    ret = register_chrdev(MAJOR_NUM, DEVICE_NAME, &fops);
    if (ret < 0) {
        printk(KERN_ERR "Failed to register character device\n");
        return ret;
    }

    //Init read trigger for --log
    kp_read.symbol_name = "vfs_read"; //Attach to vfs_read function
    kp_read.pre_handler = handler_read;
    ret = register_kprobe(&kp_read);
    if (ret < 0) {
        printk(KERN_ERR "Registering kprobe for sys_read failed\n");
        unregister_kprobe(&kp_read);
        return ret;
    }
    printk(KERN_INFO "Kprobe for idle handler read registered.\n");
    
    //Init idle trigger for --off
    kp_idle.symbol_name = "vfs_read";  // You can change this to any function you're targeting
    kp_idle.pre_handler = handler_idle;
    // Register the kprobe
    ret = register_kprobe(&kp_idle);
    if (ret < 0) {
        printk(KERN_ERR "Registering kprobe failed\n");
        return ret;
    }
    printk(KERN_INFO "Kprobe for idle handler idle registered.\n");
    

    //Init write trigger for --block
    kp_write.symbol_name = "vfs_write";  // Attach to vfs_write function
    kp_write.pre_handler = pre_handler; // Define pre-handler
    // Register the kprobe
    if (register_kprobe(&kp_write) < 0) {
        printk(KERN_ERR "Failed to register kprobe\n");
        unregister_chrdev(MAJOR_NUM, DEVICE_NAME);
        return -1;
    }
    printk(KERN_INFO "Kprobe registered to block write() for PID %d\n", target_pid);
    return 0;
}

static void __exit kprobe_exit(void)
{
    unregister_kprobe(&kp_write);
    unregister_kprobe(&kp_read);
    unregister_kprobe(&kp_idle);
  
    unregister_chrdev(MAJOR_NUM, DEVICE_NAME);
    printk(KERN_INFO "Kprobe unregistered\n");
}

module_init(kprobe_init);
module_exit(kprobe_exit);

MODULE_LICENSE("GPL");
