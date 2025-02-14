/*
sysmond.c

Implementiert ein erweiterbares Monitoring System für verschiedene Messwerte und agiert nach der Initialisierung der GPIOs und dem Device Tree als Daemon im Hintergrund.
Für jeden erhobenen Messwert wird ein Kindprozess vom Prozess abgespalten und zwei Pipes für bidirektionale Kommunikation erzeugt.
Die Kindprozesse erheben ihre Messwerte und warten dann auf ein Signal des Elternprozesses um ihre Messdaten an diesen zu übermitteln.
Der Elternprozess schreibt die Daten dann in das für die Kommunikation zwischen Userspace und Kernel genutzte Proc-File unter /proc/monitoring-system

Alle Funktionen wurden von uns beiden (Leya Wehner und Julian Frank) in Kollaboration geschreiben.
Die Kommentare zur Funktionsbeschreibung befinden sich über der jeweiligen Funktion.
*/

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
#include <sched.h>
#include <time.h>

#define NUM_JOBS 3
#define NUM_SENDERS 4
#define TIME_PER_JOB 25
#define BUFFER_SIZE 1024
#define DEVICE_FILE "/proc/monitoring-system"

//  Array das die Befehle zum auslesen der jeweiligen Messwerte enthält. Die von den Befehlen zurückgegebenen Werte müssen in einen uint16_t konvertierbar sein.
const char *jobs[NUM_JOBS] = {
    "top -bn1 | grep \"Cpu(s)\" | sed \"s/.*, *\\([0-9.]*\\)%* id.*/\\1/\" | awk '{printf \"%dn\", 100 - $1}'", // CPU Nutzung (in Prozent)
    "cat /sys/class/thermal/thermal_zone0/temp", // CPU Temperatur (in Miligrad Celsius)
    "free | grep Mem | awk '{printf \"%dn\", $3 / $2 * 100}'", // Speichernutzung (in Prozent)
};

/*
    Enthält die Logik für die Kindprozesse, welche die Messwerte kontinuierlich auslesen und anschließend an den Parent zurücksenden.
    Die Kommunikation erfolgt hierbei über jeweils 2 Pipes um bidirektionale Kommunikation zu ermöglichen.
*/
void child_process(int write_fd, int read_fd, const char *command) {
    struct sched_param param;
    param.sched_priority = 98;

    // Setzt den Kindprozess auf Realtime Priorität 98
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        perror("sched_setscheduler failed");
        exit(EXIT_FAILURE);
    }

    char buffer[BUFFER_SIZE];
    uint8_t signal;
    uint16_t val;
    FILE *fp;

    while (1) {
        signal = 0;

        fp = popen(command, "r");
        if (fp == NULL) {
            val = 0;
        }
        else {
            fgets(buffer, BUFFER_SIZE, fp);
            val = atoi(buffer);
        }

        ssize_t bytes_read = read(read_fd, &signal, sizeof(signal));
        if (bytes_read > 0 && signal == 1) {
            write(write_fd, &val, sizeof(val));
        }
        
        pclose(fp);
    }
}

/*
    Enthält die Logik für den Elternprozess, welcher die Werte die von den Kindprozessen ausgelesen und über die Pipes geschickt wurden aggregiert und an das Kernelmodul übermittelt.
*/
int parent_process(int message_pipes[][2], int signal_pipes[][2]) {
    struct sched_param param;
    param.sched_priority = 99;  // Highest real-time priority

    // Setzt den Kindprozess auf Realtime Priorität 99
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        perror("sched_setscheduler failed");
        return EXIT_FAILURE;
    }

    int fd;

    fd = open(DEVICE_FILE, O_WRONLY);
    if (fd < 0) {
        perror("Error opening the file");
        exit(EXIT_FAILURE);
    }

    uint8_t buffer[1 + NUM_JOBS + 2 * NUM_JOBS];
    uint16_t val;
    uint8_t one = 1;

    buffer[0] = 0xC2; // Sender ID
    
    sleep(1);

    struct timespec next;

    while (1) {
        int index = 1;

        clock_gettime(CLOCK_MONOTONIC, &next);
        next.tv_nsec += (NUM_JOBS * TIME_PER_JOB * NUM_SENDERS) * 1e6;
        while (next.tv_nsec >= 1e9) {
            next.tv_sec++;
            next.tv_nsec -= 1e9;
        }

        for (int i = 0; i < NUM_JOBS; i++) {
            write(signal_pipes[i][1], &one, sizeof(one));
        }

        for (int i = 0; i < NUM_JOBS; i++) {
            ssize_t bytes_read = read(message_pipes[i][0], &val, sizeof(val));
            if (bytes_read > 0) {
                buffer[index] = i; // Wert ID
                buffer[index + 1] = val & 0xFF; // Wert LSB
                buffer[index + 2] = (val >> 8) & 0xFF; // Wert MSB
            }
            index += 3;
        }

        if (write(fd, buffer, sizeof(buffer)) < 0) {
            perror("Error writing to the file");
        }

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    close(fd);
    return 0;
}

/* 
    Die Funktion dient als Einstiegspunkt für das Programm und spaltet vom Elternprozess für jeden überwachten Wert jeweils einen Kindprozess ab.
*/
int main() {
    int message_pipes[NUM_JOBS][2];
    int signal_pipes[NUM_JOBS][2];

    pid_t pids[NUM_JOBS];

    for (int i = 0; i < NUM_JOBS; i++) {
        if (pipe(message_pipes[i]) == -1) {
            perror("pipe failed");
            exit(EXIT_FAILURE);
        }

        if (pipe(signal_pipes[i]) == -1) {
            perror("pipe failed");
            exit(EXIT_FAILURE);
        }

        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork failed");
            exit(EXIT_FAILURE);
        }

        if (pids[i] == 0) {  // Kindprozess
            close(message_pipes[i][0]);
            close(signal_pipes[i][1]);
            child_process(message_pipes[i][1], signal_pipes[i][0], jobs[i]);
            exit(EXIT_SUCCESS);
        }

        close(message_pipes[i][1]);
        close(signal_pipes[i][0]);
    }

    return parent_process(message_pipes, signal_pipes);
}
