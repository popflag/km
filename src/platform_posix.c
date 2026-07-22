#include "platform.h"
#include "render.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define KM_MAX_PASTE_BYTES (1024u * 1024u)

struct KmPlatform {
    int interactive;
    int raw_enabled;
    int screen_entered;
    int installed_signals;
    int signal_read_fd;
    int signal_write_fd;
    struct termios original_termios;
    struct sigaction original_signals[5];
    unsigned columns;
    unsigned rows;
    uint32_t utf8_value;
    uint32_t utf8_min;
    unsigned utf8_needed;
    unsigned char pending_byte;
    int pending_byte_valid;
    int eof_pending;
    uint8_t *paste;
    size_t paste_len;
    size_t paste_cap;
    int discard_escape;
};

static volatile sig_atomic_t resize_pending;
static volatile sig_atomic_t terminate_pending;
static int interactive_active;
static volatile sig_atomic_t signal_notify_fd = -1;

static const int managed_signals[] = {
    SIGWINCH, SIGTERM, SIGHUP, SIGINT, SIGPIPE
};

static void set_error(char *error, size_t cap, const char *format, ...)
{
    va_list args;

    if (error == NULL || cap == 0) {
        return;
    }
    va_start(args, format);
    (void)vsnprintf(error, cap, format, args);
    va_end(args);
}

static void notify_main(unsigned char value)
{
    int saved_errno = errno;
    int descriptor = (int)signal_notify_fd;
    if (descriptor >= 0) {
        (void)write(descriptor, &value, 1);
    }
    errno = saved_errno;
}

static void handle_resize(int signal_number)
{
    (void)signal_number;
    resize_pending = 1;
    notify_main(1);
}

static void handle_terminate(int signal_number)
{
    terminate_pending = signal_number;
    notify_main(2);
}

static void update_size(KmPlatform *platform)
{
    struct winsize size;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 &&
        size.ws_col != 0 && size.ws_row != 0) {
        platform->columns = size.ws_col;
        platform->rows = size.ws_row;
    } else {
        platform->columns = 80;
        platform->rows = 24;
    }
}

static int write_all(const char *bytes, size_t length,
                     char *error, size_t error_cap)
{
    size_t written = 0;

    while (written < length) {
        ssize_t count = write(STDOUT_FILENO, bytes + written, length - written);
        if (count > 0) {
            written += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            int code = count < 0 ? errno : EIO;
            set_error(error, error_cap, "write terminal: %s", strerror(code));
            return -1;
        }
    }
    return 0;
}

static int install_signals(KmPlatform *platform, char *error, size_t error_cap)
{
    size_t i;

    for (i = 0; i < sizeof(managed_signals) / sizeof(managed_signals[0]); ++i) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        sigemptyset(&action.sa_mask);
        if (managed_signals[i] == SIGWINCH) {
            action.sa_handler = handle_resize;
        } else if (managed_signals[i] == SIGPIPE) {
            action.sa_handler = SIG_IGN;
        } else {
            action.sa_handler = handle_terminate;
        }
        if (sigaction(managed_signals[i], &action,
                      &platform->original_signals[i]) != 0) {
            set_error(error, error_cap, "install signal handler: %s",
                      strerror(errno));
            return -1;
        }
        platform->installed_signals = (int)i + 1;
    }
    return 0;
}

