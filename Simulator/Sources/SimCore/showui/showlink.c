/**
 * showlink.c — StageWand <-> StageWizard link engine. Portable C.
 *
 * Single-threaded, non-blocking BSD sockets; the same file compiles on macOS
 * (simulator) and ESP-IDF/lwIP (firmware).
 *
 * Commands out:  OSC 1.0 address-only messages over UDP.
 * State in, two paths, OSC preferred:
 *   1. Proposed OSC feedback (docs/stagewizard-osc-requests.html): we send
 *      /stagewand/ping every second as an implicit subscription; the host
 *      replies to our source address with /stagewizard/status/... messages.
 *      Ingested passively on the same UDP socket.
 *   2. Fallback: the web remote's GET /status polled at 2 Hz over a
 *      non-blocking TCP state machine — automatically suppressed while OSC
 *      feedback is fresh, so the two host versions interoperate seamlessly.
 */
#include "showlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define PING_INTERVAL_MS   1000u
#define POLL_INTERVAL_MS   500u
#define HTTP_TIMEOUT_MS    1500u
/* The host pushes OSC feedback only on change (elapsed only while running),
 * so a quiet-but-alive host can be silent for a while — allow 10 s before
 * considering OSC stale. HTTP polls are request/response, so 2.5 s is right
 * there. A host-side heartbeat (requested) will let this window shrink. */
#define OSC_FRESH_MS       10000u
#define HTTP_FRESH_MS      2500u
#define ELAPSED_FRESH_MS   2000u

typedef enum { HTTP_IDLE, HTTP_CONNECTING, HTTP_SENDING, HTTP_READING } http_phase_t;

static struct {
    bool enabled;
    char host[SHOWLINK_HOST_MAX];
    uint16_t osc_port, http_port;
    struct sockaddr_in osc_addr, http_addr;
    bool addr_ok;

    int osc_fd;
    uint32_t last_ping_ms;
    bool ping_ever;
    /* OSC path-death detection. The UDP socket is connect()ed to the host,
     * so when the OSC listener goes away the ICMP port-unreachable surfaces
     * as ECONNREFUSED — that, not data staleness, is the "link lost" signal
     * (feedback is change-only, so a quiet host is healthy, not dead).
     * macOS quirk: the queued error is returned on ALTERNATE sends (OK,
     * ECONNREFUSED, OK, ...), so a successful send() proves nothing and must
     * NOT clear the count — only actually received data does. Errors count
     * only while recent, so a transient blip self-heals. */
    int osc_err_count;
    bool osc_err_ever; uint32_t osc_err_ms;

    int http_fd;
    http_phase_t http_phase;
    uint32_t http_started_ms;
    uint32_t last_poll_ms;
    bool poll_ever;
    char http_req[160];
    size_t http_req_len, http_sent;
    char http_buf[2048];
    size_t http_len;

    bool osc_seen;  uint32_t osc_ms;   /* last good OSC status feedback */
    bool http_seen; uint32_t http_ms;  /* last good HTTP /status */
    uint32_t now_ms;                   /* clock as of the latest tick */

    char sb_number[SHOWLINK_NUM_MAX];
    char sb_name[SHOWLINK_NAME_MAX];
    int32_t running_count;
    bool show_mode, panicking;

    int32_t window_index, window_total;
    char prev_number[SHOWLINK_NUM_MAX], prev_name[SHOWLINK_NAME_MAX];
    char next_number[SHOWLINK_NUM_MAX], next_name[SHOWLINK_NAME_MAX];
    char notes[SHOWLINK_NOTES_MAX];
    float elapsed_s, duration_s;
    bool elapsed_seen; uint32_t elapsed_ms;
} L = { .osc_fd = -1, .http_fd = -1 };

/* ---------- small utils ---------------------------------------------------- */

static uint32_t age_of(bool seen, uint32_t then, uint32_t now)
{
    return seen ? (uint32_t)(now - then) : UINT32_MAX;
}

static void note_osc_error(void)
{
    if (L.osc_err_count < 100) L.osc_err_count++;
    L.osc_err_ever = true;
    L.osc_err_ms = L.now_ms;
}

