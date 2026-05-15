#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(struct config *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->broker_host,  sizeof(c->broker_host),  "127.0.0.1");
    c->broker_port = 1883;
    snprintf(c->topic_prefix, sizeof(c->topic_prefix), "delta-bridge");
    c->poll_hz = 1;
    snprintf(c->log_level, sizeof(c->log_level), "info");
    snprintf(c->log_path,  sizeof(c->log_path),  "/Storage/delta-bridge.log");
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

int config_parse(struct config *c, const char *text)
{
    char line[256];
    const char *p = text;
    while (*p) {
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
        /* unknown key: ignored */
    }
    if (c->poll_hz < 1)
        c->poll_hz = 1;
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