static int open_signal_pipe(KmPlatform *platform, char *error, size_t error_cap)
{
    int descriptors[2];
    int flags;

    if (pipe(descriptors) != 0) {
        set_error(error, error_cap, "create signal pipe: %s", strerror(errno));
        return -1;
    }
    platform->signal_read_fd = descriptors[0];
    platform->signal_write_fd = descriptors[1];
    flags = fcntl(platform->signal_read_fd, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(platform->signal_read_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        set_error(error, error_cap, "configure signal pipe: %s", strerror(errno));
        return -1;
    }
    flags = fcntl(platform->signal_write_fd, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(platform->signal_write_fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
        fcntl(platform->signal_read_fd, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(platform->signal_write_fd, F_SETFD, FD_CLOEXEC) != 0) {
        set_error(error, error_cap, "configure signal pipe: %s", strerror(errno));
        return -1;
    }
    signal_notify_fd = platform->signal_write_fd;
    return 0;
}

static void restore_signals(KmPlatform *platform)
{
    while (platform->installed_signals > 0) {
        int i = --platform->installed_signals;
        (void)sigaction(managed_signals[i], &platform->original_signals[i], NULL);
    }
}

int km_platform_open(KmPlatform **out, char *error, size_t error_cap)
{
    static const char enter_screen[] = "\x1b[?1049h\x1b[?2004h\x1b[?25l";
    KmPlatform *platform;
    struct termios raw;

    if (out == NULL) {
        set_error(error, error_cap, "invalid platform output pointer");
        return -1;
    }
    *out = NULL;
    platform = (KmPlatform *)calloc(1, sizeof(*platform));
    if (platform == NULL) {
        set_error(error, error_cap, "allocate platform: out of memory");
        return -1;
    }
    platform->signal_read_fd = -1;
    platform->signal_write_fd = -1;
    platform->interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    update_size(platform);
    if (!platform->interactive) {
        *out = platform;
        return 0;
    }
    if (interactive_active) {
        set_error(error, error_cap, "an interactive terminal is already active");
        free(platform);
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &platform->original_termios) != 0) {
        set_error(error, error_cap, "read terminal mode: %s", strerror(errno));
        free(platform);
        return -1;
    }

    interactive_active = 1;
    resize_pending = 0;
    terminate_pending = 0;
    if (open_signal_pipe(platform, error, error_cap) != 0 ||
        install_signals(platform, error, error_cap) != 0) {
        km_platform_close(platform);
        return -1;
    }

    raw = platform->original_termios;
    raw.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ICRNL | INLCR |
                               IGNCR | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag &= (tcflag_t)~CSIZE;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        set_error(error, error_cap, "enter raw terminal mode: %s", strerror(errno));
        km_platform_close(platform);
        return -1;
    }
    platform->raw_enabled = 1;
    platform->screen_entered = 1;
    if (write_all(enter_screen, sizeof(enter_screen) - 1,
                  error, error_cap) != 0) {
        km_platform_close(platform);
        return -1;
    }
    *out = platform;
    return 0;
}

void km_platform_close(KmPlatform *platform)
{
    static const char leave_screen[] = "\x1b[?25h\x1b[?2004l\x1b[?1049l";
    int terminating_signal;

    if (platform == NULL) {
        return;
    }
    terminating_signal = (int)terminate_pending;
    if (platform->screen_entered) {
        (void)write_all(leave_screen, sizeof(leave_screen) - 1, NULL, 0);
    }
    if (platform->raw_enabled) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &platform->original_termios);
    }
    restore_signals(platform);
    signal_notify_fd = -1;
    if (platform->signal_read_fd >= 0) close(platform->signal_read_fd);
    if (platform->signal_write_fd >= 0) close(platform->signal_write_fd);
    if (platform->interactive) {
        interactive_active = 0;
    }
    free(platform->paste);
    free(platform);
    if (terminating_signal != 0) {
        (void)raise(terminating_signal);
    }
}

int km_platform_is_interactive(const KmPlatform *platform)
{
    return platform != NULL && platform->interactive;
}

void km_platform_size(const KmPlatform *platform, unsigned *columns,
                      unsigned *rows)
{
    if (columns != NULL) *columns = platform == NULL ? 0 : platform->columns;
    if (rows != NULL) *rows = platform == NULL ? 0 : platform->rows;
}

int km_platform_draw_grid(KmPlatform *platform, const KmCellGrid *grid,
                          size_t cursor_row, size_t cursor_column,
                          char *error, size_t error_cap)
{
    KmError render_error;
    char *bytes = NULL;
    size_t length = 0;
    int result;

    if (platform == NULL || !platform->interactive) {
        set_error(error, error_cap, "draw grid: interactive terminal required");
        return -1;
    }
    if (km_cell_grid_encode_frame_vt(grid, cursor_row, cursor_column,
                                     &bytes, &length, &render_error) != KM_OK) {
        set_error(error, error_cap, "%s",
                  render_error.operation == NULL ? "encode terminal frame"
                                                 : render_error.operation);
        return -1;
    }
    result = write_all(bytes, length, error, error_cap);
    free(bytes);
    return result;
}

int km_platform_draw_probe(KmPlatform *platform, char *error, size_t error_cap)
{
    KmCellGrid *grid = NULL;
    KmError render_error;
    char *bytes = NULL;
    size_t length = 0;
    int result;

    if (platform == NULL) {
        set_error(error, error_cap, "draw probe: platform is null");
        return -1;
    }
    if (platform->interactive) update_size(platform);
    if (km_render_probe_grid_create(platform->columns, platform->rows,
                                    platform->interactive, &grid,
                                    &render_error) != KM_OK ||
        km_cell_grid_encode_vt(grid, platform->interactive, &bytes, &length,
                               &render_error) != KM_OK) {
        set_error(error, error_cap, "%s",
                  render_error.operation == NULL ? "render terminal probe"
                                                 : render_error.operation);
        km_cell_grid_destroy(grid);
        return -1;
    }
    result = write_all(bytes, length, error, error_cap);
    free(bytes);
    km_cell_grid_destroy(grid);
    return result;
}

