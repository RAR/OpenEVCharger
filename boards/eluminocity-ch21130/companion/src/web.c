/* delta-bridge embedded HTTP/1.1 server — see web.h for design.
 *
 * Scope: one route table, basic-auth gate, form/JSON helpers, and a small
 * accept-loop. No keepalive, no chunking, no TLS. Single 8 KiB read cap
 * per request.
 *
 * The handler entry point web_handle_request() is sock-free so host tests
 * can drive it with raw request strings.
 */

#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE

#include "web.h"
#include "web_html.h"
#include "config.h"
#include "commands.h"
#include "charger_state.h"
#include "shmem.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_REQ_BYTES   (8 * 1024)
/* RESP_CAP sized to comfortably hold INDEX_HTML (~11 KB) plus headers. The
 * other responses (JSON state/config) are <2 KB; the SPA is what drives the
 * upper bound. */
#define RESP_CAP        (16 * 1024)

/* --- base64 -------------------------------------------------------------- */

static int b64v(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int web_base64_decode(const char *in, size_t in_len,
                      unsigned char *out, size_t out_cap)
{
    size_t i = 0, o = 0;
    while (i < in_len) {
        /* Skip whitespace tolerantly */
        while (i < in_len && (in[i] == ' ' || in[i] == '\t' ||
                              in[i] == '\r' || in[i] == '\n'))
            i++;
        if (i >= in_len) break;
        if (in_len - i < 4) return -1;
        int v0 = b64v((unsigned char)in[i]);
        int v1 = b64v((unsigned char)in[i + 1]);
        if (v0 < 0 || v1 < 0) return -1;
        int v2 = (in[i + 2] == '=') ? -2 : b64v((unsigned char)in[i + 2]);
        int v3 = (in[i + 3] == '=') ? -2 : b64v((unsigned char)in[i + 3]);
        if (v2 == -1 || v3 == -1) return -1;
        if (o >= out_cap) return -1;
        out[o++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        if (v2 != -2) {
            if (o >= out_cap) return -1;
            out[o++] = (unsigned char)(((v1 & 0xF) << 4) | (v2 >> 2));
            if (v3 != -2) {
                if (o >= out_cap) return -1;
                out[o++] = (unsigned char)(((v2 & 0x3) << 6) | v3);
            }
        }
        i += 4;
    }
    return (int)o;
}

/* --- URL-decode + form parser ------------------------------------------- */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int web_urldecode(const char *in, size_t in_len, char *out, size_t out_cap)
{
    size_t i = 0, o = 0;
    while (i < in_len) {
        char c = in[i++];
        if (c == '+') {
            if (o + 1 >= out_cap) return -1;
            out[o++] = ' ';
        } else if (c == '%') {
            if (i + 1 >= in_len + 1) return -1;
            if (i + 1 > in_len) return -1;
            int h = hex_nibble(in[i]);
            int l = hex_nibble(in[i + 1]);
            if (h < 0 || l < 0) return -1;
            if (o + 1 >= out_cap) return -1;
            out[o++] = (char)((h << 4) | l);
            i += 2;
        } else {
            if (o + 1 >= out_cap) return -1;
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return (int)o;
}

int web_form_next(const char *body, size_t body_len, size_t *pos,
                  char *key, size_t key_cap,
                  char *val, size_t val_cap)
{
    if (*pos >= body_len) return 0;
    size_t start = *pos;
    /* find '=' or '&' */
    size_t eq = start;
    while (eq < body_len && body[eq] != '=' && body[eq] != '&')
        eq++;
    /* If we found '&' before '=', that's a flag-style key (no value). */
    size_t val_start, val_end;
    if (eq >= body_len || body[eq] == '&') {
        /* key=<empty> */
        val_start = eq;
        val_end   = eq;
    } else {
        val_start = eq + 1;
        val_end   = val_start;
        while (val_end < body_len && body[val_end] != '&')
            val_end++;
    }
    if (web_urldecode(body + start, eq - start, key, key_cap) < 0) return -1;
    if (web_urldecode(body + val_start, val_end - val_start, val, val_cap) < 0)
        return -1;
    *pos = (val_end < body_len) ? val_end + 1 : body_len;
    return 1;
}

/* --- request struct ----------------------------------------------------- */

struct req {
    char  method[8];
    char  path[256];
    char  version[16];
    char  host[128];
    char  authorization[256];
    size_t content_length;
    int    have_content_length;
    char  content_type[64];
    const char *body;
    size_t      body_len;
};

/* Parse the request head. Returns 0 on success, -1 on malformed input,
 * -2 on too-large (413). `req_len` is the total length of the request
 * buffer; we'll point `out->body` at the byte after the CRLF CRLF. */
static int parse_request(const char *buf, size_t len, struct req *r)
{
    memset(r, 0, sizeof(*r));
    /* Find end-of-headers */
    const char *eoh = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            eoh = buf + i;
            break;
        }
    }
    if (!eoh) return -1;

    /* Request line. `end` is one past the final header's '\r' so the
     * header-scan loop can match that '\r' (it doubles as the first byte
     * of the \r\n\r\n terminator). Originally `end = eoh` dropped the
     * last header line. */
    const char *p = buf;
    const char *end = eoh + 1;
    const char *sp1 = memchr(p, ' ', end - p);
    if (!sp1) return -1;
    if ((size_t)(sp1 - p) >= sizeof(r->method)) return -1;
    memcpy(r->method, p, sp1 - p); r->method[sp1 - p] = '\0';
    p = sp1 + 1;
    const char *sp2 = memchr(p, ' ', end - p);
    if (!sp2) return -1;
    if ((size_t)(sp2 - p) >= sizeof(r->path)) return -1;
    memcpy(r->path, p, sp2 - p); r->path[sp2 - p] = '\0';
    p = sp2 + 1;
    const char *eol = memchr(p, '\r', end - p);
    if (!eol) return -1;
    if ((size_t)(eol - p) >= sizeof(r->version)) return -1;
    memcpy(r->version, p, eol - p); r->version[eol - p] = '\0';
    p = eol + 2;

    /* Check version: only HTTP/1.0 or HTTP/1.1 */
    if (strcmp(r->version, "HTTP/1.0") != 0 &&
        strcmp(r->version, "HTTP/1.1") != 0)
        return -1;

    /* Headers */
    while (p < end) {
        const char *line_end = memchr(p, '\r', end - p);
        if (!line_end) break;
        size_t hlen = line_end - p;
        const char *colon = memchr(p, ':', hlen);
        if (!colon) { p = line_end + 2; continue; }
        size_t name_len = colon - p;
        const char *val = colon + 1;
        while (val < line_end && (*val == ' ' || *val == '\t')) val++;
        size_t val_len = line_end - val;

        if (name_len == 4 && strncasecmp(p, "Host", 4) == 0) {
            if (val_len < sizeof(r->host)) {
                memcpy(r->host, val, val_len);
                r->host[val_len] = '\0';
            }
        } else if (name_len == 13 && strncasecmp(p, "Authorization", 13) == 0) {
            if (val_len < sizeof(r->authorization)) {
                memcpy(r->authorization, val, val_len);
                r->authorization[val_len] = '\0';
            }
        } else if (name_len == 14 && strncasecmp(p, "Content-Length", 14) == 0) {
            char tmp[32];
            if (val_len >= sizeof(tmp)) return -1;
            memcpy(tmp, val, val_len); tmp[val_len] = '\0';
            char *endp = NULL;
            long n = strtol(tmp, &endp, 10);
            if (!endp || *endp != '\0' || n < 0) return -1;
            r->content_length = (size_t)n;
            r->have_content_length = 1;
        } else if (name_len == 12 && strncasecmp(p, "Content-Type", 12) == 0) {
            if (val_len < sizeof(r->content_type)) {
                memcpy(r->content_type, val, val_len);
                r->content_type[val_len] = '\0';
            }
        }
        p = line_end + 2;
    }

    /* Body is everything after CRLF CRLF. */
    const char *body_start = eoh + 4;
    size_t body_avail = (size_t)((buf + len) - body_start);
    if (r->have_content_length) {
        if (r->content_length > body_avail) {
            /* Underread — for our simple sync server we treat what we have
             * as the body anyway; main.c reads up to MAX_REQ_BYTES once. */
            r->body_len = body_avail;
        } else {
            r->body_len = r->content_length;
        }
    } else {
        r->body_len = body_avail;
    }
    r->body = body_start;
    return 0;
}

/* --- response helpers --------------------------------------------------- */

static size_t make_response(char *out, size_t cap, int status,
                            const char *status_text,
                            const char *content_type,
                            const char *extra_headers,
                            const char *body, size_t body_len)
{
    int n = snprintf(out, cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "%s"
        "\r\n",
        status, status_text, content_type, body_len,
        extra_headers ? extra_headers : "");
    if (n < 0 || (size_t)n >= cap) return 0;
    size_t total = (size_t)n;
    if (body_len > 0) {
        if (total + body_len >= cap) return 0;
        memcpy(out + total, body, body_len);
        total += body_len;
    }
    if (total < cap) out[total] = '\0';
    return total;
}

static size_t resp_text(char *out, size_t cap, int status, const char *text,
                        const char *body)
{
    return make_response(out, cap, status, text, "text/plain; charset=utf-8",
                         NULL, body, body ? strlen(body) : 0);
}

static size_t resp_json(char *out, size_t cap, int status, const char *text,
                        const char *body)
{
    return make_response(out, cap, status, text, "application/json",
                         NULL, body, strlen(body));
}

/* --- auth --------------------------------------------------------------- */

/* Returns 1 if request is authorized, 0 if not. */
static int check_auth(const struct web_server *ws, const struct req *r)
{
    if (ws->auth_disabled) return 1;
    /* Expect "Basic <b64>" */
    const char *a = r->authorization;
    if (strncasecmp(a, "Basic ", 6) != 0) return 0;
    const char *b = a + 6;
    while (*b == ' ' || *b == '\t') b++;
    unsigned char decoded[256];
    int n = web_base64_decode(b, strlen(b), decoded, sizeof(decoded) - 1);
    if (n < 0) return 0;
    decoded[n] = '\0';
    /* "user:pass" */
    char *colon = strchr((char *)decoded, ':');
    if (!colon) return 0;
    *colon = '\0';
    const char *u = (const char *)decoded;
    const char *p = colon + 1;
    return strcmp(u, ws->web_user) == 0 && strcmp(p, ws->web_pass) == 0;
}

static size_t resp_401(char *out, size_t cap)
{
    static const char *body = "401 Unauthorized\n";
    return make_response(out, cap, 401, "Unauthorized",
                         "text/plain; charset=utf-8",
                         "WWW-Authenticate: Basic realm=\"delta-bridge\"\r\n",
                         body, strlen(body));
}

/* --- JSON helpers ------------------------------------------------------- */

/* Append a JSON-quoted string. Returns bytes appended (or 0 on overflow). */
static size_t json_quote(char *out, size_t cap, size_t off, const char *s)
{
    if (off >= cap) return 0;
    size_t start = off;
    if (off >= cap - 1) return 0;
    out[off++] = '"';
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        const char *esc = NULL;
        switch (c) {
        case '"':  esc = "\\\"";  break;
        case '\\': esc = "\\\\";  break;
        case '\n': esc = "\\n";   break;
        case '\r': esc = "\\r";   break;
        case '\t': esc = "\\t";   break;
        default: break;
        }
        if (esc) {
            if (off + 2 >= cap) return 0;
            out[off++] = esc[0];
            out[off++] = esc[1];
        } else if (c < 0x20) {
            if (off + 6 >= cap) return 0;
            int w = snprintf(out + off, cap - off, "\\u%04x", c);
            if (w < 0 || (size_t)w >= cap - off) return 0;
            off += (size_t)w;
        } else {
            if (off + 1 >= cap) return 0;
            out[off++] = (char)c;
        }
    }
    if (off >= cap - 1) return 0;
    out[off++] = '"';
    return off - start;
}

/* --- state JSON --------------------------------------------------------- */

static size_t build_state_json(struct web_server *ws, char *body, size_t cap)
{
    /* Snapshot the live shmem into a charger_state. If shm is NULL (no
     * write_enable, no RW attach) we still want the page to function — we
     * fall back to RO-attached numbers if available, but the bridge always
     * has SOME shmem pointer at this point because main.c attaches before
     * starting the web server. For belt-and-braces, gate on shm != NULL. */
    struct charger_state cs;
    charger_state_init(&cs);
    int availability_online = 0;
    if (ws->shm && ws->shm->base) {
        charger_state_read(&cs, ws->shm);
        /* "online" = stm32 link healthy. */
        availability_online = cs.stm32_link_ok ? 1 : 0;
    }

    /* Compose faults string (comma-joined, like the MQTT layer). */
    char faults[384] = "none";
    size_t fn = 0;
    if (cs.fault_bits) {
        faults[0] = '\0';
        for (int i = 0; i < CHARGER_MAX_FAULTS; i++) {
            if (!(cs.fault_bits & (1u << i))) continue;
            int w = snprintf(faults + fn, sizeof(faults) - fn,
                             "%s%s", fn ? "," : "", charger_fault_name(i));
            if (w < 0 || (size_t)w >= sizeof(faults) - fn) break;
            fn += (size_t)w;
        }
    }

    const char *device_id = ws->cfg->device_id[0] ? ws->cfg->device_id : "evmu30";

    int n = snprintf(body, cap,
        "{"
        "\"availability\":\"%s\","
        "\"device_id\":\"%s\","
        "\"voltage\":%.2f,"
        "\"current\":%.2f,"
        "\"power\":%.0f,"
        "\"pilot_duty\":%u,"
        "\"rated_amps\":%u,"
        "\"pilot_state\":\"%s\","
        "\"pri_state\":%u,"
        "\"user_state\":%u,"
        "\"red_led\":%u,"
        "\"stm32_link_ok\":%s,"
        "\"stm32_fault_raw\":%u,"
        "\"fault_bits\":%u,"
        "\"active_faults\":\"%s\""
        "}",
        availability_online ? "online" : "offline",
        device_id,
        cs.voltage_v, cs.current_a, cs.power_w,
        (unsigned)cs.pilot_duty_pct, (unsigned)cs.rated_amps,
        pilot_state_str(cs.pilot_state),
        (unsigned)cs.pri_state, (unsigned)cs.user_state,
        (unsigned)cs.red_led,
        cs.stm32_link_ok ? "true" : "false",
        (unsigned)cs.stm32_fault_raw,
        (unsigned)cs.fault_bits,
        faults);
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

/* --- config JSON (mask passwords) -------------------------------------- */

static const char *mask_pw(const char *s) { return s[0] ? "********" : ""; }

/* Append a printf-style segment. Returns 0 on overflow (caller bails). */
static int json_append_fmt(char *body, size_t cap, size_t *off,
                           const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(body + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *off) return 0;
    *off += (size_t)n;
    return 1;
}

/* Append a JSON-quoted string segment. Returns 0 on overflow. */
static int json_append_str(char *body, size_t cap, size_t *off, const char *s)
{
    size_t q = json_quote(body, cap, *off, s);
    if (q == 0) return 0;
    *off += q;
    return 1;
}

static size_t build_config_json(const struct config *c, char *body, size_t cap)
{
    size_t off = 0;
    if (!json_append_fmt(body, cap, &off, "{\"broker_host\":")) return 0;
    if (!json_append_str(body, cap, &off, c->broker_host))      return 0;
    if (!json_append_fmt(body, cap, &off,
                         ",\"broker_port\":%d,\"broker_user\":",
                         c->broker_port))                       return 0;
    if (!json_append_str(body, cap, &off, c->broker_user))      return 0;
    if (!json_append_fmt(body, cap, &off, ",\"broker_pass\":")) return 0;
    if (!json_append_str(body, cap, &off, mask_pw(c->broker_pass))) return 0;
    if (!json_append_fmt(body, cap, &off, ",\"topic_prefix\":")) return 0;
    if (!json_append_str(body, cap, &off, c->topic_prefix))     return 0;
    if (!json_append_fmt(body, cap, &off, ",\"device_id\":"))   return 0;
    if (!json_append_str(body, cap, &off, c->device_id))        return 0;
    if (!json_append_fmt(body, cap, &off,
                         ",\"poll_hz\":%d,\"write_enable\":%s,"
                         "\"web_enable\":%s,\"web_port\":%d,\"web_user\":",
                         c->poll_hz, c->write_enable ? "true" : "false",
                         c->web_enable ? "true" : "false", c->web_port))
        return 0;
    if (!json_append_str(body, cap, &off, c->web_user))         return 0;
    if (!json_append_fmt(body, cap, &off, ",\"web_pass\":"))    return 0;
    if (!json_append_str(body, cap, &off, mask_pw(c->web_pass))) return 0;

    if (off + 1 >= cap) return 0;
    body[off++] = '}';
    body[off]   = '\0';
    return off;
}

/* --- POST /api/config -------------------------------------------------- */

static int validate_and_apply_config(struct config *c, const char *body,
                                     size_t body_len, char *err, size_t errcap)
{
    /* Iterate body — apply each present key. Missing keys leave the
     * current config_load()'d values intact.
     *
     * val[] is sized to CONFIG_STR_MAX so that the snprintf-into-target
     * calls aren't flagged by -Wformat-truncation. Inputs longer than
     * that get truncated by web_form_next — fine for a small config UI. */
    size_t pos = 0;
    char key[64], val[CONFIG_STR_MAX];
    int rc;
    while ((rc = web_form_next(body, body_len, &pos,
                               key, sizeof(key), val, sizeof(val))) == 1) {
        if (!strcmp(key, "broker_host")) {
            snprintf(c->broker_host, sizeof(c->broker_host), "%s", val);
        } else if (!strcmp(key, "broker_port")) {
            char *endp = NULL;
            long n = strtol(val, &endp, 10);
            if (!endp || *endp != '\0' || n < 1 || n > 65535) {
                snprintf(err, errcap, "broker_port out of range");
                return -1;
            }
            c->broker_port = (int)n;
        } else if (!strcmp(key, "broker_user")) {
            snprintf(c->broker_user, sizeof(c->broker_user), "%s", val);
        } else if (!strcmp(key, "broker_pass")) {
            /* Empty submission means "don't change" — but the masked value
             * "********" coming back from a form re-POST is also a "don't
             * change" intent. The browser only sends new content if the
             * user typed something. We accept either as "no change",
             * otherwise overwrite. */
            if (val[0] != '\0' && strcmp(val, "********") != 0)
                snprintf(c->broker_pass, sizeof(c->broker_pass), "%s", val);
        } else if (!strcmp(key, "topic_prefix")) {
            snprintf(c->topic_prefix, sizeof(c->topic_prefix), "%s", val);
        } else if (!strcmp(key, "device_id")) {
            snprintf(c->device_id, sizeof(c->device_id), "%s", val);
        } else if (!strcmp(key, "poll_hz")) {
            char *endp = NULL;
            long n = strtol(val, &endp, 10);
            if (!endp || *endp != '\0' || n < 1) {
                snprintf(err, errcap, "poll_hz must be >= 1");
                return -1;
            }
            c->poll_hz = (int)n;
        } else if (!strcmp(key, "write_enable")) {
            /* Mirror config.c's parse_bool. */
            if (!strcasecmp(val, "true") || !strcasecmp(val, "yes") ||
                !strcasecmp(val, "on")   || !strcmp(val, "1"))
                c->write_enable = 1;
            else if (!strcasecmp(val, "false") || !strcasecmp(val, "no") ||
                     !strcasecmp(val, "off")   || !strcmp(val, "0"))
                c->write_enable = 0;
            else {
                snprintf(err, errcap, "write_enable must be bool");
                return -1;
            }
        } else if (!strcmp(key, "web_enable")) {
            if (!strcasecmp(val, "true") || !strcasecmp(val, "yes") ||
                !strcasecmp(val, "on")   || !strcmp(val, "1"))
                c->web_enable = 1;
            else if (!strcasecmp(val, "false") || !strcasecmp(val, "no") ||
                     !strcasecmp(val, "off")   || !strcmp(val, "0"))
                c->web_enable = 0;
            else {
                snprintf(err, errcap, "web_enable must be bool");
                return -1;
            }
        } else if (!strcmp(key, "web_port")) {
            char *endp = NULL;
            long n = strtol(val, &endp, 10);
            if (!endp || *endp != '\0' || n < 1 || n > 65535) {
                snprintf(err, errcap, "web_port out of range");
                return -1;
            }
            c->web_port = (int)n;
        } else if (!strcmp(key, "web_user")) {
            snprintf(c->web_user, sizeof(c->web_user), "%s", val);
        } else if (!strcmp(key, "web_pass")) {
            if (val[0] != '\0' && strcmp(val, "********") != 0)
                snprintf(c->web_pass, sizeof(c->web_pass), "%s", val);
        }
        /* Unknown keys silently ignored — the form may include hidden
         * fields, browser-added _csrf-style fields, etc. */
    }
    if (rc < 0) {
        snprintf(err, errcap, "malformed form body");
        return -1;
    }
    return 0;
}

static int write_config_file(const struct config *c, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "# delta-bridge configuration — written by web UI\n");
    fprintf(f, "broker_host  = %s\n",  c->broker_host);
    fprintf(f, "broker_port  = %d\n",  c->broker_port);
    if (c->broker_user[0])
        fprintf(f, "broker_user  = %s\n", c->broker_user);
    if (c->broker_pass[0])
        fprintf(f, "broker_pass  = %s\n", c->broker_pass);
    fprintf(f, "topic_prefix = %s\n",  c->topic_prefix);
    if (c->device_id[0])
        fprintf(f, "device_id    = %s\n", c->device_id);
    fprintf(f, "poll_hz      = %d\n",  c->poll_hz);
    fprintf(f, "write_enable = %s\n",  c->write_enable ? "true" : "false");
    fprintf(f, "web_enable   = %s\n",  c->web_enable   ? "true" : "false");
    fprintf(f, "web_port     = %d\n",  c->web_port);
    if (c->web_user[0])
        fprintf(f, "web_user     = %s\n", c->web_user);
    if (c->web_pass[0])
        fprintf(f, "web_pass     = %s\n", c->web_pass);
    fclose(f);
    return 0;
}

/* --- restart ------------------------------------------------------------ */

static void do_restart(struct web_server *ws)
{
    fflush(stderr);
    fflush(stdout);
    fprintf(stderr, "delta-bridge: web: /api/restart — re-exec'ing %s\n",
            ws->orig_argv ? ws->orig_argv[0] : "?");
    fflush(stderr);
    if (ws->orig_argv && ws->orig_argv[0]) {
        execv(ws->orig_argv[0], ws->orig_argv);
        /* If execv returns we're in deep trouble. */
        fprintf(stderr, "delta-bridge: web: execv failed: %s\n",
                strerror(errno));
    }
}

/* --- dispatch ----------------------------------------------------------- */

static size_t handle_get_root(char *out, size_t cap)
{
    /* INDEX_HTML is embedded in web_html.h. */
    return make_response(out, cap, 200, "OK",
                         "text/html; charset=utf-8", NULL,
                         INDEX_HTML, INDEX_HTML_LEN);
}

static size_t handle_get_state(struct web_server *ws, char *out, size_t cap)
{
    char body[2048];
    size_t n = build_state_json(ws, body, sizeof(body));
    if (n == 0)
        return resp_json(out, cap, 500, "Internal Server Error",
                         "{\"error\":\"state too large\"}");
    return resp_json(out, cap, 200, "OK", body);
}

static size_t handle_get_config(struct web_server *ws, char *out, size_t cap)
{
    char body[2048];
    size_t n = build_config_json(ws->cfg, body, sizeof(body));
    if (n == 0)
        return resp_json(out, cap, 500, "Internal Server Error",
                         "{\"error\":\"config too large\"}");
    return resp_json(out, cap, 200, "OK", body);
}

static size_t handle_post_config(struct web_server *ws, const struct req *r,
                                 char *out, size_t cap)
{
    char err[128] = {0};
    if (validate_and_apply_config(ws->cfg, r->body, r->body_len,
                                  err, sizeof(err)) != 0) {
        char body[256];
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
                 err[0] ? err : "validation failed");
        return resp_json(out, cap, 400, "Bad Request", body);
    }
    if (write_config_file(ws->cfg, ws->conf_path) != 0) {
        char body[256];
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"error\":\"cannot write %s\"}",
                 ws->conf_path);
        return resp_json(out, cap, 500, "Internal Server Error", body);
    }
    /* Return the new effective config, masked. */
    char cfg_body[2048];
    size_t n = build_config_json(ws->cfg, cfg_body, sizeof(cfg_body));
    if (n == 0)
        return resp_json(out, cap, 500, "Internal Server Error",
                         "{\"ok\":false,\"error\":\"config too large\"}");
    /* Wrap in {"ok":true,"config":<cfg>} */
    char body[2200];
    int w = snprintf(body, sizeof(body), "{\"ok\":true,\"config\":%s}",
                     cfg_body);
    if (w < 0 || (size_t)w >= sizeof(body))
        return resp_json(out, cap, 500, "Internal Server Error",
                         "{\"ok\":false,\"error\":\"response too large\"}");
    return resp_json(out, cap, 200, "OK", body);
}

