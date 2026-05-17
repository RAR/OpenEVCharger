#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void config_defaults(struct config *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->broker_host,  sizeof(c->broker_host),  "127.0.0.1");
    c->broker_port = 1883;
    snprintf(c->topic_prefix, sizeof(c->topic_prefix), "delta-bridge");
    c->poll_hz = 1;
    snprintf(c->log_level, sizeof(c->log_level), "info");
    /* Matches the /root/RFID wrapper's `>> .../bridge-boot.log 2>&1`
     * redirect — that's where stderr actually lands on the bench, so
     * /api/log reads it by default. Override in delta-bridge.conf if
     * your deployment redirects elsewhere. */
    snprintf(c->log_path,  sizeof(c->log_path),
             "/Storage/delta-bridge/bridge-boot.log");
    c->write_enable = 0;        /* v0.3: opt-in; default preserves v0.2 RO. */
    c->web_enable   = 0;        /* v0.4: opt-in; off by default. */
    c->web_port     = 8080;
    c->rfid_enable     = 0;     /* v0.6: opt-in; on means delta-bridge replaces /root/RFID. */
    snprintf(c->rfid_port, sizeof(c->rfid_port), "/dev/ttyAMA4");
    c->meter_v_scale   = 60.0;  /* see config.h comment */
}

/* Trim leading/trailing ASCII whitespace in place; returns the new start. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' '  || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static void set_str(char *dst, const char *src)
{
    snprintf(dst, CONFIG_STR_MAX, "%s", src);
}

/* Parse a permissive boolean. Returns 1 for true / on / yes / 1, 0 for
 * false / off / no / 0, and -1 if the value is unrecognised. */
static int parse_bool(const char *v)
{
    if (!strcasecmp(v, "true")  || !strcasecmp(v, "yes") ||
        !strcasecmp(v, "on")    || !strcmp(v, "1"))
        return 1;
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") ||
        !strcasecmp(v, "off")   || !strcmp(v, "0"))
        return 0;
    return -1;
}

int config_parse(struct config *c, const char *text)
{
    if (!text)
        return 0;
    char line[256];
    const char *p = text;
    int lineno = 0;
    while (*p) {
        lineno++;
        size_t i = 0;
        while (*p && *p != '\n' && i < sizeof(line) - 1)
            line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n')
            p++;

        char *s = trim(line);
        if (*s == '\0' || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if      (!strcmp(key, "broker_host"))  set_str(c->broker_host, val);
        else if (!strcmp(key, "broker_port"))  c->broker_port = atoi(val);
        else if (!strcmp(key, "broker_user"))  set_str(c->broker_user, val);
        else if (!strcmp(key, "broker_pass"))  set_str(c->broker_pass, val);
        else if (!strcmp(key, "topic_prefix")) set_str(c->topic_prefix, val);
        else if (!strcmp(key, "device_id"))    set_str(c->device_id, val);
        else if (!strcmp(key, "poll_hz"))      c->poll_hz = atoi(val);
        else if (!strcmp(key, "log_level"))    set_str(c->log_level, val);
        else if (!strcmp(key, "log_path"))     set_str(c->log_path, val);
        else if (!strcmp(key, "write_enable")) {
            int b = parse_bool(val);
            if (b < 0) {
                fprintf(stderr,
                        "delta-bridge: config: invalid bool '%s' for "
                        "'write_enable' at line %d, defaulting to false\n",
                        val, lineno);
                c->write_enable = 0;
            } else {
                c->write_enable = b;
            }
        }
        else if (!strcmp(key, "web_enable")) {
            int b = parse_bool(val);
            if (b < 0) {
                fprintf(stderr,
                        "delta-bridge: config: invalid bool '%s' for "
                        "'web_enable' at line %d, defaulting to false\n",
                        val, lineno);
                c->web_enable = 0;
            } else {
                c->web_enable = b;
            }
        }
        else if (!strcmp(key, "web_port"))  c->web_port = atoi(val);
        else if (!strcmp(key, "web_user"))  set_str(c->web_user, val);
        else if (!strcmp(key, "web_pass"))  set_str(c->web_pass, val);
        else if (!strcmp(key, "rfid_enable")) {
            int b = parse_bool(val);
            if (b < 0) {
                fprintf(stderr,
                        "delta-bridge: config: invalid bool '%s' for "
                        "'rfid_enable' at line %d, defaulting to false\n",
                        val, lineno);
                c->rfid_enable = 0;
            } else {
                c->rfid_enable = b;
            }
        }
        else if (!strcmp(key, "rfid_port"))   set_str(c->rfid_port, val);
        else if (!strcmp(key, "meter_v_scale")) {
            double d = strtod(val, NULL);
            if (d > 0.0 && d < 10000.0)
                c->meter_v_scale = d;
            else
                fprintf(stderr,
                        "delta-bridge: config: 'meter_v_scale' value '%s' out "
                        "of range (must be > 0 and < 10000) at line %d, "
                        "keeping current %.3f\n", val, lineno, c->meter_v_scale);
        }
        else if (!strcmp(key, "rfid_kill_stock") ||
                 !strcmp(key, "rfid_poll_hz")    ||
                 !strcmp(key, "rfid_mode")) {
            /* v0.6 removed these. Warn so users notice and clean up their
             * conf, but don't fail — old conf files should keep working. */
            fprintf(stderr,
                    "delta-bridge: config: '%s' is deprecated in v0.6 and "
                    "ignored (line %d). v0.6 always replaces stock /root/RFID "
                    "and polls at the reader's natural cadence (~9 Hz).\n",
                    key, lineno);
        }
        else {
            /* Unknown keys are non-fatal but surfaced — the M0 bench session
             * called out that silent ignoring made typos hard to spot. */
            fprintf(stderr,
                    "delta-bridge: config: unknown key '%s' at line %d, "
                    "ignored\n", key, lineno);
        }
    }
    if (c->poll_hz < 1)
        c->poll_hz = 1;
    if (c->web_port < 1 || c->web_port > 65535)
        c->web_port = 8080;
    if (!(c->meter_v_scale > 0.0 && c->meter_v_scale < 10000.0))
        c->meter_v_scale = 60.0;
    return 0;
}

int config_load(struct config *c, const char *path)
{
    config_defaults(c);
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return config_parse(c, buf);
}