static void finish_key(KmEvent *event, uint32_t codepoint)
{
    memset(event, 0, sizeof(*event));
    event->repeat = 1;
    if (codepoint == 0) {
        event->kind = KM_EVENT_KEY;
        event->codepoint = ' ';
        event->modifiers = KM_MOD_CTRL;
    } else if (codepoint == 8 || codepoint == 0x7f) {
        event->kind = KM_EVENT_KEY;
        event->codepoint = 0x7f;
    } else if (codepoint == '\t') {
        event->kind = KM_EVENT_KEY;
        event->codepoint = KM_KEY_TAB;
    } else if (codepoint == '\r' || codepoint == '\n') {
        event->kind = KM_EVENT_TEXT;
        event->codepoint = '\n';
    } else if (codepoint >= 1 && codepoint <= 26) {
        event->kind = KM_EVENT_KEY;
        event->codepoint = (uint32_t)('a' + codepoint - 1);
        event->modifiers = KM_MOD_CTRL;
    } else if (codepoint == 0x1f) {
        event->kind = KM_EVENT_KEY;
        event->codepoint = '/';
        event->modifiers = KM_MOD_CTRL;
    } else {
        event->kind = codepoint >= 0x20 ? KM_EVENT_TEXT : KM_EVENT_KEY;
        event->codepoint = codepoint;
    }
}

static void finish_named_key(KmEvent *event, uint32_t key)
{
    memset(event, 0, sizeof(*event));
    event->kind = KM_EVENT_KEY;
    event->codepoint = key;
    event->repeat = 1;
}

static int decode_key(KmPlatform *platform, unsigned char byte, KmEvent *event);

/* 1 byte, 0 timeout/signal, 2 EOF, -1 error. */
static int read_raw_byte(KmPlatform *platform, unsigned char *byte,
                         int timeout_ms, char *error, size_t error_cap)
{
    for (;;) {
        ssize_t count;

        if (platform->interactive) {
            struct pollfd descriptors[2];
            int ready;

            if (terminate_pending) return 0;
            descriptors[0].fd = STDIN_FILENO;
            descriptors[0].events = POLLIN;
            descriptors[0].revents = 0;
            descriptors[1].fd = platform->signal_read_fd;
            descriptors[1].events = POLLIN;
            descriptors[1].revents = 0;
            ready = poll(descriptors, 2, timeout_ms);
            if (ready == 0) return 0;
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) {
                set_error(error, error_cap, "poll terminal: %s", strerror(errno));
                return -1;
            }
            if ((descriptors[1].revents & POLLIN) != 0) {
                unsigned char discard[64];
                while (read(platform->signal_read_fd, discard,
                            sizeof(discard)) > 0) {
                }
                return 0;
            }
        }
        count = read(STDIN_FILENO, byte, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            set_error(error, error_cap, "read terminal: %s", strerror(errno));
            return -1;
        }
        return count == 0 ? 2 : 1;
    }
}

