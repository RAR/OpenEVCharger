/* Tiny HTTP/1.1 config + control server.
 *
 * v0.4 of delta-bridge: a single-page web app for on-device configuration +
 * basic control. HTTP-only, LAN-bound, single-threaded, integrated into the
 * bridge's existing main loop. No external dependencies; only POSIX sockets.
 *
 * Lifecycle:
 *   web_server_start()  -> binds 0.0.0.0:port, returns 0 + listen fd in
 *                          ws->listen_fd on success; -1 on failure (port in
 *                          use, etc.).
 *   web_tick()          -> non-blocking: drains all pending accepts, handles
 *                          each connection synchronously, closes it. Safe to
 *                          call every poll iteration even when idle.
 *   web_server_stop()   -> closes listen_fd. Safe on a zeroed struct.
 *
 * Auth is HTTP Basic. If web_user OR web_pass is empty in the bridge config
 * AUTH IS DISABLED — useful for first-boot setup. A one-shot warning fires
 * from web_server_start() in that case. Otherwise every request is gated
 * against the configured creds.
 *
 * The server holds a const-pointer to the live `struct config` and the
 * (RW-or-NULL) shmem so handlers can read state + write controls. The
 * config pointer must NOT be freed while the server is running; main.c
 * owns its lifetime.
 *
 * argv is also stashed for /api/restart's execv() path — the spec asks
 * the bridge to respawn itself on config save.
 */
#ifndef WEB_H
#define WEB_H

#include <stddef.h>

struct config;
struct shmem;

struct web_server {
    int           listen_fd;          /* -1 when not running */
    int           port;
    const char   *web_user;           /* points into cfg; empty = no auth */
    const char   *web_pass;
    struct config *cfg;               /* read+write (POST /api/config) */
    struct shmem  *shm;               /* live shmem (always set by main.c);
                                       * reads work, writes gate on
                                       * shm->writable inside cs_apply_* */
    char        **orig_argv;          /* for execv() on /api/restart */
    const char   *conf_path;          /* where POST /api/config writes */
    int           auth_disabled;      /* derived from web_user/pass empty */
};

/* Open the listen socket. Returns 0 on success, -1 on failure (in which
 * case ws->listen_fd is -1). Non-fatal — main.c logs and skips the server. */
int  web_server_start(struct web_server *ws);

/* Drain pending connections — each is handled fully then closed. Returns the
 * number of requests serviced this tick (0 = nothing happened). */
int  web_tick(struct web_server *ws);

/* Close listen socket. */
void web_server_stop(struct web_server *ws);

/* --- testable internals (also used by web.c itself) ----------------------
 * web_handle_request() takes the raw HTTP request as a NUL-terminated string
 * and writes a complete HTTP response into `out` (NUL-terminated). It does
 * NOT touch any socket. Used by host tests and by the per-connection
 * handler in web.c.
 *
 * `out_cap` must be at least 4 KB. Returns the number of bytes written
 * (excluding the NUL), or 0 if `out_cap` is too small. */
size_t web_handle_request(struct web_server *ws,
                          const char *req, size_t req_len,
                          char *out, size_t out_cap);

/* Decode a base64 string (no padding tolerance — accepts standard base64
 * with '=' padding). Writes up to `out_cap` bytes; returns decoded length,
 * or -1 on malformed input. Output is NOT NUL-terminated. */
int web_base64_decode(const char *in, size_t in_len,
                      unsigned char *out, size_t out_cap);

/* URL-decode `in_len` bytes from `in` into `out`. Writes at most `out_cap-1`
 * bytes and NUL-terminates. Returns bytes written (excluding NUL), -1 if
 * the output won't fit. '+' becomes ' ', '%HH' is hex-decoded; bare '%' or
 * invalid hex aborts with -1. */
int web_urldecode(const char *in, size_t in_len,
                  char *out, size_t out_cap);

/* Parse a `key=val&key=val` body. Iterator interface: call with *pos=0 to
 * start, then while it returns 1, `key`/`val` are filled (urldecoded,
 * NUL-terminated). Returns 0 at end-of-body, -1 on malformed input. */
int web_form_next(const char *body, size_t body_len, size_t *pos,
                  char *key, size_t key_cap,
                  char *val, size_t val_cap);

#endif /* WEB_H */
