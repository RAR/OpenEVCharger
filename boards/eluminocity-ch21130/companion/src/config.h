/* delta-bridge config — parsed from /Storage/delta-bridge.conf (key = value). */
#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_STR_MAX 128

struct config {
    char broker_host[CONFIG_STR_MAX];
    int  broker_port;
    char broker_user[CONFIG_STR_MAX];   /* empty = no auth */
    char broker_pass[CONFIG_STR_MAX];
    char topic_prefix[CONFIG_STR_MAX];
    char device_id[CONFIG_STR_MAX];     /* empty = derive at runtime */
    int  poll_hz;
    char log_level[CONFIG_STR_MAX];
    char log_path[CONFIG_STR_MAX];
    /* v0.3 write-controls master switch. 0 = read-only (v0.2 behaviour);
     * 1 = bridge does shmem RW attach + MQTT subscribe + command dispatch. */
    int  write_enable;
};

/* Reset `c` to built-in defaults. */
void config_defaults(struct config *c);

/* Parse `text` (the whole config file) into `c`, overriding defaults for any
 * key present. Unknown keys log a warning to stderr but are not fatal. Returns
 * 0 (always succeeds — a missing or partial file just means defaults). Call
 * config_defaults() first. */
int  config_parse(struct config *c, const char *text);

/* Read `path` and parse it. Returns 0 on success, -1 if the file cannot be
 * opened (caller may proceed with defaults). Calls config_defaults() itself. */
int  config_load(struct config *c, const char *path);

#endif /* CONFIG_H */
