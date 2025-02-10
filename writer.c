#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

// test program that 'beeps' every 5 seconds

int main() {
    const char *message = "beep ";

    // Open the file for writing (create it if it doesn't exist)
    int fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        perror("Error opening file");
        return 1;
    }

    // Infinite loop to write every 5 seconds
    while (1) {
        // Write to standard output (stdout)
        write(STDOUT_FILENO, message, sizeof(message) - 1);

        // Write to the file
        write(fd, message, sizeof(message) - 1);

        // Sleep for 5 seconds before the next iteration
        sleep(5);
    }

    // Close the file descriptor (this line will never be reached due to the infinite loop)
    close(fd);

    return 0;
}
