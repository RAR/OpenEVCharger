/* test_web.c — sock-free coverage of the embedded HTTP server.
 *
 * Drives web_handle_request() with hand-written request strings, asserts
 * the response status line / headers / body. The accept loop itself isn't
 * tested here — that needs a real socket on the host and is brittle in CI.
 *
 * Categories:
 *   1. HTTP parser (well-formed, malformed, oversize, bad version)
 *   2. Routing (known paths -> their handlers, unknown -> 404)
 *   3. Auth (basic auth, disabled when web_user empty, 401 on wrong creds)
 *   4. base64 + URL-decode + form-decoder
 *   5. JSON state output — keys present, no garbage
 *   6. /api/config GET and POST (password masking, partial-submission keeps
 *      old pass)
 *   7. /api/control/* validation matrix (delegates to commands.c but we want
 *      to confirm the response envelope is right)
 *
 * We intentionally do NOT call web_server_start() — that would bind a real
 * socket. All testing is at the in-memory request-handler boundary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_harness.h"
#include "web.h"
#include "config.h"
#include "shmem.h"
#include "shmem_offsets.h"

/* Shared shmem buffer for control tests. */
static unsigned char g_shm_buf[SHMEM_SIZE];

static void make_shm(struct shmem *sm)
{
    memset(g_shm_buf, 0, sizeof(g_shm_buf));
    memset(sm, 0, sizeof(*sm));
    sm->base     = g_shm_buf;
    sm->size     = sizeof(g_shm_buf);
    sm->shmid    = -1;
    sm->writable = 1;
}

/* Build a request string with optional body. */
static size_t mkreq(char *out, size_t cap,
                    const char *method, const char *path,
                    const char *extra_headers, const char *body)
{
    size_t body_len = body ? strlen(body) : 0;
    return snprintf(out, cap,
        "%s %s HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        "\r\n%s",
        method, path, body_len,
        extra_headers ? extra_headers : "",
        body ? body : "");
}

/* Pull the numeric status from "HTTP/1.1 NNN ..." */
static int status_of(const char *resp)
{
    if (strncmp(resp, "HTTP/1.1 ", 9) != 0) return -1;
    return atoi(resp + 9);
}

/* Return pointer to start of body (after blank line), or NULL. */
static const char *body_of(const char *resp)
{
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/* --- helpers / fixtures ------------------------------------------------- */

static void mk_ws_noauth(struct web_server *ws, struct config *cfg,
                         struct shmem *sm)
{
    config_defaults(cfg);
    snprintf(cfg->device_id, sizeof(cfg->device_id), "testunit");
    memset(ws, 0, sizeof(*ws));
    ws->listen_fd = -1;
    ws->port = 8080;
    ws->web_user = "";       /* empty = auth disabled */
    ws->web_pass = "";
    ws->cfg = cfg;
    ws->shm = sm;
    ws->conf_path = "/tmp/delta-bridge-test.conf";
    ws->auth_disabled = 1;
}

static void mk_ws_auth(struct web_server *ws, struct config *cfg,
                       struct shmem *sm,
                       const char *user, const char *pass)
{
    mk_ws_noauth(ws, cfg, sm);
    ws->web_user = user;
    ws->web_pass = pass;
    ws->auth_disabled = 0;
}

/* --- 1. parser ---------------------------------------------------------- */

static void test_parser_basic(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);

    /* GET / returns the full SPA (~11 KB). Give the response buffer 16 KB
     * so the test mirrors the server's actual RESP_CAP. */
    char req[1024];
    static char resp[16 * 1024];
    size_t rn = mkreq(req, sizeof(req), "GET", "/", NULL, NULL);
    size_t wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK(wn > 0);
    CHECK_EQ(status_of(resp), 200);
    CHECK(strstr(resp, "Content-Type: text/html") != NULL);
    CHECK(strstr(resp, "Connection: close") != NULL);
    CHECK(strstr(resp, "delta-bridge") != NULL);
    CHECK(strstr(resp, "/api/state") != NULL);
    CHECK(strstr(resp, "/api/config") != NULL);
}

static void test_parser_bad_version(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);

    char resp[8192];
    const char *req =
        "GET / HTTP/0.9\r\n"
        "Host: x\r\n\r\n";
    size_t wn = web_handle_request(&ws, req, strlen(req), resp, sizeof(resp));
    CHECK(wn > 0);
    CHECK_EQ(status_of(resp), 400);
}

