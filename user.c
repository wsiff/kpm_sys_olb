#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>

// user space handler file to control the kernel module at runtime

#define DEVICE_PATH "kp_device"
#define IOCTL_SET_PID _IOW(100, 0, int)
#define IOCTL_ENABLE_LOG _IO(100, 1)
#define IOCTL_DISABLE_MODULE _IO(100, 2)

#define LOG_FILE "log.txt"

int main(int argc, char *argv[])
{
    void print_usage(const char *progname) {
        printf("Usage: %s --block [pid] | --log | --off\n", progname);
    } 
    int fd;
    int pid;
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    // Open the device
    fd = open(DEVICE_PATH, O_RDWR);
    if (fd == -1) {
        perror("Failed to open device");
        return 1;
    }
    
    if (strcmp(argv[1], "--block") == 0) { 
        if (argc != 3) {
            print_usage(argv[0]);
            close(fd);
            return EXIT_FAILURE;
        }
        pid = atoi(argv[2]);
        if (ioctl(fd, IOCTL_SET_PID, &pid) < 0) { // takes an extra argument as PID(int)
            perror("Failed to block PID");
            close(fd);
            return EXIT_FAILURE;
        }
        printf("Blocked PID: %d\n", pid);
    } else if (strcmp(argv[1], "--log") == 0) {
        if (ioctl(fd, IOCTL_ENABLE_LOG) < 0) {
            perror("Failed to enable logging");
            close(fd);
            return EXIT_FAILURE;
        }
        printf("Logging enabled\n");
    } else if (strcmp(argv[1], "--off") == 0) {
        if (ioctl(fd, IOCTL_DISABLE_MODULE) < 0) {
            perror("Failed to disable module");
            close(fd);
            return EXIT_FAILURE;
        }
        printf("Module off.\n");
    } else {
        print_usage(argv[0]);
        close(fd);
        return EXIT_FAILURE;
    }

    // Run the dmesg command and save the output to log.txt
    if (system("sudo dmesg > log.txt") < 0) {
        perror("Failed to run dmesg");
        close(fd);
        return EXIT_FAILURE;
    }
    
    close(fd);
    return EXIT_SUCCESS;
}
