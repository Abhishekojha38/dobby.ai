/*
 * serial_tool.c — Native serial port tool (termios, no external deps)
 *
 * Feature parity with serial_node.py:
 *   - Device registry (device.conf) resolved by name — LLM only needs device
 *   - Explicit port/baud/etc override when no named device is given
 *   - Raw termios 8N1, non-blocking reads, explicit deadline on every op
 *   - Input buffer flush on open (discard stale OS bytes)
 *   - ANSI/VT100 escape sequence stripping from output
 *   - Login sequence: configurable login_prompt, username, password
 *   - Per-command shell prompt detection for output boundary
 *   - Command echo stripping from captured output
 *   - Per-command timeout + overall login_timeout
 *   - Inter-command delay
 *   - Output cap (256 KB per command) — safe on embedded hosts
 *   - Structured JSON result: status, per-command results, elapsed_ms
 *   - Port always closed on all exit paths (normal, error, timeout)
 *   - Baud validation against POSIX constants
 *
 * Implementation notes:
 *   - Uses CLOCK_MONOTONIC for all deadlines — immune to wall-clock jumps.
 *   - select() with 50ms quantum for the read loop — no busy-spinning.
 *   - No signals, no threads — safe alongside Dobby's scheduler/gateway.
 *   - All heap allocations released before return.
 */
#define _GNU_SOURCE
#include "serial_tool.h"
#include "../../core/log.h"
#include "../../core/cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Constants */

#define SERIAL_MAX_OUTPUT_BYTES  (256 * 1024)  /* per-command cap          */
#define SERIAL_READ_CHUNK        512            /* bytes per read() call    */
#define SERIAL_SELECT_MS         50             /* select() quantum (ms)    */
#define SERIAL_MAX_DEVICES       64             /* entries in device.conf   */
#define SERIAL_MAX_COMMANDS      32             /* commands per tool call   */
#define DEVICE_CONF_KEY_LEN      64
#define DEVICE_CONF_VAL_LEN      256

/* Module state */

static char g_device_conf_path[512] = "";

/* Monotonic time helpers */

static double mono_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Device profile */

typedef struct {
    char name[DEVICE_CONF_KEY_LEN];
    char port[DEVICE_CONF_VAL_LEN];    /* /dev/ttyUSBx                    */
    int  baud;                          /* e.g. 115200                     */
    char username[DEVICE_CONF_VAL_LEN];
    char password[DEVICE_CONF_VAL_LEN];
    char login_prompt[DEVICE_CONF_VAL_LEN]; /* e.g. "m1700 login"         */
    char prompt[DEVICE_CONF_VAL_LEN];  /* shell prompt, e.g. "root@m1700" */
    bool no_login;
    int  timeout_sec;                   /* per-command                     */
    int  login_timeout_sec;
    int  inter_cmd_delay_ms;
} serial_device_t;

/* device.conf parser */

/*
 * Parse a single device.conf INI file.
 * Sections map to device names; keys map to fields.
 * Returns number of devices parsed into out[] (up to max_devices).
 */