static void test_parser_no_double_crlf(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);

    char resp[8192];
    /* missing the terminating blank line */
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\n";
    size_t wn = web_handle_request(&ws, req, strlen(req), resp, sizeof(resp));
    CHECK(wn > 0);
    CHECK_EQ(status_of(resp), 400);
}

static void test_parser_oversize(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);

    /* 8KB + 1 — but we don't have 8KB stack space; reuse the heap. */
    char *req = malloc(16 * 1024);
    memset(req, 'A', 9 * 1024);
    char resp[8192];
    size_t wn = web_handle_request(&ws, req, 9 * 1024, resp, sizeof(resp));
    CHECK(wn > 0);
    CHECK_EQ(status_of(resp), 413);
    free(req);
}

static void test_parser_post_with_body(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);
    /* Reset state so OFF_RATED_AMPS reads as something we can change. */
    shmem_write_u8(&sm, OFF_RATED_AMPS, 30);

    char req[512], resp[8192];
    size_t rn = mkreq(req, sizeof(req),
                      "POST", "/api/control/rated_amps",
                      "Content-Type: application/x-www-form-urlencoded\r\n",
                      "amps=12");
    size_t wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK(wn > 0);
    CHECK_EQ(status_of(resp), 200);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 12);
}

/* --- 2. routing --------------------------------------------------------- */

static void test_routing(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);

    char req[512], resp[8192];
    /* unknown -> 404 */
    size_t rn = mkreq(req, sizeof(req), "GET", "/nope", NULL, NULL);
    size_t wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK(wn > 0);
    CHECK_EQ(status_of(resp), 404);

    /* known GETs */
    rn = mkreq(req, sizeof(req), "GET", "/api/state", NULL, NULL);
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    CHECK(strstr(resp, "Content-Type: application/json") != NULL);

    rn = mkreq(req, sizeof(req), "GET", "/api/config", NULL, NULL);
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    CHECK(strstr(resp, "Content-Type: application/json") != NULL);

    /* unsupported method -> 405 */
    rn = mkreq(req, sizeof(req), "DELETE", "/api/config", NULL, NULL);
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 405);

    /* path with query string normalises */
    rn = mkreq(req, sizeof(req), "GET", "/api/state?nocache=1", NULL, NULL);
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    (void)wn;
}

/* --- 3. auth ------------------------------------------------------------ */

static void test_auth(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_auth(&ws, &cfg, &sm, "admin", "hunter2");

    char req[512], resp[8192];
    /* No creds at all -> 401 */
    size_t rn = mkreq(req, sizeof(req), "GET", "/api/state", NULL, NULL);
    size_t wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 401);
    CHECK(strstr(resp, "WWW-Authenticate: Basic") != NULL);

    /* Right creds: "admin:hunter2" -> "YWRtaW46aHVudGVyMg==" */
    rn = mkreq(req, sizeof(req), "GET", "/api/state",
               "Authorization: Basic YWRtaW46aHVudGVyMg==\r\n", NULL);
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);

    /* Wrong pass — same user, "admin:wrongpw" -> "YWRtaW46d3JvbmdwdwbM" .. simpler to use a known wrong one. */
    /* "admin:nope" -> YWRtaW46bm9wZQ== */
    rn = mkreq(req, sizeof(req), "GET", "/api/state",
               "Authorization: Basic YWRtaW46bm9wZQ==\r\n", NULL);
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 401);

    /* Malformed Authorization header -> 401 */
    rn = mkreq(req, sizeof(req), "GET", "/api/state",
               "Authorization: Bearer something\r\n", NULL);
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 401);

    /* Now disable auth and the same naked request must succeed. */
    mk_ws_noauth(&ws, &cfg, &sm);
    rn = mkreq(req, sizeof(req), "GET", "/api/state", NULL, NULL);
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    (void)wn;
}

/* --- 4. helpers --------------------------------------------------------- */