/* Dead = repeated ICMP-refused errors that are still recent. */
static bool osc_path_dead(void)
{
    return L.osc_err_count >= 2 &&
           age_of(L.osc_err_ever, L.osc_err_ms, L.now_ms) < 3000u;
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    /* macOS: a send() on a reset TCP socket (e.g. the web remote is off and
     * the connect was refused) raises SIGPIPE and kills the process unless
     * suppressed. lwIP has no SIGPIPE, so this is a host-platform guard. */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

static void close_http(void)
{
    if (L.http_fd >= 0) { close(L.http_fd); L.http_fd = -1; }
    L.http_phase = HTTP_IDLE;
    L.http_len = 0;
    L.http_sent = 0;
}

static void close_all(void)
{
    if (L.osc_fd >= 0) { close(L.osc_fd); L.osc_fd = -1; }
    close_http();
}

/* ---------- OSC encoding (address-only messages) --------------------------- */

static void osc_send_address(const char *addr)
{
    if (!L.enabled || L.osc_fd < 0 || !L.addr_ok) return;
    uint8_t buf[96];
    size_t alen = strlen(addr);
    size_t apad = ((alen + 1) + 3) & ~(size_t)3;   /* string + NUL, 4-aligned */
    if (apad + 4 > sizeof(buf)) return;
    memset(buf, 0, apad + 4);
    memcpy(buf, addr, alen);
    buf[apad] = ',';                                /* ",\0\0\0": no arguments */
    ssize_t n = send(L.osc_fd, buf, apad + 4, 0);   /* connected UDP socket */
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        note_osc_error();
    }
    /* A successful send is NOT evidence of life (see osc_err_count note). */
}

/* ---------- OSC decoding (proposed status feedback) ------------------------ */

static const uint8_t *osc_read_str(const uint8_t *p, const uint8_t *end, const char **out)
{
    const uint8_t *nul = memchr(p, 0, (size_t)(end - p));
    if (!nul) return NULL;
    size_t consumed = (size_t)(nul - p) + 1;
    size_t padded = (consumed + 3) & ~(size_t)3;
    if (p + padded > end) return NULL;
    *out = (const char *)p;
    return p + padded;
}

