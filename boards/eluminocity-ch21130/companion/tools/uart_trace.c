/* uart_trace.c — LD_PRELOAD shim for tracing stock /root/RFID UART traffic.
 *
 * Wraps open / openat / read / write / close / ioctl with direct-syscall
 * tee-loggers so we can run the stock binary (or any other glibc-linked
 * Delta daemon) on this unit and capture exactly what it sends and receives
 * over /dev/ttyAMA* without modifying it.
 *
 * Target glibc is 2.10.2 on the unit. We build with the musl armv5l cross
 * toolchain — so to avoid an ABI mismatch we issue raw Linux syscalls via
 * inline asm and link with -nostdlib. No libc dependency at all.
 *
 * Log format (one line per event, written to TRACE_LOG):
 *
 *   [s.us] tag fd=N ret=R "args"   bytes...
 *
 * where bytes are pipe-delimited hex pairs after "<<" (read) or ">>"
 * (write). Lines longer than LINE_MAX are truncated with a "...trunc" tail.
 *
 * Usage on target:
 *
 *   killall RFID 2>/dev/null
 *   rm -f /tmp/uart-trace.log
 *   LD_PRELOAD=/tmp/uart_trace.so /root/RFID &
 *   # ...exercise the reader (hold a card)...
 *   killall RFID
 *   hexdump -C /tmp/uart-trace.log
 *
 * Build (host):
 *
 *   docker run --rm -v "$PWD:/work" -w /work \
 *     muslcc/x86_64:armv5l-linux-musleabi \
 *     sh -c 'cc -shared -fPIC -nostdlib -Wl,-soname,uart_trace.so \
 *            -o uart_trace.so tools/uart_trace.c'
 *
 * Notes:
 *   - Errno is not set on syscall failure. Daemon will see raw -EFOO
 *     return values. Acceptable for observation runs.
 *   - We log ALL fds, not just ttyAMA*. Grep the resulting log for the
 *     fd returned by the open("/dev/ttyAMA4") line to isolate UART traffic.
 *   - Recursion is avoided because we never call libc — we only invoke
 *     syscalls, which the kernel doesn't route back through our shim.
 */

typedef unsigned int   size_t;
typedef          int   ssize_t;
typedef unsigned int   uint32_t;
typedef          int   int32_t;
typedef unsigned long  uint64_t;  /* ARM ILP32: long is 32-bit, but we only use 64-bit for timeval scratch */
typedef          long  off_t;

/* ARM EABI Linux syscall numbers (asm/unistd-eabi.h). */
#define SYS_read          3
#define SYS_write         4
#define SYS_open          5
#define SYS_close         6
#define SYS_ioctl        54
#define SYS_openat      322
#define SYS_clock_gettime 263

#define O_WRONLY 0x0001
#define O_CREAT  0x0040
#define O_APPEND 0x0400
#define CLOCK_MONOTONIC 1

#define LINE_MAX  512
#define BYTES_MAX 64        /* max bytes inlined per read/write log line */

#define TRACE_LOG "/tmp/uart-trace.log"

static int g_log_fd = -1;    /* lazy-opened on first event */
static int g_log_init = 0;

/* -------- raw syscall wrappers (ARM EABI). -------- */