static size_t handle_post_rated_amps(struct web_server *ws, const struct req *r,
                                     char *out, size_t cap)
{
    /* Parse amps= */
    size_t pos = 0;
    char key[32], val[64];
    int rc;
    const char *amps_val = NULL;
    while ((rc = web_form_next(r->body, r->body_len, &pos,
                               key, sizeof(key), val, sizeof(val))) == 1) {
        if (!strcmp(key, "amps")) { amps_val = val; break; }
    }
    if (!amps_val) {
        return resp_json(out, cap, 400, "Bad Request",
                         "{\"ok\":false,\"error\":\"missing amps\"}");
    }
    char err[64] = {0};
    long out_amps = 0;
    int crc = cs_apply_rated_amps_write(ws->shm, (const unsigned char *)amps_val,
                                        strlen(amps_val), &out_amps,
                                        err, sizeof(err));
    char body[256];
    if (crc == 0) {
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"rated_amps\":%ld}", out_amps);
        return resp_json(out, cap, 200, "OK", body);
    }
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
             err[0] ? err : "invalid");
    return resp_json(out, cap, (crc == -2) ? 503 : 400,
                     (crc == -2) ? "Service Unavailable" : "Bad Request", body);
}

static size_t handle_post_authorize(struct web_server *ws, const struct req *r,
                                    char *out, size_t cap)
{
    size_t pos = 0;
    char key[32], val[64];
    int rc;
    const char *st = NULL;
    while ((rc = web_form_next(r->body, r->body_len, &pos,
                               key, sizeof(key), val, sizeof(val))) == 1) {
        if (!strcmp(key, "state")) { st = val; break; }
    }
    if (!st) {
        return resp_json(out, cap, 400, "Bad Request",
                         "{\"ok\":false,\"error\":\"missing state\"}");
    }
    char err[64] = {0};
    int on = -1;
    int crc = cs_apply_authorize_write(ws->shm, (const unsigned char *)st,
                                       strlen(st), &on, err, sizeof(err));
    char body[256];
    if (crc == 0) {
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"state\":\"%s\"}", on ? "ON" : "OFF");
        return resp_json(out, cap, 200, "OK", body);
    }
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
             err[0] ? err : "invalid");
    return resp_json(out, cap, (crc == -2) ? 503 : 400,
                     (crc == -2) ? "Service Unavailable" : "Bad Request", body);
}