static const uint8_t *osc_read_i32(const uint8_t *p, const uint8_t *end, int32_t *out)
{
    if (p + 4 > end) return NULL;
    uint32_t raw = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    *out = (int32_t)raw;
    return p + 4;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void osc_ingest(const uint8_t *p, size_t len, uint32_t now_ms)
{
    const uint8_t *end = p + len;
    const char *addr, *tags;
    p = osc_read_str(p, end, &addr);
    if (!p || addr[0] != '/') return;
    p = osc_read_str(p, end, &tags);
    if (!p || tags[0] != ',') return;

    /* Decode up to 6 args of the i/f/s shapes the status feedback uses,
     * stored by argument position. */
    enum { MAX_ARGS = 6 };
    int32_t iv[MAX_ARGS] = { 0 };
    float fv[MAX_ARGS] = { 0 };
    const char *sv[MAX_ARGS];
    for (int i = 0; i < MAX_ARGS; i++) sv[i] = "";
    int argc = 0;
    for (const char *t = tags + 1; *t && argc < MAX_ARGS; t++) {
        if (*t == 'i') {
            if (!(p = osc_read_i32(p, end, &iv[argc]))) return;
        } else if (*t == 's') {
            if (!(p = osc_read_str(p, end, &sv[argc]))) return;
        } else if (*t == 'f') {
            int32_t bits;
            if (!(p = osc_read_i32(p, end, &bits))) return;
            union { int32_t i; float f; } u = { .i = bits };
            fv[argc] = u.f;
        } else {
            return; /* unknown tag: widths untrustworthy from here on */
        }
        argc++;
    }

    if (strcmp(addr, "/stagewizard/status/standingby") == 0) {
        copy_str(L.sb_number, sizeof(L.sb_number), sv[0]);
        copy_str(L.sb_name, sizeof(L.sb_name), sv[1]);
    } else if (strcmp(addr, "/stagewizard/status/running") == 0) {
        L.running_count = iv[0];
    } else if (strcmp(addr, "/stagewizard/status/panic") == 0) {
        L.panicking = (iv[0] != 0);
    } else if (strcmp(addr, "/stagewizard/status/showmode") == 0) {
        L.show_mode = (iv[0] != 0);
    } else if (strcmp(addr, "/stagewizard/status/window") == 0) {
        L.window_index = iv[0];
        L.window_total = iv[1];
        copy_str(L.prev_number, sizeof(L.prev_number), sv[2]);
        copy_str(L.prev_name, sizeof(L.prev_name), sv[3]);
        copy_str(L.next_number, sizeof(L.next_number), sv[4]);
        copy_str(L.next_name, sizeof(L.next_name), sv[5]);
    } else if (strcmp(addr, "/stagewizard/status/notes") == 0) {
        copy_str(L.notes, sizeof(L.notes), sv[0]);
    } else if (strcmp(addr, "/stagewizard/status/elapsed") == 0) {
        L.elapsed_s = fv[0];
        L.duration_s = fv[1];
        L.elapsed_seen = true;
        L.elapsed_ms = now_ms;
    } else {
        return; /* not a status message: does not refresh freshness */
    }
    L.osc_seen = true;
    L.osc_ms = now_ms;
}

static void osc_recv_all(uint32_t now_ms)
{
    if (L.osc_fd < 0) return;
    uint8_t buf[512];
    for (int i = 0; i < 16; i++) {   /* bounded work per tick */
        ssize_t n = recv(L.osc_fd, buf, sizeof(buf), 0);
        if (n < 0) {
            /* A queued ICMP unreachable can surface on recv instead of send. */
            if (errno == ECONNREFUSED || errno == ECONNRESET) {
                note_osc_error();
                continue;
            }
            break;
        }
        if (n == 0) break;
        L.osc_err_count = 0;   /* real bytes from the host: the path lives */
        if ((size_t)n >= 8 && memcmp(buf, "#bundle\0", 8) == 0) continue;
        osc_ingest(buf, (size_t)n, now_ms);
    }
}

/* ---------- minimal JSON field extraction (fixed /status payload) ---------- */

static const char *json_find(const char *body, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    return p;
}

static void json_string(const char *body, const char *key, char *dst, size_t cap)
{
    dst[0] = '\0';
    const char *p = json_find(body, key);
    if (!p) return;
    if (strncmp(p, "null", 4) == 0) return;
    if (*p != '"') return;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n < cap - 1) {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
                case 'n': c = ' '; break;
                case 't': c = ' '; break;
                case 'u':                    /* \uXXXX: not worth decoding on a wand */
                    c = '?';
                    for (int k = 0; k < 4 && *p; k++) p++;
                    break;
                default: c = e; break;       /* \" \\ \/ */
            }
        }
        dst[n++] = c;
    }
    dst[n] = '\0';
}

static int32_t json_int(const char *body, const char *key, int32_t fallback)
{
    const char *p = json_find(body, key);
    if (!p) return fallback;
    return (int32_t)strtol(p, NULL, 10);
}

static bool json_bool(const char *body, const char *key, bool fallback)
{
    const char *p = json_find(body, key);
    if (!p) return fallback;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return fallback;
}

static void http_finish(uint32_t now_ms)
{
    L.http_buf[L.http_len < sizeof(L.http_buf) ? L.http_len : sizeof(L.http_buf) - 1] = '\0';
    const char *body = strstr(L.http_buf, "\r\n\r\n");
    if (body && strncmp(L.http_buf, "HTTP/1.1 200", 12) == 0) {
        body += 4;
        json_string(body, "standingByNumber", L.sb_number, sizeof(L.sb_number));
        json_string(body, "standingByName", L.sb_name, sizeof(L.sb_name));
        L.running_count = json_int(body, "runningCount", L.running_count);
        L.show_mode = json_bool(body, "showMode", L.show_mode);
        L.panicking = json_bool(body, "panicking", L.panicking);
        L.http_seen = true;
        L.http_ms = now_ms;
    }
    close_http();
}

/* ---------- HTTP /status poll state machine -------------------------------- */

static void http_start(uint32_t now_ms)
{
    L.http_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (L.http_fd < 0) return;
    set_nonblocking(L.http_fd);
    L.http_req_len = (size_t)snprintf(L.http_req, sizeof(L.http_req),
        "GET /status HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", L.host);
    L.http_sent = 0;
    L.http_len = 0;
    L.http_started_ms = now_ms;
    int rc = connect(L.http_fd, (struct sockaddr *)&L.http_addr, sizeof(L.http_addr));
    if (rc == 0) L.http_phase = HTTP_SENDING;
    else if (errno == EINPROGRESS) L.http_phase = HTTP_CONNECTING;
    else close_http();
}