static int parse_device_conf(const char *path,
                              serial_device_t *out, int max_devices) {
    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_DEBUG("serial: device.conf not found at %s", path);
        return 0;
    }

    int  count  = 0;
    int  cur    = -1;  /* index of current section in out[] */
    char line[512];

    while (fgets(line, sizeof(line), f)) {
        /* Trim leading whitespace */
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;

        /* Strip trailing newline / CR */
        size_t len = strlen(s);
        while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) s[--len] = '\0';

        if (!*s || *s == '#' || *s == ';') continue;

        /* [section] — new device entry */
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) continue;
            *end = '\0';
            const char *name = s + 1;
            if (count >= max_devices) break;
            cur = count++;
            memset(&out[cur], 0, sizeof(serial_device_t));
            strncpy(out[cur].name,  name,  sizeof(out[cur].name)  - 1);
            /* Defaults */
            out[cur].baud                = 115200;
            out[cur].timeout_sec         = 10;
            out[cur].login_timeout_sec   = 30;
            out[cur].inter_cmd_delay_ms  = 300;
            strncpy(out[cur].prompt, "#", sizeof(out[cur].prompt) - 1);
            continue;
        }

        if (cur < 0) continue;

        /* key = value */
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';

        /* Trim key and value */
        char *key = s;
        char *val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        while (*val == ' ' || *val == '\t') val++;
        /* Trim trailing spaces from key */
        char *kend = key + strlen(key) - 1;
        while (kend > key && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';

        serial_device_t *d = &out[cur];

        if      (!strcmp(key, "port"))             strncpy(d->port,          val, sizeof(d->port)-1);
        else if (!strcmp(key, "baud"))             d->baud = atoi(val);
        else if (!strcmp(key, "username"))         strncpy(d->username,      val, sizeof(d->username)-1);
        else if (!strcmp(key, "password"))         strncpy(d->password,      val, sizeof(d->password)-1);
        else if (!strcmp(key, "login_prompt"))     strncpy(d->login_prompt,  val, sizeof(d->login_prompt)-1);
        else if (!strcmp(key, "prompt"))           strncpy(d->prompt,        val, sizeof(d->prompt)-1);
        else if (!strcmp(key, "no_login"))         d->no_login = (!strcmp(val,"true")||!strcmp(val,"1")||!strcmp(val,"yes"));
        else if (!strcmp(key, "timeout"))          d->timeout_sec = atoi(val);
        else if (!strcmp(key, "login_timeout"))    d->login_timeout_sec = atoi(val);
        else if (!strcmp(key, "inter_cmd_delay")) {
            /* Accept float seconds, store as ms */
            double v = atof(val);
            d->inter_cmd_delay_ms = (int)(v * 1000.0);
        }
    }
    fclose(f);
    return count;
}

/*
 * Look up a device by name in device.conf.
 * Returns true and fills *out if found.
 */
static bool device_lookup(const char *name, serial_device_t *out) {
    if (!name || !*name) return false;

    serial_device_t devs[SERIAL_MAX_DEVICES];
    int n = parse_device_conf(g_device_conf_path, devs, SERIAL_MAX_DEVICES);

    for (int i = 0; i < n; i++) {
        if (!strcmp(devs[i].name, name)) {
            *out = devs[i];
            return true;
        }
    }
    return false;
}

/* Baud rate → termios constant */

static speed_t baud_to_speed(int baud) {
    switch (baud) {
        case     50: return B50;
        case     75: return B75;
        case    110: return B110;
        case    134: return B134;
        case    150: return B150;
        case    200: return B200;
        case    300: return B300;
        case    600: return B600;
        case   1200: return B1200;
        case   1800: return B1800;
        case   2400: return B2400;
        case   4800: return B4800;
        case   9600: return B9600;
        case  19200: return B19200;
        case  38400: return B38400;
        case  57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
        default:     return (speed_t)-1;
    }
}

/* Port open / configure */

/*
 * Open port in raw 8N1 mode, non-blocking reads.
 * Returns fd >= 0 on success, -1 on error (writes message to err_buf).
 */
static int serial_open(const char *port, int baud,
                        char *err_buf, size_t err_sz) {
    /* Validate device exists and is a character device */
    struct stat st;
    if (stat(port, &st) != 0) {
        snprintf(err_buf, err_sz, "Device not found: %s (%s)", port, strerror(errno));
        return -1;
    }
    if (!S_ISCHR(st.st_mode)) {
        snprintf(err_buf, err_sz, "%s is not a character device", port);
        return -1;
    }

    speed_t speed = baud_to_speed(baud);
    if (speed == (speed_t)-1) {
        snprintf(err_buf, err_sz, "Unsupported baud rate: %d", baud);
        return -1;
    }

    /* O_NOCTTY: don't make this our controlling terminal
     * O_NONBLOCK: open without waiting for carrier (DCD) */
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(err_buf, err_sz, "Cannot open %s: %s — try: sudo chmod a+rw %s",
                 port, strerror(errno), port);
        return -1;
    }

    /* Configure termios: raw 8N1, no flow control */
    struct termios tty;
    memset(&tty, 0, sizeof(tty));

    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    tty.c_cflag  = CS8 | CREAD | CLOCAL;   /* 8 bits, enable RX, ignore modem lines */
    tty.c_iflag  = 0;                        /* no input processing                   */
    tty.c_oflag  = 0;                        /* no output processing                  */
    tty.c_lflag  = 0;                        /* raw — no echo, no signals, no canon   */

    tty.c_cc[VMIN]  = 0;  /* return immediately if no data            */
    tty.c_cc[VTIME] = 0;  /* no inter-character timer                 */

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        snprintf(err_buf, err_sz, "tcsetattr failed on %s: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    /* Flush any stale data in OS RX/TX buffers */
    tcflush(fd, TCIOFLUSH);

    return fd;
}