static inline long _syscall1(long nr, long a)
{
    register long r0 __asm__("r0") = a;
    register long r7 __asm__("r7") = nr;
    __asm__ volatile("swi #0" : "+r"(r0) : "r"(r7) : "memory");
    return r0;
}
static inline long _syscall2(long nr, long a, long b)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r7 __asm__("r7") = nr;
    __asm__ volatile("swi #0" : "+r"(r0) : "r"(r1), "r"(r7) : "memory");
    return r0;
}
static inline long _syscall3(long nr, long a, long b, long c)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r7 __asm__("r7") = nr;
    __asm__ volatile("swi #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}
static inline long _syscall4(long nr, long a, long b, long c, long d)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r7 __asm__("r7") = nr;
    __asm__ volatile("swi #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r7) : "memory");
    return r0;
}

static long sys_read (int fd, void *buf, size_t n)   { return _syscall3(SYS_read,  fd, (long)buf, n); }
static long sys_write(int fd, const void *buf, size_t n) { return _syscall3(SYS_write, fd, (long)buf, n); }
static long sys_open (const char *p, int flags, int mode) { return _syscall3(SYS_open,  (long)p, flags, mode); }
static long sys_close(int fd)                        { return _syscall1(SYS_close, fd); }
static long sys_ioctl(int fd, unsigned long req, long arg) { return _syscall3(SYS_ioctl, fd, req, arg); }
static long sys_openat(int dirfd, const char *p, int flags, int mode) { return _syscall4(SYS_openat, dirfd, (long)p, flags, mode); }

struct __ts { long sec; long nsec; };
static long sys_clock_gettime(int clk, struct __ts *t) { return _syscall2(SYS_clock_gettime, clk, (long)t); }

/* -------- minimal mem/str helpers. -------- */

static size_t my_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int my_strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i]; if (!a[i]) return 0; }
    return 0;
}

static char *fmt_dec(char *p, long v)
{
    char tmp[24]; int i = 0; int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { tmp[i++] = '0'; }
    while (v) { tmp[i++] = '0' + (v % 10); v /= 10; }
    if (neg) *p++ = '-';
    while (i--) *p++ = tmp[i];
    return p;
}
static char *fmt_hex2(char *p, unsigned char v)
{
    static const char hex[] = "0123456789abcdef";
    *p++ = hex[v >> 4]; *p++ = hex[v & 0xf]; return p;
}
static char *fmt_str(char *p, char *end, const char *s)
{
    while (*s && p < end) *p++ = *s++;
    return p;
}

/* -------- lazy log init. Called from every override. -------- */

static void log_init(void)
{
    if (g_log_init) return;
    g_log_init = 1;
    g_log_fd = (int)sys_open(TRACE_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    /* if open fails (g_log_fd < 0), all subsequent log_emit() are no-ops. */
}

static void log_emit(char *line, char *p)
{
    log_init();
    if (g_log_fd < 0) return;
    *p++ = '\n';
    sys_write(g_log_fd, line, p - line);
}

/* Emit log line header: "[s.us] tag fd=N ret=R ". Returns write-ptr. */
static char *log_hdr(char *line, char *end, const char *tag, int fd, long ret)
{
    struct __ts t = {0,0};
    sys_clock_gettime(CLOCK_MONOTONIC, &t);
    char *p = line;
    *p++ = '[';
    p = fmt_dec(p, t.sec);
    *p++ = '.';
    /* us, zero-padded to 6 */
    long us = t.nsec / 1000;
    char tmp[6]; int i;
    for (i = 5; i >= 0; i--) { tmp[i] = '0' + (us % 10); us /= 10; }
    for (i = 0; i < 6; i++) *p++ = tmp[i];
    *p++ = ']'; *p++ = ' ';
    p = fmt_str(p, end, tag);
    *p++ = ' ';
    p = fmt_str(p, end, "fd=");
    p = fmt_dec(p, fd);
    *p++ = ' ';
    p = fmt_str(p, end, "ret=");
    p = fmt_dec(p, ret);
    *p++ = ' ';
    return p;
}

/* -------- public LD_PRELOAD overrides. -------- */

int open(const char *pathname, int flags, ...)
{
    int mode = 0;
    /* Pull mode off the stack only if O_CREAT is set. The variadic arg is
     * passed in r2 per ARM AAPCS, but since we're declaring with ... we
     * pull it via __builtin_va_*. */
    if (flags & O_CREAT) {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }
    long ret = sys_open(pathname, flags, mode);

    char line[LINE_MAX]; char *end = line + LINE_MAX - 32;
    char *p = log_hdr(line, end, "OPEN", -1, ret);
    *p++ = '"'; p = fmt_str(p, end, pathname); *p++ = '"';
    p = fmt_str(p, end, " flags=0x");
    p = fmt_hex2(p, (unsigned char)((flags >> 8) & 0xff));
    p = fmt_hex2(p, (unsigned char)(flags & 0xff));
    log_emit(line, p);
    return (int)ret;
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }
    long ret = sys_openat(dirfd, pathname, flags, mode);

    char line[LINE_MAX]; char *end = line + LINE_MAX - 32;
    char *p = log_hdr(line, end, "OPENAT", dirfd, ret);
    *p++ = '"'; p = fmt_str(p, end, pathname); *p++ = '"';
    log_emit(line, p);
    return (int)ret;
}

