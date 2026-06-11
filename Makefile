# kpm_sys_olb — Kernel Probe Module build system
#
# Targets:
#   make            — build kernel module + userspace programs
#   make modules    — build only the kernel module
#   make user       — build only the user utility
#   make writer     — build only the test writer program
#   make load       — insmod the kernel module
#   make unload     — rmmod the kernel module
#   make device     — create /dev/kpm_device with correct permissions
#   make clean      — remove all build artefacts

obj-m += kmodule.o

KDIR ?= /lib/modules/$(shell uname -r)/build
CFLAGS_USER = -Wall -Wextra -O2

.PHONY: all modules user writer clean load unload device

all: modules user writer

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

user: user.c kpm_ioctl.h
	gcc $(CFLAGS_USER) -o user user.c

writer: writer.c
	gcc $(CFLAGS_USER) -o writer writer.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f user writer

load:
	sudo insmod kmodule.ko

unload:
	sudo rmmod kmodule

device:
	sudo mknod /dev/kpm_device c 100 0 2>/dev/null || true
	sudo chmod 666 /dev/kpm_device