/* ANSI escape stripping */

/*
 * Strip ANSI/VT100 escape sequences in-place.
 * Handles: ESC [ ... final-byte  and  ESC ( / ESC )
 * Returns number of bytes in stripped output (always <= input len).
 */
static size_t strip_ansi(const char *in, size_t in_len, char *out) {
    size_t r = 0, w = 0;
    while (r < in_len) {
        if (in[r] == '\x1b') {
            r++;
            if (r >= in_len) break;
            if (in[r] == '[') {
                /* CSI sequence: skip until final byte 0x40–0x7E */
                r++;
                while (r < in_len && (in[r] < 0x40 || in[r] > 0x7e)) r++;
                if (r < in_len) r++;
            } else if (in[r] == '(' || in[r] == ')') {
                /* Character set designation — skip one more byte */
                r++;
                if (r < in_len) r++;
            } else {
                r++;
            }
            continue;
        }
        out[w++] = in[r++];
    }
    return w;
}

/* Read loop with deadline */

/*
 * Read from fd until any string in patterns[] appears in the accumulated
 * buffer, or deadline_ms (monotonic) is reached.
 *
 * All accumulated output is ANSI-stripped before pattern matching and
 * before being stored in sb.
 *
 * Returns index of matched pattern (0-based), or -1 on timeout.
 * sb is appended to; caller initialises it.
 */
static int read_until(int fd,
                       const char **patterns, int n_patterns,
                       double deadline_ms,
                       strbuf_t *sb) {
    char  raw[SERIAL_READ_CHUNK];
    char  stripped[SERIAL_READ_CHUNK];

    while (mono_now_ms() < deadline_ms) {
        /* select() with short quantum so we check deadline regularly */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { 0, SERIAL_SELECT_MS * 1000 };
        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);

        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (sel > 0 && FD_ISSET(fd, &rfds)) {
            ssize_t n = read(fd, raw, sizeof(raw));
            if (n > 0) {
                /* Strip ANSI before accumulating */
                size_t sn = strip_ansi(raw, (size_t)n, stripped);
                if (sn > 0) {
                    /* Cap total buffer size */
                    if (sb->len < SERIAL_MAX_OUTPUT_BYTES) {
                        size_t take = sn;
                        if (sb->len + take > SERIAL_MAX_OUTPUT_BYTES)
                            take = SERIAL_MAX_OUTPUT_BYTES - sb->len;
                        strbuf_append_len(sb, stripped, take);
                    }
                }

                /* Check for each pattern in the full accumulated buffer */
                for (int p = 0; p < n_patterns; p++) {
                    if (patterns[p] && *patterns[p] &&
                        strstr(sb->data, patterns[p])) {
                        return p;
                    }
                }
            }
        }
    }
    return -1;  /* timeout */
}

/* Send a line (CR+LF terminated) */

static bool serial_send(int fd, const char *text) {
    size_t len = strlen(text);
    /* Write text + CRLF in one shot */
    char *buf = malloc(len + 3);
    if (!buf) return false;
    memcpy(buf, text, len);
    buf[len]   = '\r';
    buf[len+1] = '\n';
    buf[len+2] = '\0';

    size_t  total   = len + 2;
    ssize_t written = 0;
    while ((size_t)written < total) {
        ssize_t n = write(fd, buf + written, total - (size_t)written);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return false;
        }
        written += n;
    }
    tcdrain(fd);  /* wait for TX buffer to drain */
    free(buf);
    return true;
}

/* Login sequence */

/*
 * Perform login if credentials are configured.
 * Returns "ok", "skipped", "already_authenticated", or writes to
 * err_buf and returns NULL.
 */