static size_t handle_post_clear_faults(struct web_server *ws, const struct req *r,
                                       char *out, size_t cap)
{
    (void)r;
    char err[64] = {0};
    int crc = cs_apply_clear_faults_write(ws->shm, err, sizeof(err));
    if (crc == 0)
        return resp_json(out, cap, 200, "OK", "{\"ok\":true}");
    char body[256];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
             err[0] ? err : "invalid");
    return resp_json(out, cap, (crc == -2) ? 503 : 400,
                     (crc == -2) ? "Service Unavailable" : "Bad Request", body);
}

size_t web_handle_request(struct web_server *ws, const char *req, size_t req_len,
                          char *out, size_t cap)
{
    if (req_len > MAX_REQ_BYTES)
        return resp_text(out, cap, 413, "Payload Too Large",
                         "413 Payload Too Large\n");
    struct req r;
    int prc = parse_request(req, req_len, &r);
    if (prc != 0)
        return resp_text(out, cap, 400, "Bad Request", "400 Bad Request\n");

    if (!check_auth(ws, &r))
        return resp_401(out, cap);

    /* Strip query string from path (we don't currently use any). */
    char *q = strchr(r.path, '?');
    if (q) *q = '\0';

    if (!strcmp(r.method, "GET")) {
        if (!strcmp(r.path, "/") || !strcmp(r.path, "/index.html"))
            return handle_get_root(out, cap);
        if (!strcmp(r.path, "/api/state"))
            return handle_get_state(ws, out, cap);
        if (!strcmp(r.path, "/api/config"))
            return handle_get_config(ws, out, cap);
        return resp_text(out, cap, 404, "Not Found", "404 Not Found\n");
    }
    if (!strcmp(r.method, "POST")) {
        if (!strcmp(r.path, "/api/config"))
            return handle_post_config(ws, &r, out, cap);
        if (!strcmp(r.path, "/api/control/rated_amps"))
            return handle_post_rated_amps(ws, &r, out, cap);
        if (!strcmp(r.path, "/api/control/authorize"))
            return handle_post_authorize(ws, &r, out, cap);
        if (!strcmp(r.path, "/api/control/clear_faults"))
            return handle_post_clear_faults(ws, &r, out, cap);
        if (!strcmp(r.path, "/api/restart")) {
            /* Send 200 first so the client sees a clean response, then
             * exec — but we never actually return from there. We write
             * the response into `out`; the caller's I/O loop will flush
             * the bytes and then call do_restart() because we hand it
             * back via a sentinel. For host tests we just return the
             * response and skip the exec. */
            size_t n = resp_json(out, cap, 200, "OK",
                                 "{\"ok\":true,\"restarting\":true}");
            /* Mark the restart side-effect via a flag on ws — main loop
             * checks it after sending the response. Done this way so
             * we can unit-test without re-execing the test process. */
            ws->listen_fd = (ws->listen_fd >= 0) ? ws->listen_fd : -1;
            /* Stash via env var? No — much simpler: leave a flag here. */
            /* Use a static flag observable through a getter; but
             * accessing main from web is messy. Instead, the per-tick
             * caller passes ws back in and checks a "restart_pending"
             * field. Set it here. */
            ws->orig_argv = ws->orig_argv;        /* no-op (keep linter happy) */
            /* See web_tick — it checks ws->_restart_pending after each
             * request. Add field below. */
            extern void web_request_restart(struct web_server *ws);
            web_request_restart(ws);
            return n;
        }
        return resp_text(out, cap, 404, "Not Found", "404 Not Found\n");
    }
    return resp_text(out, cap, 405, "Method Not Allowed",
                     "405 Method Not Allowed\n");
}