ssize_t read(int fd, void *buf, size_t count)
{
    long ret = sys_read(fd, buf, count);

    char line[LINE_MAX]; char *end = line + LINE_MAX - 32;
    char *p = log_hdr(line, end, "READ", fd, ret);
    p = fmt_str(p, end, "n="); p = fmt_dec(p, (long)count);
    if (ret > 0) {
        p = fmt_str(p, end, " <<");
        long m = ret < BYTES_MAX ? ret : BYTES_MAX;
        for (long i = 0; i < m; i++) {
            *p++ = ' ';
            p = fmt_hex2(p, ((unsigned char *)buf)[i]);
        }
        if (ret > BYTES_MAX) p = fmt_str(p, end, " ...");
    }
    log_emit(line, p);
    return (ssize_t)ret;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    long ret = sys_write(fd, buf, count);

    char line[LINE_MAX]; char *end = line + LINE_MAX - 32;
    char *p = log_hdr(line, end, "WRITE", fd, ret);
    p = fmt_str(p, end, "n="); p = fmt_dec(p, (long)count);
    p = fmt_str(p, end, " >>");
    long m = count < BYTES_MAX ? (long)count : BYTES_MAX;
    for (long i = 0; i < m; i++) {
        *p++ = ' ';
        p = fmt_hex2(p, ((unsigned char *)buf)[i]);
    }
    if ((long)count > BYTES_MAX) p = fmt_str(p, end, " ...");
    log_emit(line, p);
    return (ssize_t)ret;
}

int close(int fd)
{
    long ret = sys_close(fd);
    char line[LINE_MAX]; char *end = line + LINE_MAX - 32;
    char *p = log_hdr(line, end, "CLOSE", fd, ret);
    log_emit(line, p);
    return (int)ret;
}

int ioctl(int fd, unsigned long request, ...)
{
    long arg;
    __builtin_va_list ap;
    __builtin_va_start(ap, request);
    arg = __builtin_va_arg(ap, long);
    __builtin_va_end(ap);

    long ret = sys_ioctl(fd, request, arg);
    char line[LINE_MAX]; char *end = line + LINE_MAX - 32;
    char *p = log_hdr(line, end, "IOCTL", fd, ret);
    p = fmt_str(p, end, "req=0x");
    for (int sh = 28; sh >= 0; sh -= 4) {
        unsigned d = (request >> sh) & 0xf;
        *p++ = "0123456789abcdef"[d];
    }
    /* Always log arg value as a hex pointer, plus deref for TCGETS/TCSETS. */
    p = fmt_str(p, end, " arg=0x");
    for (int sh = 28; sh >= 0; sh -= 4) {
        unsigned d = (((unsigned long)arg) >> sh) & 0xf;
        *p++ = "0123456789abcdef"[d];
    }
    if (request == 0x5401 || request == 0x5402 ||
        request == 0x5403 || request == 0x5404) {
        const unsigned char *b = (const unsigned char *)arg;
        p = fmt_str(p, end, " termios=");
        for (int i = 0; i < 60 && p + 3 < end; i++) {
            *p++ = ' ';
            p = fmt_hex2(p, b[i]);
        }
    }
    log_emit(line, p);
    (void)my_strncmp; (void)my_strlen;
    return (int)ret;
}