static const char *serial_login(int fd, const serial_device_t *dev,
                                  char *err_buf, size_t err_sz) {
    if (dev->no_login || !dev->username[0]) return "skipped";

    double deadline = mono_now_ms() + (double)dev->login_timeout_sec * 1000.0;

    /* Send a bare CR to tickle any waiting login prompt */
    serial_send(fd, "");
    struct timespec ts = {0, 300000000L};  /* 300 ms */
    nanosleep(&ts, NULL);

    /* Build list of patterns to detect login prompt */
    const char *login_pats[6];
    int n_lpats = 0;
    if (dev->login_prompt[0]) login_pats[n_lpats++] = dev->login_prompt;
    login_pats[n_lpats++] = "login:";
    login_pats[n_lpats++] = "Login:";
    login_pats[n_lpats++] = "username:";
    login_pats[n_lpats++] = "Username:";
    login_pats[n_lpats++] = dev->prompt;  /* maybe already at shell */

    strbuf_t buf;
    strbuf_init(&buf, 1024);

    int matched = read_until(fd, login_pats, n_lpats, deadline, &buf);

    if (matched < 0) {
        snprintf(err_buf, err_sz,
                 "Login prompt not found within %ds. Last: %.200s",
                 dev->login_timeout_sec,
                 buf.len ? buf.data + (buf.len > 200 ? buf.len - 200 : 0) : "");
        strbuf_free(&buf);
        return NULL;
    }

    /* If we matched the shell prompt, already authenticated */
    if (matched == n_lpats - 1) {
        strbuf_free(&buf);
        return "already_authenticated";
    }

    /* Send username */
    strbuf_clear(&buf);
    serial_send(fd, dev->username);

    /* Wait for password prompt (if password is configured) */
    if (dev->password[0]) {
        const char *pwd_pats[] = { "password:", "Password:" };
        int pm = read_until(fd, pwd_pats, 2, deadline, &buf);
        if (pm < 0) {
            snprintf(err_buf, err_sz,
                     "Password prompt not found after sending username. "
                     "Last: %.200s",
                     buf.len ? buf.data + (buf.len > 200 ? buf.len - 200 : 0) : "");
            strbuf_free(&buf);
            return NULL;
        }
        strbuf_clear(&buf);
        serial_send(fd, dev->password);
    }

    /* Wait for shell prompt to confirm successful login */
    strbuf_clear(&buf);
    const char *shell_pat[] = { dev->prompt };
    int sp = read_until(fd, shell_pat, 1, deadline, &buf);

    if (sp < 0) {
        /* Check for common failure keywords */
        const char *fail[] = { "incorrect", "denied", "failed", "invalid", "error", NULL };
        for (int i = 0; fail[i]; i++) {
            if (buf.data && strstr(buf.data, fail[i])) {
                snprintf(err_buf, err_sz,
                         "Login failed (detected '%s'). Check credentials. "
                         "Last: %.200s", fail[i],
                         buf.len ? buf.data + (buf.len > 200 ? buf.len - 200 : 0) : "");
                strbuf_free(&buf);
                return NULL;
            }
        }
        snprintf(err_buf, err_sz,
                 "Shell prompt '%s' not seen after login within %ds. "
                 "Last: %.200s",
                 dev->prompt, dev->login_timeout_sec,
                 buf.len ? buf.data + (buf.len > 200 ? buf.len - 200 : 0) : "");
        strbuf_free(&buf);
        return NULL;
    }

    strbuf_free(&buf);
    return "ok";
}

/* Run a single command */

typedef struct {
    char   command[1024];
    char  *output;       /* heap-allocated; caller frees */
    char   status[16];   /* "ok", "timeout", "error"     */
} cmd_result_t;

