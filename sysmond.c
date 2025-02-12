#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <sys/select.h>
#include <stdint.h>
#include <fcntl.h>

#define NUM_JOBS 3
#define BUFFER_SIZE 1024
#define DEVICE_FILE "/proc/monitoring-system"


const char *jobs[NUM_JOBS] = {
    "top -bn1 | grep \"Cpu(s)\" | sed \"s/.*, *\\([0-9.]*\\)%* id.*/\\1/\" | awk '{printf \"%dn\", 100 - $1}'", // CPU usage
    "cat /sys/class/thermal/thermal_zone0/temp", // CPU temperature
    "free | grep Mem | awk '{printf \"%dn\", $3 / $2 * 100}'" // Memory usage
};

void child_process(int write_fd, const char *command) {
    char buffer[BUFFER_SIZE];
    FILE *fp;

    // Continuous execution loop
    while (1) {
        // Execute command and open pipe to read its output
        fp = popen(command, "r");
        if (fp == NULL) {
            write(write_fd, 0, sizeof(uint32_t));
        }

        // Read command output and send to parent
        fgets(buffer, BUFFER_SIZE, fp);
        u_int32_t val = atoi(buffer);
        write(write_fd, &val, sizeof(val));

        pclose(fp);
        sleep(1); // Delay for demonstration; adjust as needed
    }
}

int main() {
    int pipes[NUM_JOBS][2];
    pid_t pids[NUM_JOBS];

    for (int i = 0; i < NUM_JOBS; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe failed");
            exit(EXIT_FAILURE);
        }

        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork failed");
            exit(EXIT_FAILURE);
        }

        if (pids[i] == 0) {  // Child process
            child_process(pipes[i][1], jobs[i]);
            exit(EXIT_SUCCESS);
        }
    }

    // Parent collects output
    int fd;

    fd = open(DEVICE_FILE, O_WRONLY);
    if (fd < 0) {
        perror("Fehler beim Öffnen der Datei");
        exit(EXIT_FAILURE);
    }

    uint8_t buffer[1 + NUM_JOBS + 2 * NUM_JOBS];
    uint32_t val;

    buffer[0] = 0xC2; // Sender ID

    while (1) {
        // Read from ready pipes
        int index = 1;
        for (int i = 0; i < NUM_JOBS; i++) {
            ssize_t bytes_read = read(pipes[i][0], &val, sizeof(val));
            if (bytes_read > 0) {
                buffer[index] = i + 1; // Wert ID
                buffer[index + 1] = val & 0xFF; // Wert LSB
                buffer[index + 2] = (val >> 8) & 0xFF; // Wert MSB
            }
            index += 3;
        }

        if (write(fd, buffer, sizeof(buffer)) < 0) {
            perror("Fehler beim Schreiben");
        }
        sleep(1);
    }

    // Should not reach here as children run infinitely
    close(fd);
    return 0;
}
