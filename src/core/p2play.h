/*
 * P2Play core, C API: for pPlay (the core inside the player) and the wrappers (the daemon,
 * the Android service). The core serves HTTP (the TorrServer MatriX API and the streams);
 * the host talks to it there.
 */
#ifndef P2PLAY_H
#define P2PLAY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*p2play_log_fn)(const char *line, void *user);

typedef struct p2play_config {
    const char *data_dir;   /* library, settings, metadata, disk storage */
    int http_port;          /* 0: 8090 (TorrServer's); -1: any free port */
    int console;            /* nonzero: console limits (no LSD/UPnP, IPv4 only, fewer connections) */
    const char *platform;   /* shown by /echo next to the version; may be NULL */
    p2play_log_fn log;      /* may be NULL */
    void *log_user;
    /* Optional: override the fixed owner's password on each embedded start. */
    const char *owner_password;
} p2play_config;

/* 0 on success; one core per process */
int p2play_start(const p2play_config *config);

/* the HTTP port, 0 if not running */
int p2play_http_port(void);

/* streams (and preloads) being served: the host keeps the device awake while nonzero */
int p2play_active_streams(void);

/* nonzero once a client asked the server to stop (TorrServer's /shutdown); the host decides */
int p2play_shutdown_requested(void);

void p2play_stop(void);

const char *p2play_version(void);

#ifdef __cplusplus
}
#endif

#endif