static void test_helpers(void)
{
    /* base64 — "admin:hunter2" round-trip */
    unsigned char out[64];
    int n = web_base64_decode("YWRtaW46aHVudGVyMg==", 20, out, sizeof(out));
    CHECK_EQ(n, 13);
    out[n] = '\0';
    CHECK_STR((char *)out, "admin:hunter2");

    /* base64 — invalid */
    n = web_base64_decode("$$$$$$$$", 8, out, sizeof(out));
    CHECK(n < 0);

    /* URL-decode */
    char buf[64];
    int u = web_urldecode("hello+world", 11, buf, sizeof(buf));
    CHECK_EQ(u, 11);
    CHECK_STR(buf, "hello world");

    u = web_urldecode("%41%42%43", 9, buf, sizeof(buf));
    CHECK_EQ(u, 3);
    CHECK_STR(buf, "ABC");

    u = web_urldecode("a%20b", 5, buf, sizeof(buf));
    CHECK_STR(buf, "a b");

    u = web_urldecode("a%2", 3, buf, sizeof(buf));
    CHECK(u < 0);

    /* form parser */
    size_t pos = 0;
    char k[32], v[64];
    int rc = web_form_next("amps=18", 7, &pos, k, sizeof(k), v, sizeof(v));
    CHECK_EQ(rc, 1);
    CHECK_STR(k, "amps");
    CHECK_STR(v, "18");
    rc = web_form_next("amps=18", 7, &pos, k, sizeof(k), v, sizeof(v));
    CHECK_EQ(rc, 0);    /* end */

    pos = 0;
    rc = web_form_next("a=1&b=two&c=", 12, &pos, k, sizeof(k), v, sizeof(v));
    CHECK_EQ(rc, 1); CHECK_STR(k, "a"); CHECK_STR(v, "1");
    rc = web_form_next("a=1&b=two&c=", 12, &pos, k, sizeof(k), v, sizeof(v));
    CHECK_EQ(rc, 1); CHECK_STR(k, "b"); CHECK_STR(v, "two");
    rc = web_form_next("a=1&b=two&c=", 12, &pos, k, sizeof(k), v, sizeof(v));
    CHECK_EQ(rc, 1); CHECK_STR(k, "c"); CHECK_STR(v, "");
    rc = web_form_next("a=1&b=two&c=", 12, &pos, k, sizeof(k), v, sizeof(v));
    CHECK_EQ(rc, 0);

    /* form parser with URL-encoded value */
    pos = 0;
    rc = web_form_next("host=10.0.0.5&pass=p%40w%20rd", 29, &pos,
                       k, sizeof(k), v, sizeof(v));
    CHECK_EQ(rc, 1); CHECK_STR(k, "host"); CHECK_STR(v, "10.0.0.5");
    rc = web_form_next("host=10.0.0.5&pass=p%40w%20rd", 29, &pos,
                       k, sizeof(k), v, sizeof(v));
    CHECK_EQ(rc, 1); CHECK_STR(k, "pass"); CHECK_STR(v, "p@w rd");
}

/* --- 5. /api/state JSON ------------------------------------------------- */

static void test_state_json(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    /* Seed some recognizable values. */
    g_shm_buf[OFF_VRMS_MEAS]   = 0x29; g_shm_buf[OFF_VRMS_MEAS + 1] = 0x31; /* 125.85 */
    g_shm_buf[OFF_IRMS_MEAS]   = 0xa0; g_shm_buf[OFF_IRMS_MEAS + 1] = 0x00; /* 16.0 */
    g_shm_buf[OFF_POWER_MEAS]  = 0x5c; g_shm_buf[OFF_POWER_MEAS + 1] = 0x00; /* 92 */
    g_shm_buf[OFF_PILOT_STATE] = 2;    /* C */
    g_shm_buf[OFF_PRI_STATE]   = 3;
    g_shm_buf[OFF_USER_STATE]  = 2;
    g_shm_buf[OFF_RED_LED]     = 1;
    g_shm_buf[OFF_PILOT_DUTY]  = 25;
    g_shm_buf[OFF_RATED_AMPS]  = 18;

    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);

    char req[256], resp[8192];
    size_t rn = mkreq(req, sizeof(req), "GET", "/api/state", NULL, NULL);
    size_t wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK(wn > 0);
    CHECK_EQ(status_of(resp), 200);

    const char *body = body_of(resp);
    CHECK(body != NULL);
    /* All keys present */
    CHECK(strstr(body, "\"availability\":") != NULL);
    CHECK(strstr(body, "\"device_id\":\"testunit\"") != NULL);
    CHECK(strstr(body, "\"voltage\":125.85") != NULL);
    CHECK(strstr(body, "\"current\":16.00") != NULL);
    CHECK(strstr(body, "\"power\":92") != NULL);
    CHECK(strstr(body, "\"pilot_duty\":25") != NULL);
    CHECK(strstr(body, "\"rated_amps\":18") != NULL);
    CHECK(strstr(body, "\"pilot_state\":\"C\"") != NULL);
    CHECK(strstr(body, "\"pri_state\":3") != NULL);
    CHECK(strstr(body, "\"user_state\":2") != NULL);
    CHECK(strstr(body, "\"red_led\":1") != NULL);
    CHECK(strstr(body, "\"stm32_link_ok\":true") != NULL);
    CHECK(strstr(body, "\"active_faults\":\"none\"") != NULL);
}

