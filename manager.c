#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

#define DEVICE_FILE "/proc/monitoring-system"

int main() {
    int fd;
    uint8_t buffer[7];  // ID + Werte + CRC

    buffer[0] = 0xC3; // Sender ID
    buffer[1] = 0x01; // Wert ID 1
    buffer[2] = 1234 & 0xFF;
    buffer[3] = (1234 >> 8) & 0xFF;
    buffer[4] = 0x02; // Wert ID 2
    buffer[5] = 5678 & 0xFF;
    buffer[6] = (5678 >> 8) & 0xFF;

    fd = open(DEVICE_FILE, O_WRONLY);
    if (fd < 0) {
        perror("Fehler beim Öffnen der Datei");
        return 1;
    }

    if (write(fd, buffer, sizeof(buffer)) < 0) {
        perror("Fehler beim Schreiben");
    }

    close(fd);
    return 0;
}