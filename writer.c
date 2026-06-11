#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

/*
 * writer.c — Simple test program that writes "beep " to stdout and
 * a file every 5 seconds.  Use this to verify that the kernel module
 * can log or block write syscalls for a running process.
 *
 * Usage:
 *   ./writer &          # start in background
 *   echo $!             # note the PID
 *   ./user --block --syscall write --pid <PID>
 */

int main(void)
{
	const char *message = "beep ";
	size_t len = strlen(message);

	/* Open (or create) output file */
	int fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd == -1) {
		perror("Error opening file");
		return 1;
	}

	/* Write every 5 seconds until killed */
	while (1) {
		write(STDOUT_FILENO, message, len);
		write(fd, message, len);
		sleep(5);
	}

	close(fd);
	return 0;
}