/* --- 6. /api/config GET + POST ----------------------------------------- */

static void test_config_get_masks_passwords(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);
    snprintf(cfg.broker_pass, sizeof(cfg.broker_pass), "secret123");
    snprintf(cfg.web_pass, sizeof(cfg.web_pass), "webpw");

    char req[256], resp[8192];
    size_t rn = mkreq(req, sizeof(req), "GET", "/api/config", NULL, NULL);
    size_t wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    const char *body = body_of(resp);
    CHECK(body != NULL);
    CHECK(strstr(body, "secret123") == NULL);    /* never leak */
    CHECK(strstr(body, "webpw") == NULL);
    CHECK(strstr(body, "\"broker_pass\":\"********\"") != NULL);
    CHECK(strstr(body, "\"web_pass\":\"********\"") != NULL);

    /* Empty pass is empty string, not the mask. */
    cfg.broker_pass[0] = '\0';
    cfg.web_pass[0]    = '\0';
    rn = mkreq(req, sizeof(req), "GET", "/api/config", NULL, NULL);
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    body = body_of(resp);
    CHECK(strstr(body, "\"broker_pass\":\"\"") != NULL);
    CHECK(strstr(body, "\"web_pass\":\"\"") != NULL);
    (void)wn;
}

static void test_config_post_partial_keeps_pass(void)
{
    /* Use a temp config file path. */
    const char *path = "/tmp/delta-bridge-test-cfg-1.conf";
    unlink(path);

    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);
    ws.conf_path = path;
    snprintf(cfg.broker_pass, sizeof(cfg.broker_pass), "originalpass");

    /* POST only broker_host — broker_pass must survive. */
    char req[512], resp[8192];
    size_t rn = mkreq(req, sizeof(req),
                      "POST", "/api/config",
                      "Content-Type: application/x-www-form-urlencoded\r\n",
                      "broker_host=10.1.1.1");
    size_t wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    CHECK_STR(cfg.broker_host, "10.1.1.1");
    CHECK_STR(cfg.broker_pass, "originalpass");

    /* Now POST broker_pass with the mask sentinel — must NOT overwrite. */
    rn = mkreq(req, sizeof(req),
               "POST", "/api/config",
               "Content-Type: application/x-www-form-urlencoded\r\n",
               "broker_host=10.1.1.2&broker_pass=********");
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    CHECK_STR(cfg.broker_pass, "originalpass");
    CHECK_STR(cfg.broker_host, "10.1.1.2");

    /* POST a real new pass — overrides. */
    rn = mkreq(req, sizeof(req),
               "POST", "/api/config",
               "Content-Type: application/x-www-form-urlencoded\r\n",
               "broker_pass=newpass");
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    CHECK_STR(cfg.broker_pass, "newpass");

    /* File should now exist and have key=value lines. */
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (f) {
        char buf[2048] = {0};
        fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(strstr(buf, "broker_host  = 10.1.1.2") != NULL);
        CHECK(strstr(buf, "broker_pass  = newpass") != NULL);
    }
    unlink(path);
    (void)wn;
}