static void http_advance(uint32_t now_ms)
{
    if (L.http_phase == HTTP_IDLE) return;
    if ((uint32_t)(now_ms - L.http_started_ms) > HTTP_TIMEOUT_MS) { close_http(); return; }

    if (L.http_phase == HTTP_CONNECTING) {
        /* A non-blocking connect is done when the socket turns writable;
         * probing with a zero-length send avoids select/poll portability
         * differences between macOS and lwIP. */
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(L.http_fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0) {
            if (err == 0) {
                /* Not connected yet? send() below tells us via EAGAIN/ENOTCONN. */
                L.http_phase = HTTP_SENDING;
            } else if (err != EINPROGRESS && err != EALREADY) {
                close_http();
                return;
            }
        }
    }

    if (L.http_phase == HTTP_SENDING) {
        ssize_t n = send(L.http_fd, L.http_req + L.http_sent,
                         L.http_req_len - L.http_sent, 0);
        if (n > 0) {
            L.http_sent += (size_t)n;
            if (L.http_sent >= L.http_req_len) L.http_phase = HTTP_READING;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != ENOTCONN && errno != EINPROGRESS && errno != EALREADY) {
            close_http();
            return;
        }
    }

    if (L.http_phase == HTTP_READING) {
        for (int i = 0; i < 8; i++) {
            if (L.http_len >= sizeof(L.http_buf) - 1) { http_finish(now_ms); return; }
            ssize_t n = recv(L.http_fd, L.http_buf + L.http_len,
                             sizeof(L.http_buf) - 1 - L.http_len, 0);
            if (n > 0) {
                L.http_len += (size_t)n;
            } else if (n == 0) {          /* server closed: response complete */
                http_finish(now_ms);
                return;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) close_http();
                return;
            }
        }
    }
}

/* ---------- public API ----------------------------------------------------- */

void showlink_init(void)
{
    L.osc_fd = -1;
    L.http_fd = -1;
    L.enabled = false;
    L.http_phase = HTTP_IDLE;
}

void showlink_configure(const char *host_ip, uint16_t osc_port,
                        uint16_t http_port, bool enabled)
{
    close_all();
    L.enabled = false;
    L.addr_ok = false;
    L.osc_seen = false;
    L.http_seen = false;
    L.ping_ever = false;
    L.poll_ever = false;
    L.sb_number[0] = '\0';
    L.sb_name[0] = '\0';
    L.running_count = 0;
    L.show_mode = false;
    L.panicking = false;
    L.window_index = 0;
    L.window_total = 0;
    L.prev_number[0] = '\0'; L.prev_name[0] = '\0';
    L.next_number[0] = '\0'; L.next_name[0] = '\0';
    L.notes[0] = '\0';
    L.elapsed_s = 0; L.duration_s = 0; L.elapsed_seen = false;

    if (!enabled || !host_ip || !host_ip[0]) return;

    copy_str(L.host, sizeof(L.host), host_ip);
    L.osc_port = osc_port ? osc_port : SHOWLINK_DEFAULT_OSC_PORT;
    L.http_port = http_port ? http_port : SHOWLINK_DEFAULT_HTTP_PORT;

    struct in_addr ia;
    if (inet_pton(AF_INET, L.host, &ia) != 1) return;

    memset(&L.osc_addr, 0, sizeof(L.osc_addr));
    L.osc_addr.sin_family = AF_INET;
    L.osc_addr.sin_addr = ia;
    L.osc_addr.sin_port = htons(L.osc_port);
    L.http_addr = L.osc_addr;
    L.http_addr.sin_port = htons(L.http_port);
    L.addr_ok = true;

    L.osc_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (L.osc_fd < 0) return;
    set_nonblocking(L.osc_fd);
    /* Connected UDP: replies filter to the host only, and — the important
     * part — ICMP port-unreachable (host app quit) surfaces as ECONNREFUSED
     * on our sends, which is the link-death detector. */
    if (connect(L.osc_fd, (struct sockaddr *)&L.osc_addr, sizeof(L.osc_addr)) < 0) {
        close(L.osc_fd);
        L.osc_fd = -1;
        return;
    }
    L.osc_err_count = 0;
    L.osc_err_ever = false;

    L.enabled = true;
}