static void run_one_command(int fd, const char *command,
                              const char *prompt, int timeout_sec,
                              cmd_result_t *out) {
    strncpy(out->command, command, sizeof(out->command) - 1);
    out->output = NULL;

    double deadline = mono_now_ms() + (double)timeout_sec * 1000.0;

    /* Flush any pending RX before sending */
    tcflush(fd, TCIFLUSH);

    if (!serial_send(fd, command)) {
        strcpy(out->status, "error");
        out->output = strdup("write failed");
        return;
    }

    strbuf_t buf;
    strbuf_init(&buf, 2048);

    const char *pats[] = { prompt };
    int matched = read_until(fd, pats, 1, deadline, &buf);

    /* Process output:
     * 1. Split into lines
     * 2. Remove the echoed command (first matching line)
     * 3. Remove bare prompt lines
     * 4. Re-join */
    strbuf_t cleaned;
    strbuf_init(&cleaned, buf.len + 1);

    bool echo_removed = false;
    const char *p = buf.data ? buf.data : "";
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);

        /* Extract line (without newline) */
        char *line = strndup(p, llen);

        /* Trim trailing CR */
        size_t ll = strlen(line);
        if (ll > 0 && line[ll-1] == '\r') line[ll-1] = '\0';

        const char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        /* Skip echo of the sent command */
        if (!echo_removed && strstr(trimmed, command)) {
            echo_removed = true;
            free(line);
            p += llen + (nl ? 1 : 0);
            continue;
        }

        /* Skip bare prompt lines */
        if (strcmp(trimmed, prompt) == 0) {
            free(line);
            p += llen + (nl ? 1 : 0);
            continue;
        }

        if (cleaned.len > 0) strbuf_append(&cleaned, "\n");
        strbuf_append(&cleaned, line);
        free(line);
        p += llen + (nl ? 1 : 0);
    }

    /* Trim leading/trailing whitespace from final output */
    char *result = cleaned.data ? cleaned.data : "";
    while (*result == '\n' || *result == '\r' || *result == ' ') result++;
    size_t rlen = strlen(result);
    while (rlen > 0 && (result[rlen-1] == '\n' ||
                        result[rlen-1] == '\r' ||
                        result[rlen-1] == ' ')) {
        result[--rlen] = '\0';
    }

    out->output = strdup(result);
    strcpy(out->status, matched >= 0 ? "ok" : "timeout");

    strbuf_free(&buf);
    strbuf_free(&cleaned);
}

/* JSON helpers */

static char *json_err(const char *msg) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "error");
    cJSON_AddStringToObject(o, "error",  msg);
    char *j = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return j;
}

/* Build tool result JSON */

static char *build_result(const char *status,
                            const char *device_name,
                            const serial_device_t *dev,
                            const char *login_status,
                            cmd_result_t *results, int n_results,
                            const char *error_msg,
                            double elapsed_ms) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", status);
    if (device_name && *device_name)
        cJSON_AddStringToObject(root, "device", device_name);
    cJSON_AddStringToObject(root, "port",   dev->port);
    cJSON_AddNumberToObject(root, "baud",   dev->baud);
    if (login_status)
        cJSON_AddStringToObject(root, "login_status", login_status);

    cJSON *cmds = cJSON_CreateArray();
    cJSON *res  = cJSON_CreateArray();
    for (int i = 0; i < n_results; i++) {
        cJSON_AddItemToArray(cmds, cJSON_CreateString(results[i].command));
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "command", results[i].command);
        cJSON_AddStringToObject(r, "output",  results[i].output ? results[i].output : "");
        cJSON_AddStringToObject(r, "status",  results[i].status);
        cJSON_AddItemToArray(res, r);
    }
    cJSON_AddItemToObject(root, "commands", cmds);
    cJSON_AddItemToObject(root, "results",  res);

    if (error_msg && *error_msg)
        cJSON_AddStringToObject(root, "error", error_msg);

    cJSON_AddNumberToObject(root, "elapsed_ms", (int)elapsed_ms);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* Tool execute callback */