static void test_config_post_validation(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);
    ws.conf_path = "/tmp/delta-bridge-test-cfg-validate.conf";
    unlink(ws.conf_path);

    char req[512], resp[8192];
    /* broker_port out of range */
    size_t rn = mkreq(req, sizeof(req),
                      "POST", "/api/config",
                      "Content-Type: application/x-www-form-urlencoded\r\n",
                      "broker_port=70000");
    size_t wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 400);

    /* poll_hz < 1 */
    rn = mkreq(req, sizeof(req),
               "POST", "/api/config",
               "Content-Type: application/x-www-form-urlencoded\r\n",
               "poll_hz=0");
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 400);

    /* web_port bad */
    rn = mkreq(req, sizeof(req),
               "POST", "/api/config",
               "Content-Type: application/x-www-form-urlencoded\r\n",
               "web_port=-5");
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 400);

    /* web_enable garbage */
    rn = mkreq(req, sizeof(req),
               "POST", "/api/config",
               "Content-Type: application/x-www-form-urlencoded\r\n",
               "web_enable=maybe");
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 400);

    /* valid web_* keys all together */
    rn = mkreq(req, sizeof(req),
               "POST", "/api/config",
               "Content-Type: application/x-www-form-urlencoded\r\n",
               "web_enable=true&web_port=9090&web_user=u&web_pass=p");
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    CHECK_EQ(cfg.web_enable, 1);
    CHECK_EQ(cfg.web_port, 9090);
    CHECK_STR(cfg.web_user, "u");
    CHECK_STR(cfg.web_pass, "p");
    unlink(ws.conf_path);
    (void)wn;
}

/* --- 7. control endpoints --------------------------------------------- */

static void test_control_rated_amps(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);

    char req[512], resp[8192];
    size_t rn = mkreq(req, sizeof(req),
                      "POST", "/api/control/rated_amps",
                      "Content-Type: application/x-www-form-urlencoded\r\n",
                      "amps=22");
    size_t wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 22);
    CHECK(strstr(body_of(resp), "\"rated_amps\":22") != NULL);

    /* out of range */
    rn = mkreq(req, sizeof(req),
               "POST", "/api/control/rated_amps",
               "Content-Type: application/x-www-form-urlencoded\r\n",
               "amps=99");
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 400);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 22);   /* unchanged */

    /* missing amps */
    rn = mkreq(req, sizeof(req),
               "POST", "/api/control/rated_amps",
               "Content-Type: application/x-www-form-urlencoded\r\n",
               "wrong=18");
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 400);
    (void)wn;
}

static void test_control_authorize(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);

    char req[512], resp[8192];
    size_t rn = mkreq(req, sizeof(req),
                      "POST", "/api/control/authorize",
                      "Content-Type: application/x-www-form-urlencoded\r\n",
                      "state=ON");
    size_t wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    CHECK_EQ(shmem_u8(&sm, OFF_USER_STATE), 1);

    rn = mkreq(req, sizeof(req),
               "POST", "/api/control/authorize",
               "Content-Type: application/x-www-form-urlencoded\r\n",
               "state=OFF");
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    CHECK_EQ(shmem_u8(&sm, OFF_USER_STATE), 0);

    /* bad */
    rn = mkreq(req, sizeof(req),
               "POST", "/api/control/authorize",
               "Content-Type: application/x-www-form-urlencoded\r\n",
               "state=on");
    wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 400);
    (void)wn;
}

static void test_control_clear_faults(void)
{
    struct config cfg;
    struct shmem sm; make_shm(&sm);
    struct web_server ws;
    mk_ws_noauth(&ws, &cfg, &sm);
    shmem_write_u32_le(&sm, OFF_ALARM_BITMAP, 0xDEADBEEFu);

    char req[256], resp[8192];
    size_t rn = mkreq(req, sizeof(req),
                      "POST", "/api/control/clear_faults",
                      "Content-Type: application/x-www-form-urlencoded\r\n",
                      "");
    size_t wn = web_handle_request(&ws, req, rn, resp, sizeof(resp));
    CHECK_EQ(status_of(resp), 200);
    CHECK_EQ(shmem_u32_le(&sm, OFF_ALARM_BITMAP), 0u);
    (void)wn;
}

int main(void)
{
    test_parser_basic();
    test_parser_bad_version();
    test_parser_no_double_crlf();
    test_parser_oversize();
    test_parser_post_with_body();
    test_routing();
    test_auth();
    test_helpers();
    test_state_json();
    test_config_get_masks_passwords();
    test_config_post_partial_keeps_pass();
    test_config_post_validation();
    test_control_rated_amps();
    test_control_authorize();
    test_control_clear_faults();
    TEST_MAIN_END();
}