static int read_bracketed_paste(KmPlatform *platform, KmEvent *event,
                                char *error, size_t error_cap)
{
    static const uint8_t end_marker[] = {0x1b, '[', '2', '0', '1', '~'};
    size_t matched = 0;
    size_t total = 0;
    bool over_limit = false;

    platform->paste_len = 0;
    for (;;) {
        unsigned char byte;
        int read_status = read_raw_byte(platform, &byte, -1, error, error_cap);
        uint8_t *paste;
        size_t capacity;

        if (read_status < 0) return -1;
        if (read_status == 0 && resize_pending) continue;
        if (read_status != 1) {
            set_error(error, error_cap, "read paste: incomplete delimiter");
            return -1;
        }
        if (platform->paste_len < KM_MAX_PASTE_BYTES + sizeof(end_marker) &&
            platform->paste_len == platform->paste_cap) {
            capacity = platform->paste_cap == 0 ? 256 : platform->paste_cap * 2;
            if (capacity > KM_MAX_PASTE_BYTES + sizeof(end_marker)) {
                capacity = KM_MAX_PASTE_BYTES + sizeof(end_marker);
            }
            paste = (uint8_t *)realloc(platform->paste, capacity);
            if (paste == NULL) {
                set_error(error, error_cap, "read paste: out of memory");
                return -1;
            }
            platform->paste = paste;
            platform->paste_cap = capacity;
        }
        if (platform->paste_len < KM_MAX_PASTE_BYTES + sizeof(end_marker)) {
            platform->paste[platform->paste_len++] = byte;
        }
        if (total < KM_MAX_PASTE_BYTES + sizeof(end_marker)) {
            ++total;
        } else {
            over_limit = true;
        }
        matched = byte == end_marker[matched]
                      ? matched + 1
                      : (byte == end_marker[0] ? 1u : 0u);
        if (matched == sizeof(end_marker)) {
            size_t content_len = total - sizeof(end_marker);
            if (over_limit || content_len > KM_MAX_PASTE_BYTES) {
                platform->paste_len = 0;
                set_error(error, error_cap, "read paste: input exceeds 1 MiB");
                return -1;
            }
            platform->paste_len = content_len;
            memset(event, 0, sizeof(*event));
            event->kind = KM_EVENT_PASTE;
            event->repeat = 1;
            event->text = platform->paste;
            event->text_len = platform->paste_len;
            return 0;
        }
    }
}

static int decode_escape(KmPlatform *platform, KmEvent *event,
                         char *error, size_t error_cap)
{
    unsigned char byte;
    int read_status = read_raw_byte(platform, &byte,
                                    platform->interactive ? 50 : -1,
                                    error, error_cap);

    if (read_status < 0) return -1;
    if (read_status != 1) {
        finish_named_key(event, KM_KEY_ESCAPE);
        if (read_status == 2) platform->eof_pending = 1;
        return 0;
    }
    if (byte == '[' || byte == 'O') {
        unsigned char sequence[16];
        size_t length = 0;
        bool complete = false;

        while (length < sizeof(sequence)) {
            read_status = read_raw_byte(platform, &sequence[length],
                                        platform->interactive ? 50 : -1,
                                        error, error_cap);
            if (read_status < 0) return -1;
            if (read_status != 1) {
                if (read_status == 2) platform->eof_pending = 1;
                break;
            }
            if (sequence[length] >= 0x40 && sequence[length] <= 0x7e) {
                ++length;
                complete = true;
                break;
            }
            ++length;
        }
        if (!complete && length == sizeof(sequence)) {
            platform->discard_escape = 1;
        }
        if (complete && length == 1) {
            switch (sequence[0]) {
            case 'A': finish_named_key(event, KM_KEY_UP); return 0;
            case 'B': finish_named_key(event, KM_KEY_DOWN); return 0;
            case 'C': finish_named_key(event, KM_KEY_RIGHT); return 0;
            case 'D': finish_named_key(event, KM_KEY_LEFT); return 0;
            case 'H': finish_named_key(event, KM_KEY_HOME); return 0;
            case 'F': finish_named_key(event, KM_KEY_END); return 0;
            default: break;
            }
        }
        if (complete && byte == '[' && length == 2 &&
            sequence[0] == '3' && sequence[1] == '~') {
            finish_named_key(event, KM_KEY_DELETE);
            return 0;
        }
        if (complete && byte == '[' && length == 2 &&
            (sequence[0] == '1' || sequence[0] == '7') &&
            sequence[1] == '~') {
            finish_named_key(event, KM_KEY_HOME);
            return 0;
        }
        if (complete && byte == '[' && length == 2 &&
            (sequence[0] == '4' || sequence[0] == '8') &&
            sequence[1] == '~') {
            finish_named_key(event, KM_KEY_END);
            return 0;
        }
        if (complete && byte == '[' && length == 4 &&
            memcmp(sequence, "200~", 4) == 0) {
            return read_bracketed_paste(platform, event, error, error_cap);
        }
        finish_named_key(event, KM_KEY_ESCAPE);
        return 0;
    }
    for (;;) {
        if (decode_key(platform, byte, event)) break;
        read_status = read_raw_byte(platform, &byte,
                                    platform->interactive ? 50 : -1,
                                    error, error_cap);
        if (read_status < 0) return -1;
        if (read_status != 1) {
            platform->utf8_needed = 0;
            finish_key(event, 0xfffd);
            if (read_status == 2) platform->eof_pending = 1;
            break;
        }
    }
    if (event->kind == KM_EVENT_TEXT) event->kind = KM_EVENT_KEY;
    event->modifiers |= KM_MOD_ALT;
    return 0;
}

