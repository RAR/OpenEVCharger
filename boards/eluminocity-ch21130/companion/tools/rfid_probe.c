/* rfid_probe.c — minimal /dev/ttyAMA4 test/snoop tool.
 *
 * Three modes:
 *   poll  — open UART, send polls, dump RX. Use when no other daemon is on
 *           the UART (we own the reader).
 *   read  — open UART read-only, just dump RX. Use ALONGSIDE stock to race
 *           bytes (proves reader is responding, even if stock wins most).
 *   send  — write a single poll, exit. For deliberate transmission.
 *
 * Build: docker run --rm -v "$PWD:/work" -w /work
 *        muslcc/x86_64:armv5l-linux-musleabi
 *        sh -c 'cc -Wall -Wextra -O2 -static -o tools/rfid_probe tools/rfid_probe.c'
 */
#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int open_serial(const char *port)
{
    int fd = open(port, O_RDWR);
    if (fd < 0) { fprintf(stderr, "open(%s): %s\n", port, strerror(errno)); return -1; }
    struct termios t;
    memset(&t, 0, sizeof t);
    t.c_cflag = B115200 | CS8 | CLOCAL | CREAD;
    t.c_iflag = IGNPAR;
    t.c_oflag = 0;
    t.c_lflag = 0;
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 5;          /* 500 ms read timeout */
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
        close(fd); return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "poll";
    const char *port = (argc > 2) ? argv[2] : "/dev/ttyAMA4";

    int fd = open_serial(port);
    if (fd < 0) return 1;

    unsigned char poll[4] = { 0x03, 0x20, 0x00, 0x23 };
    unsigned char buf[128];
    struct timespec ts;

    if (!strcmp(mode, "send")) {
        ssize_t w = write(fd, poll, sizeof poll);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        fprintf(stderr, "[%ld.%06ld] send: w=%zd\n", ts.tv_sec, ts.tv_nsec/1000, w);
        ssize_t r = read(fd, buf, sizeof buf);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        fprintf(stderr, "[%ld.%06ld] read: r=%zd:", ts.tv_sec, ts.tv_nsec/1000, r);
        for (ssize_t k = 0; k < r; k++) fprintf(stderr, " %02x", buf[k]);
        fprintf(stderr, "\n");
    } else if (!strcmp(mode, "read")) {
        fprintf(stderr, "read-only snoop on %s for 30 seconds\n", port);
        time_t deadline = time(NULL) + 30;
        while (time(NULL) < deadline) {
            ssize_t r = read(fd, buf, sizeof buf);
            if (r > 0) {
                clock_gettime(CLOCK_MONOTONIC, &ts);
                fprintf(stderr, "[%ld.%06ld] r=%zd:", ts.tv_sec, ts.tv_nsec/1000, r);
                for (ssize_t k = 0; k < r; k++) fprintf(stderr, " %02x", buf[k]);
                fprintf(stderr, "\n");
            }
        }
    } else {                /* poll mode */
        fprintf(stderr, "20 polls on %s, 200 ms apart, 500 ms read timeout\n", port);
        for (int i = 0; i < 20; i++) {
            ssize_t w = write(fd, poll, sizeof poll);
            ssize_t r = read(fd, buf, sizeof buf);
            printf("[%2d] tx=%zd rx=%zd:", i, w, r);
            for (ssize_t k = 0; k < r; k++) printf(" %02x", buf[k]);
            printf("\n");
            fflush(stdout);
            usleep(200000);
        }
    }
    close(fd);
    return 0;
}