void showlink_tick(uint32_t now_ms)
{
    L.now_ms = now_ms;
    if (!L.enabled) return;

    /* Implicit subscription keepalive (harmless on hosts without feedback:
     * unknown addresses are ignored silently). */
    if (!L.ping_ever || (uint32_t)(now_ms - L.last_ping_ms) >= PING_INTERVAL_MS) {
        osc_send_address("/stagewand/ping");
        L.last_ping_ms = now_ms;
        L.ping_ever = true;
    }

    osc_recv_all(now_ms);

    /* HTTP fallback poll — suppressed while the OSC path is healthy (we've
     * received feedback and no recent refusals); a quiet host needs no polls. */
    bool osc_healthy = L.osc_seen && !osc_path_dead();
    if (!osc_healthy) {
        if (L.http_phase == HTTP_IDLE &&
            (!L.poll_ever || (uint32_t)(now_ms - L.last_poll_ms) >= POLL_INTERVAL_MS)) {
            L.last_poll_ms = now_ms;
            L.poll_ever = true;
            http_start(now_ms);
        }
    }
    http_advance(now_ms);
}

void showlink_get_state(showlink_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->enabled = L.enabled;
    /* Ages vs. the clock of the latest tick — at 10 Hz ticking that is at
     * most ~100 ms stale, well inside the freshness windows.
     *
     * Online model: OSC feedback is change-only, so a quiet host is healthy.
     * The 1 Hz ping keeps the host's subscriber registry warm; the connected
     * UDP socket turns "OSC listener gone" into send errors. So the OSC path
     * is online once we have EVER received feedback and pings are landing —
     * regardless of how long the show state has been unchanged. */
    uint32_t osc_age = age_of(L.osc_seen, L.osc_ms, L.now_ms);
    uint32_t http_age = age_of(L.http_seen, L.http_ms, L.now_ms);
    bool osc_path = L.osc_seen && L.ping_ever && !osc_path_dead();
    bool http_fresh = http_age < HTTP_FRESH_MS;
    out->online = L.enabled && (osc_path || http_fresh);
    out->transport = !out->online ? SHOWLINK_TRANSPORT_NONE
                   : (osc_path ? SHOWLINK_TRANSPORT_OSC : SHOWLINK_TRANSPORT_HTTP);
    out->last_status_age_ms = osc_age < http_age ? osc_age : http_age;
    copy_str(out->host, sizeof(out->host), L.enabled ? L.host : "");
    copy_str(out->standing_by_number, sizeof(out->standing_by_number), L.sb_number);
    copy_str(out->standing_by_name, sizeof(out->standing_by_name), L.sb_name);
    out->running_count = L.running_count;
    out->show_mode = L.show_mode;
    out->panicking = L.panicking;
    out->window_index = L.window_index;
    out->window_total = L.window_total;
    copy_str(out->prev_number, sizeof(out->prev_number), L.prev_number);
    copy_str(out->prev_name, sizeof(out->prev_name), L.prev_name);
    copy_str(out->next_number, sizeof(out->next_number), L.next_number);
    copy_str(out->next_name, sizeof(out->next_name), L.next_name);
    copy_str(out->notes, sizeof(out->notes), L.notes);
    out->elapsed_s = L.elapsed_s;
    out->duration_s = L.duration_s;
    out->elapsed_fresh = age_of(L.elapsed_seen, L.elapsed_ms, L.now_ms) < ELAPSED_FRESH_MS;
}

void showlink_send_go(void)      { osc_send_address("/stagewizard/go"); }
void showlink_send_stopall(void) { osc_send_address("/stagewizard/stopall"); }
void showlink_send_panic(void)   { osc_send_address("/stagewizard/panic"); }
void showlink_send_next(void)    { osc_send_address("/stagewizard/next"); }
void showlink_send_prev(void)    { osc_send_address("/stagewizard/prev"); }
void showlink_send_toggle(void)  { osc_send_address("/stagewizard/toggle"); }

static void send_cue_command(const char *number, const char *suffix)
{
    if (!number || !number[0] || strchr(number, '/')) return;
    char addr[64];
    int n = snprintf(addr, sizeof(addr), "/stagewizard/cue/%s/%s", number, suffix);
    if (n <= 0 || (size_t)n >= sizeof(addr)) return;
    osc_send_address(addr);
}

void showlink_send_fire_cue(const char *number)   { send_cue_command(number, "fire"); }
void showlink_send_select_cue(const char *number) { send_cue_command(number, "select"); }