/* --- restart signaling -------------------------------------------------- */

static int g_restart_pending = 0;
void web_request_restart(struct web_server *ws)
{
    (void)ws;
    g_restart_pending = 1;
}

/* --- server lifecycle --------------------------------------------------- */

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int web_server_start(struct web_server *ws)
{
    ws->listen_fd = -1;
    ws->auth_disabled =
        (ws->web_user == NULL || ws->web_user[0] == '\0' ||
         ws->web_pass == NULL || ws->web_pass[0] == '\0');
    if (ws->auth_disabled)
        fprintf(stderr,
                "delta-bridge: web: WARNING — auth disabled (web_user or "
                "web_pass empty). Every request will be accepted. "
                "Set both in /Storage/delta-bridge.conf to lock down.\n");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "delta-bridge: web: socket() failed: %s\n",
                strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons((unsigned short)ws->port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "delta-bridge: web: bind(:%d) failed: %s\n",
                ws->port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        fprintf(stderr, "delta-bridge: web: listen() failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }
    if (set_nonblock(fd) < 0) {
        fprintf(stderr,
                "delta-bridge: web: fcntl(O_NONBLOCK) failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }
    ws->listen_fd = fd;
    fprintf(stderr, "delta-bridge: web: listening on 0.0.0.0:%d\n", ws->port);
    return 0;
}

/* Read up to MAX_REQ_BYTES, blocking with a short timeout. Returns bytes
 * read, or -1 on error/oversize. */
static ssize_t read_request(int fd, char *buf, size_t cap)
{
    size_t total = 0;
    /* Set a per-recv timeout so a slow/malicious client can't wedge us. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    /* Switch the socket to blocking for the read — simpler than poll() for
     * a one-shot synchronous handler. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    while (total < cap) {
        ssize_t n = recv(fd, buf + total, cap - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        /* Stop once we have the header terminator and any declared body. */
        if (total >= 4) {
            for (size_t i = 0; i + 3 < total; i++) {
                if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                    buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                    /* Find Content-Length to decide if we need more bytes. */
                    char header_copy[2048];
                    size_t hcap = i < sizeof(header_copy) - 1 ? i : sizeof(header_copy) - 1;
                    memcpy(header_copy, buf, hcap);
                    header_copy[hcap] = '\0';
                    /* case-insensitive search */
                    long cl = 0;
                    for (char *p = header_copy; *p; p++) {
                        if (strncasecmp(p, "content-length:", 15) == 0) {
                            p += 15;
                            while (*p == ' ' || *p == '\t') p++;
                            cl = strtol(p, NULL, 10);
                            break;
                        }
                    }
                    size_t body_so_far = total - (i + 4);
                    if (cl <= 0 || body_so_far >= (size_t)cl)
                        return (ssize_t)total;
                    /* else need more — fall back into the loop */
                    goto need_more;
                }
            }
        }
need_more:
        ;
    }
    return (ssize_t)total;
}

static void write_all(int fd, const char *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, buf + total, len - total, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        total += (size_t)n;
    }
}

int web_tick(struct web_server *ws)
{
    if (ws->listen_fd < 0) return 0;
    int handled = 0;
    for (;;) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int cfd = accept(ws->listen_fd, (struct sockaddr *)&cli, &cl);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            fprintf(stderr, "delta-bridge: web: accept(): %s\n",
                    strerror(errno));
            break;
        }
        static char req_buf[MAX_REQ_BYTES + 1];
        ssize_t n = read_request(cfd, req_buf, sizeof(req_buf) - 1);
        if (n < 0) n = 0;
        req_buf[n] = '\0';

        static char resp_buf[RESP_CAP];
        size_t rn = web_handle_request(ws, req_buf, (size_t)n,
                                       resp_buf, sizeof(resp_buf));
        if (rn > 0)
            write_all(cfd, resp_buf, rn);
        close(cfd);
        handled++;

        if (g_restart_pending) {
            g_restart_pending = 0;
            do_restart(ws);
            /* If we get here, execv failed; keep serving anyway. */
        }
        /* Loop again — drain whatever else is in the accept queue. */
    }
    return handled;
}

void web_server_stop(struct web_server *ws)
{
    if (ws->listen_fd >= 0) {
        close(ws->listen_fd);
        ws->listen_fd = -1;
    }
}