static char *serial_execute(const cJSON *args, void *user_data) {
    (void)user_data;

    double t_start = mono_now_ms();
    char errbuf[1024] = "";

    /* Resolve device profile */

    serial_device_t dev;
    memset(&dev, 0, sizeof(dev));

    /* Defaults */
    dev.baud               = 115200;
    dev.timeout_sec        = 10;
    dev.login_timeout_sec  = 30;
    dev.inter_cmd_delay_ms = 300;
    strncpy(dev.prompt, "#", sizeof(dev.prompt) - 1);

    const char *device_name = cJSON_GetStringValue(cJSON_GetObjectItem(args, "device"));
    bool named_device = false;

    if (device_name && *device_name) {
        if (!device_lookup(device_name, &dev)) {
            snprintf(errbuf, sizeof(errbuf),
                     "Device '%s' not found in %s",
                     device_name, g_device_conf_path);
            return json_err(errbuf);
        }
        named_device = true;
        LOG_INFO("serial_exec: using device '%s' (%s @ %d)",
                 device_name, dev.port, dev.baud);
    }

    /* Explicit parameters override or supply device profile */
    const char *port_arg = cJSON_GetStringValue(cJSON_GetObjectItem(args, "port"));
    if (port_arg && *port_arg) strncpy(dev.port, port_arg, sizeof(dev.port)-1);

    cJSON *baud_v = cJSON_GetObjectItem(args, "baud");
    if (baud_v && cJSON_IsNumber(baud_v)) dev.baud = baud_v->valueint;

    const char *user_arg = cJSON_GetStringValue(cJSON_GetObjectItem(args, "username"));
    if (user_arg) strncpy(dev.username, user_arg, sizeof(dev.username)-1);

    const char *pass_arg = cJSON_GetStringValue(cJSON_GetObjectItem(args, "password"));
    if (pass_arg) strncpy(dev.password, pass_arg, sizeof(dev.password)-1);

    const char *lprompt_arg = cJSON_GetStringValue(cJSON_GetObjectItem(args, "login_prompt"));
    if (lprompt_arg) strncpy(dev.login_prompt, lprompt_arg, sizeof(dev.login_prompt)-1);

    const char *prompt_arg = cJSON_GetStringValue(cJSON_GetObjectItem(args, "prompt"));
    if (prompt_arg) strncpy(dev.prompt, prompt_arg, sizeof(dev.prompt)-1);

    cJSON *nologin_v = cJSON_GetObjectItem(args, "no_login");
    if (nologin_v && cJSON_IsBool(nologin_v))
        dev.no_login = cJSON_IsTrue(nologin_v);

    cJSON *to_v = cJSON_GetObjectItem(args, "timeout");
    if (to_v && cJSON_IsNumber(to_v)) dev.timeout_sec = to_v->valueint;

    cJSON *lto_v = cJSON_GetObjectItem(args, "login_timeout");
    if (lto_v && cJSON_IsNumber(lto_v)) dev.login_timeout_sec = lto_v->valueint;

    cJSON *icd_v = cJSON_GetObjectItem(args, "inter_cmd_delay_ms");
    if (icd_v && cJSON_IsNumber(icd_v)) dev.inter_cmd_delay_ms = icd_v->valueint;

    /* Validate required fields */

    if (!dev.port[0]) {
        return json_err("'port' is required (or specify a 'device' name)");
    }
    if (!dev.prompt[0]) {
        return json_err("'prompt' is required");
    }

    /* Collect commands */

    cJSON *cmds_json = cJSON_GetObjectItem(args, "commands");
    if (!cmds_json || !cJSON_IsArray(cmds_json) ||
        cJSON_GetArraySize(cmds_json) == 0) {
        return json_err("'commands' array is required and must be non-empty");
    }

    int n_cmds = cJSON_GetArraySize(cmds_json);
    if (n_cmds > SERIAL_MAX_COMMANDS)
        n_cmds = SERIAL_MAX_COMMANDS;

    const char *commands[SERIAL_MAX_COMMANDS];
    for (int i = 0; i < n_cmds; i++) {
        cJSON *c = cJSON_GetArrayItem(cmds_json, i);
        commands[i] = cJSON_IsString(c) ? c->valuestring : "";
    }

    /* Allocate result array */
    cmd_result_t *results = calloc((size_t)n_cmds, sizeof(cmd_result_t));
    if (!results) return json_err("out of memory");

    /* Open port */

    int fd = serial_open(dev.port, dev.baud, errbuf, sizeof(errbuf));
    if (fd < 0) {
        free(results);
        return json_err(errbuf);
    }

    LOG_INFO("serial_exec: opened %s @ %d baud", dev.port, dev.baud);

    /* Login */

    const char *login_status = serial_login(fd, &dev, errbuf, sizeof(errbuf));
    if (!login_status) {
        /* Login failed */
        close(fd);
        char *j = build_result("login_failed",
                                named_device ? device_name : NULL,
                                &dev, "failed",
                                results, 0,
                                errbuf,
                                mono_now_ms() - t_start);
        free(results);
        return j;
    }

    LOG_INFO("serial_exec: login_status=%s", login_status);

    /* Run commands */

    for (int i = 0; i < n_cmds; i++) {
        run_one_command(fd, commands[i], dev.prompt,
                        dev.timeout_sec, &results[i]);

        LOG_INFO("serial_exec: cmd='%s' status=%s",
                 commands[i], results[i].status);

        /* Inter-command delay */
        if (i < n_cmds - 1 && dev.inter_cmd_delay_ms > 0) {
            struct timespec ts = {
                dev.inter_cmd_delay_ms / 1000,
                (dev.inter_cmd_delay_ms % 1000) * 1000000L
            };
            nanosleep(&ts, NULL);
        }
    }

    close(fd);

    /* Determine overall status */

    const char *overall = "ok";
    strbuf_t err_detail;
    strbuf_init(&err_detail, 128);

    for (int i = 0; i < n_cmds; i++) {
        if (!strcmp(results[i].status, "error")) {
            overall = "error";
            if (err_detail.len) strbuf_append(&err_detail, "; ");
            strbuf_append(&err_detail, "error: ");
            strbuf_append(&err_detail, results[i].command);
        } else if (!strcmp(results[i].status, "timeout") &&
                   strcmp(overall, "error") != 0) {
            overall = "timeout";
            if (err_detail.len) strbuf_append(&err_detail, "; ");
            strbuf_append(&err_detail, "timeout: ");
            strbuf_append(&err_detail, results[i].command);
        }
    }

    char *j = build_result(overall,
                            named_device ? device_name : NULL,
                            &dev, login_status,
                            results, n_cmds,
                            err_detail.len ? err_detail.data : NULL,
                            mono_now_ms() - t_start);

    strbuf_free(&err_detail);
    for (int i = 0; i < n_cmds; i++) free(results[i].output);
    free(results);
    return j;
}