static int discard_escape_sequence(KmPlatform *platform, KmEvent *event,
                                   char *error, size_t error_cap)
{
    for (size_t count = 0; count < 4096; ++count) {
        unsigned char byte;
        int read_status = read_raw_byte(platform, &byte,
                                        platform->interactive ? 50 : -1,
                                        error, error_cap);
        if (read_status < 0) return -1;
        if (read_status != 1) {
            if (read_status == 2) {
                platform->eof_pending = 1;
                platform->discard_escape = 0;
            }
            finish_named_key(event, KM_KEY_ESCAPE);
            return 0;
        }
        if (byte >= 0x40 && byte <= 0x7e) {
            platform->discard_escape = 0;
            finish_named_key(event, KM_KEY_ESCAPE);
            return 0;
        }
    }
    finish_named_key(event, KM_KEY_ESCAPE);
    return 0;
}

static int decode_key(KmPlatform *platform, unsigned char byte, KmEvent *event)
{
    uint32_t codepoint;

    if (platform->utf8_needed == 0) {
        if (byte < 0x80) {
            codepoint = byte;
        } else if (byte >= 0xC2 && byte <= 0xDF) {
            platform->utf8_value = byte & 0x1Fu;
            platform->utf8_min = 0x80;
            platform->utf8_needed = 1;
            return 0;
        } else if (byte >= 0xE0 && byte <= 0xEF) {
            platform->utf8_value = byte & 0x0Fu;
            platform->utf8_min = 0x800;
            platform->utf8_needed = 2;
            return 0;
        } else if (byte >= 0xF0 && byte <= 0xF4) {
            platform->utf8_value = byte & 0x07u;
            platform->utf8_min = 0x10000;
            platform->utf8_needed = 3;
            return 0;
        } else {
            codepoint = 0xFFFD;
        }
    } else if ((byte & 0xC0u) == 0x80u) {
        platform->utf8_value = (platform->utf8_value << 6) | (byte & 0x3Fu);
        if (--platform->utf8_needed != 0) {
            return 0;
        }
        codepoint = platform->utf8_value;
        if (codepoint < platform->utf8_min || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            codepoint = 0xFFFD;
        }
    } else {
        platform->utf8_needed = 0;
        platform->pending_byte = byte;
        platform->pending_byte_valid = 1;
        codepoint = 0xFFFD;
    }

    finish_key(event, codepoint);
    return 1;
}

int km_platform_read_event(KmPlatform *platform, KmEvent *event,
                           char *error, size_t error_cap)
{
    if (platform == NULL || event == NULL) {
        set_error(error, error_cap, "read event: invalid argument");
        return -1;
    }
    for (;;) {
        unsigned char byte;
        ssize_t count;

        if (terminate_pending) {
            memset(event, 0, sizeof(*event));
            event->kind = KM_EVENT_EOF;
            return 0;
        }
        if (platform->interactive && resize_pending) {
            resize_pending = 0;
            update_size(platform);
            memset(event, 0, sizeof(*event));
            event->kind = KM_EVENT_RESIZE;
            event->columns = platform->columns;
            event->rows = platform->rows;
            return 0;
        }
        if (platform->pending_byte_valid) {
            byte = platform->pending_byte;
            platform->pending_byte_valid = 0;
            if (decode_key(platform, byte, event)) return 0;
            continue;
        }
        if (platform->eof_pending) {
            platform->eof_pending = 0;
            memset(event, 0, sizeof(*event));
            event->kind = KM_EVENT_EOF;
            return 0;
        }
        if (platform->discard_escape) {
            return discard_escape_sequence(platform, event, error, error_cap);
        }
        {
            int read_status = read_raw_byte(platform, &byte, -1,
                                            error, error_cap);
            if (read_status < 0) return -1;
            if (read_status == 0) continue;
            count = read_status == 2 ? 0 : 1;
        }
        if (count == 0) {
            if (platform->utf8_needed != 0) {
                platform->utf8_needed = 0;
                platform->eof_pending = 1;
                finish_key(event, 0xFFFD);
                return 0;
            }
            memset(event, 0, sizeof(*event));
            event->kind = KM_EVENT_EOF;
            return 0;
        }
        if (platform->utf8_needed == 0 && byte == 0x1b) {
            return decode_escape(platform, event, error, error_cap);
        }
        if (decode_key(platform, byte, event)) {
            return 0;
        }
    }
}
