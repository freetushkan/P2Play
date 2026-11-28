/*
 * P2Play core, C API: for pPlay (the core inside the player) and the wrappers (the daemon,
 * the Android service). The core serves the native P2Play REST API and media streams;
 * the host talks to it there.
 */
#ifndef P2PLAY_H
#define P2PLAY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*p2play_log_fn)(const char *line, void *user);
typedef int (*p2play_http_get_fn)(const char *url, const char *pinned_resolve, char *body, size_t body_capacity, size_t *body_size, char *content_type, size_t content_type_capacity, char *location, size_t location_capacity, char *error, size_t error_capacity, void *user);

typedef struct p2play_config {
    const char *data_dir;   /* library, settings, metadata, disk storage */
    int http_port;          /* 0: 8660; -1: any free port */
    int console;            /* nonzero: console limits (no LSD/UPnP, IPv4 only, fewer connections) */
    const char *platform;   /* shown by /echo next to the version; may be NULL */
    p2play_log_fn log;      /* may be NULL */
    void *log_user;
    /* Optional: override the fixed owner's password on each embedded start. */
    const char *owner_password;
    p2play_http_get_fn http_get;
    void *http_user;
    const char *http_prefix; /* optional URL path prefix, e.g. "/media/p2play" */
    int loopback_only;       /* nonzero: bind the HTTP listener to 127.0.0.1 only */
} p2play_config;

/* 0 on success; one core per process */
int p2play_start(const p2play_config *config);

/* the HTTP port, 0 if not running */
int p2play_http_port(void);

/* streams (and preloads) being served; nonzero while media operations are active */
int p2play_active_streams(void);

/* accepted HTTP requests, including stream responses, which have not finished yet */
int p2play_active_requests(void);

/* stop accepting new HTTP connections but let already accepted requests finish */
void p2play_stop_accepting(void);

/* nonzero reserved for host lifecycle integration; the host decides */
int p2play_shutdown_requested(void);

void p2play_stop(void);

const char *p2play_version(void);

#ifdef __cplusplus
}
#endif

#endif