/* Registration */

void serial_tool_register(const char *device_conf_path) {
    if (device_conf_path && *device_conf_path) {
        strncpy(g_device_conf_path, device_conf_path,
                sizeof(g_device_conf_path) - 1);
    }

    static const tool_t tool = {
        .name        = "serial_exec",
        .description =
            "Execute commands on a serial-connected device (UART/TTY). "
            "Use 'device' to reference a named profile from device.conf "
            "(preferred — no need to specify port/baud/credentials manually). "
            "Pass 'commands' as an array of shell commands to run in sequence. "
            "Returns structured JSON with per-command output and status.",

        .parameters_schema =
            "{"
              "\"type\":\"object\","
              "\"properties\":{"
                "\"device\":{"
                  "\"type\":\"string\","
                  "\"description\":\"Named device from device.conf (e.g. 'm1700_0'). "
                                   "Resolves port, baud, credentials automatically.\""
                "},"
                "\"commands\":{"
                  "\"type\":\"array\","
                  "\"items\":{\"type\":\"string\"},"
                  "\"description\":\"Shell commands to run on the device in order.\""
                "},"
                "\"port\":{"
                  "\"type\":\"string\","
                  "\"description\":\"Serial device path (e.g. /dev/ttyUSB0). "
                                   "Required if 'device' is not specified.\""
                "},"
                "\"baud\":{"
                  "\"type\":\"integer\","
                  "\"description\":\"Baud rate (default 115200).\""
                "},"
                "\"no_login\":{"
                  "\"type\":\"boolean\","
                  "\"description\":\"Skip login — shell is already active on connect.\""
                "},"
                "\"username\":{"
                  "\"type\":\"string\","
                  "\"description\":\"Login username (omit to skip login).\""
                "},"
                "\"password\":{"
                  "\"type\":\"string\","
                  "\"description\":\"Login password.\""
                "},"
                "\"login_prompt\":{"
                  "\"type\":\"string\","
                  "\"description\":\"Custom string the device prints before username "
                                   "(e.g. 'm1700 login'). Auto-detected if omitted.\""
                "},"
                "\"prompt\":{"
                  "\"type\":\"string\","
                  "\"description\":\"Shell prompt string that marks end of command output "
                                   "(e.g. 'root@m1700:~#'). Default: '#'.\""
                "},"
                "\"timeout\":{"
                  "\"type\":\"integer\","
                  "\"description\":\"Per-command timeout in seconds (default 10).\""
                "},"
                "\"login_timeout\":{"
                  "\"type\":\"integer\","
                  "\"description\":\"Max seconds for the full login sequence (default 30).\""
                "},"
                "\"inter_cmd_delay_ms\":{"
                  "\"type\":\"integer\","
                  "\"description\":\"Milliseconds to wait between commands (default 300).\""
                "}"
              "},"
              "\"required\":[\"commands\"]"
            "}",

        .execute   = serial_execute,
        .user_data = NULL,
    };

    tool_register(&tool);
    LOG_DEBUG("serial_exec tool registered (device.conf: %s)",
              g_device_conf_path[0] ? g_device_conf_path : "(not set)");
}
