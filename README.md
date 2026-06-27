# Kprobe based Syscall Logger / Blocker

A Linux kernel module that uses **kprobes** (for logging) and **kretprobes** (for blocking) to monitor and control the `open`, `read`, and `write` system calls, plus a userspace utility to control it at runtime via ioctl.

## Features

| Mode      | Description |
|-----------|-------------|
| **off**   | Module is idle — all probe handlers return immediately. |
| **log**   | Logs process name, PID, and syscall arguments whenever the configured target syscall fires. Increments an observation counter for FSM support. |
| **block** | Overrides the return value of the configured target syscall with `-EPERM`, but only for a specific target PID. |

The userspace utility supports a **finite-state machine (FSM)** mode: load a JSON file describing a sequence of syscalls, and the utility will cycle through them, transitioning each time the kernel module observes the current syscall.

---

## Quick Start

### 1. Install prerequisites

```sh
sudo apt update && sudo apt install -y build-essential linux-headers-$(uname -r)
```

### 2. Build everything

```sh
make          # builds kernel module + user utility + writer test program
```

Or build individually:

```sh
make modules  # kernel module only
make user     # userspace utility only
make writer   # test program only
```

### 3. Load the module and create the device node

```sh
sudo insmod kmodule.ko        # or: make load
sudo mknod /dev/kpm_device c 100 0   # or: make device
sudo chmod 666 /dev/kpm_device
```

### 4. Use the utility

```sh
# Enable logging for read syscalls
./user --log --syscall read

# Check kernel logs
sudo dmesg | grep kpm

# Block write syscalls for a specific PID
./user --block --syscall write --pid 12345

# Disable the module
./user --off

# Run FSM from a JSON file (log mode)
./user --log --file sample_fsm.json
```

### 5. Unload the module

```sh
sudo rmmod kmodule             # or: make unload
```

---

## Userspace Utility — Command Reference

```
Usage: ./user [OPTIONS]

Mode (choose one):
  --off                 Disable the module
  --log                 Enable logging mode
  --block               Enable blocking mode

Options:
  --syscall <string>    Set target syscall (open, read, write)
  --pid <pid>           Set target PID (required for --block)
  --file <path.json>    Run FSM from JSON file (log mode only)
  --help                Show help
```

### Examples

```sh
./user --log --syscall read              # log all read() calls
./user --block --syscall write --pid 42  # block write() for PID 42
./user --log --file sample_fsm.json      # run FSM
./user --off                             # disable module
```

---

## FSM Mode

The `--file` flag accepts a JSON file describing a finite-state machine. The format is:

```json
{
    "states": ["open", "read", "write"]
}
```

Each state is a syscall name. The utility:

1. Sets the module to **log** mode.
2. Configures the target syscall to the current state's value.
3. Polls the kernel module's observation counter (via `IOCTL_GET_OBSERVE`).
4. When the counter increments (syscall observed), transitions to the next state.
5. After the last state, loops back to the first.

Press **Ctrl+C** to stop the FSM.

---

## Testing with `writer`

The included `writer.c` program writes `"beep "` to stdout and `output.txt` every 5 seconds. Use it to test blocking:

```sh
# Terminal 1: start the writer
./writer &
echo "Writer PID: $!"

# Terminal 2: block its writes
./user --block --syscall write --pid <PID>

# Terminal 2: check kernel logs
sudo dmesg | tail | grep kpm
```

---

## Project Structure

| File                | Description |
|---------------------|-------------|
| `kmodule.c`         | Kernel module — kprobes for logging, kretprobes for blocking |
| `kpm_ioctl.h`       | Shared header — ioctl commands, mode/syscall constants |
| `user.c`            | Userspace control utility with FSM support |
| `writer.c`          | Test program that periodically calls write() |
| `sample_fsm.json`   | Example FSM definition file |
| `Makefile`          | Build system for kernel module and userspace programs |

---

## Notes

- **Major number**: The module registers character device major `100`. If that conflicts on your system, change `KPM_MAJOR` in `kpm_ioctl.h`.
- **Open symbol**: The module probes `do_sys_openat2` for the open syscall (Linux 5.6+). For older kernels, change `KPM_OPEN_SYMBOL` in `kmodule.c` to `"do_sys_open"`.
- **Architecture**: Syscall argument extraction uses x86_64 register layout (`regs->di`, `regs->si`, `regs->dx`). Adjust for other architectures.
- **Blocking mechanism**: Uses kretprobes to override the return value with `-EPERM`. The function body still executes, but the caller sees an error return.
