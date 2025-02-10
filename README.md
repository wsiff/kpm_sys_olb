# Kernel Module Setup Guide

## Update Your System
```sh
sudo apt update && sudo apt upgrade -y
```

## Install Essential Packages
```sh
sudo apt install -y build-essential linux-headers-$(uname -r)
```

## Install the Kernel Module
```sh
make
```

## Create the Device File
```sh
sudo mknod kp_device c 100 0  # Replace major_number with 100
sudo chmod 666 kp_device
```

## Compile and Run the Utility Program
```sh
gcc -o user user.c
```

## Create Device Node
```sh
sudo mknod /dev/kp_device c 100 0
sudo chmod 666 /dev/kp_device
```

## Compile the Test Program
```sh
gcc -o writer writer.c
```

## Fetch PID of the Test Program
Use the following command to find the PID of the test program:
```sh
ps aux | grep writer
```

Before proceeding, run the `write` syscall on a different terminal:
```sh
./writer
```

## Insert the Kernel Module
```sh
sudo insmod kmodule.ko
```

## Use the Utility Program
```sh
./user --block [pid]
./user --log
./user --off
```

## Check Logs
```sh
tail log.txt
```

## Remove the Kernel Module
```sh
sudo rmmod kmodule
```

---

### Notes
- It's generally a good practice to let Linux decide the major number, but for this setup, we manually assign `100`.
- Ensure that the device file and permissions are properly set before running the utility program.
- Running `./writer` in a different terminal helps confirm the blocking behavior.


