#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "platform.h"
#include "render.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct KmPlatform {
    HANDLE input;
    HANDLE output;
    DWORD original_input_mode;
    DWORD original_output_mode;
    int input_console;
    int output_console;
    int input_mode_changed;
    int output_mode_changed;
    int screen_entered;
    int handler_installed;
    int interactive;
    unsigned columns;
    unsigned rows;
    WCHAR high_surrogate;
    uint32_t utf8_value;
    uint32_t utf8_min;
    unsigned utf8_needed;
    unsigned char pending_byte;
    int pending_byte_valid;
    int eof_pending;
    int discard_escape;
};

static volatile LONG interactive_active;
static HANDLE active_input;
static HANDLE active_output;
static DWORD active_input_mode;
static DWORD active_output_mode;
static volatile LONG active_input_changed;
static volatile LONG active_output_changed;
static volatile LONG active_screen_entered;
static volatile LONG active_handler_enabled;
static volatile LONG active_handler_count;
static SRWLOCK active_console_lock = SRWLOCK_INIT;

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

static void set_windows_error(char *error, size_t cap, const char *operation)
{
    set_error(error, cap, "%s: Windows error %lu", operation,
              (unsigned long)GetLastError());
}

static BOOL WINAPI handle_console_control(DWORD control_type)
{
    static const WCHAR leave_screen[] = L"\x1b[?25h\x1b[?1049l";
    DWORD written;

    if (control_type != CTRL_C_EVENT && control_type != CTRL_BREAK_EVENT &&
        control_type != CTRL_CLOSE_EVENT && control_type != CTRL_LOGOFF_EVENT &&
        control_type != CTRL_SHUTDOWN_EVENT) {
        return FALSE;
    }
    (void)InterlockedIncrement(&active_handler_count);
    if (InterlockedCompareExchange(&active_handler_enabled, 0, 0) == 0) {
        (void)InterlockedDecrement(&active_handler_count);
        return FALSE;
    }
    AcquireSRWLockExclusive(&active_console_lock);
    if (InterlockedCompareExchange(&active_handler_enabled, 0, 0) == 0) {
        ReleaseSRWLockExclusive(&active_console_lock);
        (void)InterlockedDecrement(&active_handler_count);
        return FALSE;
    }
    if (InterlockedCompareExchange(&active_screen_entered, 0, 0) != 0) {
        (void)WriteConsoleW(active_output, leave_screen,
                            (DWORD)(sizeof(leave_screen) / sizeof(leave_screen[0]) - 1),
                            &written, NULL);
    }
    if (InterlockedCompareExchange(&active_input_changed, 0, 0) != 0) {
        (void)SetConsoleMode(active_input, active_input_mode);
    }
    if (InterlockedCompareExchange(&active_output_changed, 0, 0) != 0) {
        (void)SetConsoleMode(active_output, active_output_mode);
    }
    ReleaseSRWLockExclusive(&active_console_lock);
    (void)InterlockedDecrement(&active_handler_count);
    return FALSE;
}

static void update_size(KmPlatform *platform)
{
    CONSOLE_SCREEN_BUFFER_INFO info;

    if (platform->output_console &&
        GetConsoleScreenBufferInfo(platform->output, &info)) {
        platform->columns = (unsigned)(info.srWindow.Right - info.srWindow.Left + 1);
        platform->rows = (unsigned)(info.srWindow.Bottom - info.srWindow.Top + 1);
    } else {
        platform->columns = 80;
        platform->rows = 24;
    }
}

static int write_utf8(KmPlatform *platform, const char *bytes, size_t length,
                      char *error, size_t error_cap)
{
    if (!platform->output_console) {
        size_t total = 0;
        while (total < length) {
            DWORD count = (DWORD)((length - total) > MAXDWORD
                                      ? MAXDWORD
                                      : (length - total));
            DWORD written = 0;
            if (!WriteFile(platform->output, bytes + total, count, &written, NULL)) {
                set_windows_error(error, error_cap, "write output");
                return -1;
            }
            if (written == 0) {
                set_error(error, error_cap, "write output: no progress");
                return -1;
            }
            total += written;
        }
        return 0;
    }

    if (length > INT_MAX) {
        set_error(error, error_cap, "write console: output is too large");
        return -1;
    } else {
        int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                               bytes, (int)length, NULL, 0);
        WCHAR *wide;
        DWORD total = 0;
        if (wide_length <= 0) {
            set_windows_error(error, error_cap, "decode UTF-8 output");
            return -1;
        }
        if ((size_t)wide_length > SIZE_MAX / sizeof(*wide)) {
            set_error(error, error_cap, "write console: output is too large");
            return -1;
        }
        wide = (WCHAR *)malloc((size_t)wide_length * sizeof(*wide));
        if (wide == NULL) {
            set_error(error, error_cap, "write console: out of memory");
            return -1;
        }
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)length,
                                wide, wide_length) != wide_length) {
            set_windows_error(error, error_cap, "decode UTF-8 output");
            free(wide);
            return -1;
        }
        while (total < (DWORD)wide_length) {
            DWORD written = 0;
            if (!WriteConsoleW(platform->output, wide + total,
                               (DWORD)wide_length - total, &written, NULL)) {
                set_windows_error(error, error_cap, "write console");
                free(wide);
                return -1;
            }
            if (written == 0) {
                set_error(error, error_cap, "write console: no progress");
                free(wide);
                return -1;
            }
            total += written;
        }
        free(wide);
    }
    return 0;
}

int km_platform_open(KmPlatform **out, char *error, size_t error_cap)
{
    static const char enter_screen[] = "\x1b[?1049h\x1b[?25l";
    KmPlatform *platform;
    DWORD mode;

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
    platform->input = GetStdHandle(STD_INPUT_HANDLE);
    platform->output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (platform->input == NULL || platform->input == INVALID_HANDLE_VALUE ||
        platform->output == NULL || platform->output == INVALID_HANDLE_VALUE) {
        set_windows_error(error, error_cap, "get standard handles");
        free(platform);
        return -1;
    }
    platform->input_console = GetConsoleMode(platform->input,
                                              &platform->original_input_mode) != 0;
    platform->output_console = GetConsoleMode(platform->output,
                                               &platform->original_output_mode) != 0;
    platform->interactive = platform->input_console && platform->output_console;
    update_size(platform);
    if (!platform->interactive) {
        *out = platform;
        return 0;
    }
    if (InterlockedCompareExchange(&interactive_active, 1, 0) != 0) {
        set_error(error, error_cap, "an interactive console is already active");
        free(platform);
        return -1;
    }

    active_input = platform->input;
    active_output = platform->output;
    active_input_mode = platform->original_input_mode;
    active_output_mode = platform->original_output_mode;
    AcquireSRWLockExclusive(&active_console_lock);
    (void)InterlockedExchange(&active_handler_enabled, 1);
    if (!SetConsoleCtrlHandler(handle_console_control, TRUE)) {
        (void)InterlockedExchange(&active_handler_enabled, 0);
        ReleaseSRWLockExclusive(&active_console_lock);
        set_windows_error(error, error_cap, "install console control handler");
        km_platform_close(platform);
        return -1;
    }
    platform->handler_installed = 1;
    platform->output_mode_changed = 1;
    (void)InterlockedExchange(&active_output_changed, 1);
    mode = platform->original_output_mode |
           ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(platform->output, mode)) {
        set_windows_error(error, error_cap, "enable VT output");
        ReleaseSRWLockExclusive(&active_console_lock);
        km_platform_close(platform);
        return -1;
    }
    platform->input_mode_changed = 1;
    (void)InterlockedExchange(&active_input_changed, 1);
    mode = platform->original_input_mode;
    mode &= ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    mode |= ENABLE_WINDOW_INPUT;
    if (!SetConsoleMode(platform->input, mode)) {
        set_windows_error(error, error_cap, "enter console input mode");
        ReleaseSRWLockExclusive(&active_console_lock);
        km_platform_close(platform);
        return -1;
    }

    platform->screen_entered = 1;
    (void)InterlockedExchange(&active_screen_entered, 1);
    if (write_utf8(platform, enter_screen, sizeof(enter_screen) - 1,
                   error, error_cap) != 0) {
        ReleaseSRWLockExclusive(&active_console_lock);
        km_platform_close(platform);
        return -1;
    }
    ReleaseSRWLockExclusive(&active_console_lock);
    *out = platform;
    return 0;
}

void km_platform_close(KmPlatform *platform)
{
    static const char leave_screen[] = "\x1b[?25h\x1b[?1049l";

    if (platform == NULL) {
        return;
    }
    if (platform->interactive) {
        AcquireSRWLockExclusive(&active_console_lock);
    }
    if (platform->screen_entered) {
        (void)write_utf8(platform, leave_screen, sizeof(leave_screen) - 1, NULL, 0);
    }
    (void)InterlockedExchange(&active_screen_entered, 0);
    if (platform->input_mode_changed) {
        (void)SetConsoleMode(platform->input, platform->original_input_mode);
    }
    if (platform->output_mode_changed) {
        (void)SetConsoleMode(platform->output, platform->original_output_mode);
    }
    (void)InterlockedExchange(&active_input_changed, 0);
    (void)InterlockedExchange(&active_output_changed, 0);
    if (platform->interactive) {
        ReleaseSRWLockExclusive(&active_console_lock);
    }
    if (platform->handler_installed) {
        (void)InterlockedExchange(&active_handler_enabled, 0);
        (void)SetConsoleCtrlHandler(handle_console_control, FALSE);
        while (InterlockedCompareExchange(&active_handler_count, 0, 0) != 0) {
            Sleep(0);
        }
        platform->handler_installed = 0;
    }
    if (platform->interactive) {
        (void)InterlockedExchange(&interactive_active, 0);
    }
    free(platform);
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
    result = write_utf8(platform, bytes, length, error, error_cap);
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
    result = write_utf8(platform, bytes, length, error, error_cap);
    free(bytes);
    km_cell_grid_destroy(grid);
    return result;
}

static void finish_key(KmEvent *event, uint32_t codepoint,
                       uint32_t modifiers, uint32_t repeat)
{
    memset(event, 0, sizeof(*event));
    event->repeat = repeat;
    if (codepoint == 0) {
        event->kind = KM_EVENT_KEY;
        event->codepoint = ' ';
        event->modifiers = modifiers | KM_MOD_CTRL;
    } else if (codepoint == 8 || codepoint == 0x7f) {
        event->kind = KM_EVENT_KEY;
        event->codepoint = 0x7f;
        event->modifiers = modifiers & (KM_MOD_CTRL | KM_MOD_ALT);
    } else if (codepoint == '\t') {
        event->kind = KM_EVENT_KEY;
        event->codepoint = KM_KEY_TAB;
        event->modifiers = modifiers;
    } else if (codepoint == '\r' || codepoint == '\n') {
        event->kind = KM_EVENT_TEXT;
        event->codepoint = '\n';
    } else if (codepoint >= 1 && codepoint <= 26) {
        event->kind = KM_EVENT_KEY;
        event->codepoint = (uint32_t)('a' + codepoint - 1);
        event->modifiers = modifiers | KM_MOD_CTRL;
    } else if (codepoint == 0x1f) {
        event->kind = KM_EVENT_KEY;
        event->codepoint = '/';
        event->modifiers = modifiers | KM_MOD_CTRL;
    } else {
        event->kind = codepoint >= 0x20 &&
                              (modifiers & (KM_MOD_CTRL | KM_MOD_ALT)) == 0
                          ? KM_EVENT_TEXT
                          : KM_EVENT_KEY;
        event->codepoint = codepoint;
        event->modifiers = event->kind == KM_EVENT_TEXT ? 0 : modifiers;
    }
}

static int decode_redirected_key(KmPlatform *platform, unsigned char byte,
                                 KmEvent *event)
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
    finish_key(event, codepoint, 0, 1);
    return 1;
}

static void finish_named_key(KmEvent *event, uint32_t key)
{
    memset(event, 0, sizeof(*event));
    event->kind = KM_EVENT_KEY;
    event->codepoint = key;
    event->repeat = 1;
}

static int read_redirected_byte(KmPlatform *platform, unsigned char *byte,
                                char *error, size_t error_cap)
{
    DWORD count = 0;

    if (!ReadFile(platform->input, byte, 1, &count, NULL)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) return 0;
        set_windows_error(error, error_cap, "read input");
        return -1;
    }
    return count == 0 ? 0 : 1;
}

static int decode_redirected_escape(KmPlatform *platform, KmEvent *event,
                                    char *error, size_t error_cap)
{
    unsigned char byte;
    int read_status = read_redirected_byte(platform, &byte, error, error_cap);

    if (read_status < 0) return -1;
    if (read_status == 0) {
        finish_named_key(event, KM_KEY_ESCAPE);
        platform->eof_pending = 1;
        return 0;
    }
    if (byte == '[' || byte == 'O') {
        unsigned char sequence[16];
        size_t length = 0;
        bool complete = false;

        while (length < sizeof(sequence)) {
            read_status = read_redirected_byte(platform, &sequence[length],
                                               error, error_cap);
            if (read_status < 0) return -1;
            if (read_status == 0) {
                platform->eof_pending = 1;
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
        if (complete && byte == '[' && length == 2 && sequence[0] == '3' &&
            sequence[1] == '~') {
            finish_named_key(event, KM_KEY_DELETE);
            return 0;
        }
        finish_named_key(event, KM_KEY_ESCAPE);
        return 0;
    }
    for (;;) {
        if (decode_redirected_key(platform, byte, event)) break;
        read_status = read_redirected_byte(platform, &byte, error, error_cap);
        if (read_status < 0) return -1;
        if (read_status == 0) {
            platform->utf8_needed = 0;
            finish_key(event, 0xfffd, 0, 1);
            platform->eof_pending = 1;
            break;
        }
    }
    if (event->kind == KM_EVENT_TEXT) event->kind = KM_EVENT_KEY;
    event->modifiers |= KM_MOD_ALT;
    return 0;
}

static int discard_redirected_escape(KmPlatform *platform, KmEvent *event,
                                     char *error, size_t error_cap)
{
    for (size_t count = 0; count < 4096; ++count) {
        unsigned char byte;
        int read_status = read_redirected_byte(platform, &byte,
                                               error, error_cap);
        if (read_status < 0) return -1;
        if (read_status == 0) {
            platform->discard_escape = 0;
            platform->eof_pending = 1;
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

static uint32_t console_modifiers(DWORD state)
{
    uint32_t modifiers = 0;
    if (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        modifiers |= KM_MOD_CTRL;
    }
    if (state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) {
        modifiers |= KM_MOD_ALT;
    }
    if (state & SHIFT_PRESSED) {
        modifiers |= KM_MOD_SHIFT;
    }
    return modifiers;
}

static uint32_t named_console_key(WORD virtual_key)
{
    switch (virtual_key) {
    case VK_ESCAPE: return KM_KEY_ESCAPE;
    case VK_LEFT: return KM_KEY_LEFT;
    case VK_RIGHT: return KM_KEY_RIGHT;
    case VK_UP: return KM_KEY_UP;
    case VK_DOWN: return KM_KEY_DOWN;
    case VK_HOME: return KM_KEY_HOME;
    case VK_END: return KM_KEY_END;
    case VK_TAB: return KM_KEY_TAB;
    case VK_DELETE: return KM_KEY_DELETE;
    default: return 0;
    }
}

int km_platform_read_event(KmPlatform *platform, KmEvent *event,
                           char *error, size_t error_cap)
{
    if (platform == NULL || event == NULL) {
        set_error(error, error_cap, "read event: invalid argument");
        return -1;
    }
    if (!platform->input_console) {
        for (;;) {
            unsigned char byte;
            DWORD read_count = 0;
            if (platform->pending_byte_valid) {
                byte = platform->pending_byte;
                platform->pending_byte_valid = 0;
                if (decode_redirected_key(platform, byte, event)) return 0;
                continue;
            }
            if (platform->eof_pending) {
                platform->eof_pending = 0;
                memset(event, 0, sizeof(*event));
                event->kind = KM_EVENT_EOF;
                return 0;
            }
            if (platform->discard_escape) {
                return discard_redirected_escape(platform, event,
                                                 error, error_cap);
            }
            if (!ReadFile(platform->input, &byte, 1, &read_count, NULL)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) {
                    read_count = 0;
                } else {
                    set_windows_error(error, error_cap, "read input");
                    return -1;
                }
            }
            if (read_count == 0) {
                if (platform->utf8_needed != 0) {
                    platform->utf8_needed = 0;
                    platform->eof_pending = 1;
                    finish_key(event, 0xFFFD, 0, 1);
                    return 0;
                }
                memset(event, 0, sizeof(*event));
                event->kind = KM_EVENT_EOF;
                return 0;
            }
            if (platform->utf8_needed == 0 && byte == 0x1b) {
                return decode_redirected_escape(platform, event,
                                                error, error_cap);
            }
            if (decode_redirected_key(platform, byte, event)) {
                return 0;
            }
        }
    }

    for (;;) {
        INPUT_RECORD record;
        DWORD count = 0;
        KEY_EVENT_RECORD *key;
        uint32_t codepoint;
        uint32_t modifiers;

        if (!ReadConsoleInputW(platform->input, &record, 1, &count)) {
            set_windows_error(error, error_cap, "read console input");
            return -1;
        }
        if (count == 0) {
            memset(event, 0, sizeof(*event));
            event->kind = KM_EVENT_EOF;
            return 0;
        }
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            update_size(platform);
            memset(event, 0, sizeof(*event));
            event->kind = KM_EVENT_RESIZE;
            event->columns = platform->columns;
            event->rows = platform->rows;
            return 0;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
            continue;
        }
        key = &record.Event.KeyEvent;
        if (key->uChar.UnicodeChar >= 0xD800 &&
            key->uChar.UnicodeChar <= 0xDBFF) {
            platform->high_surrogate = key->uChar.UnicodeChar;
            continue;
        }
        if (key->uChar.UnicodeChar >= 0xDC00 &&
            key->uChar.UnicodeChar <= 0xDFFF) {
            if (platform->high_surrogate == 0) {
                continue;
            }
            codepoint = 0x10000u +
                        (((uint32_t)platform->high_surrogate - 0xD800u) << 10) +
                        ((uint32_t)key->uChar.UnicodeChar - 0xDC00u);
            platform->high_surrogate = 0;
        } else {
            platform->high_surrogate = 0;
            codepoint = key->uChar.UnicodeChar;
        }
        if (key->wVirtualKeyCode == VK_ESCAPE) {
            memset(event, 0, sizeof(*event));
            event->kind = KM_EVENT_KEY;
            event->codepoint = KM_KEY_ESCAPE;
            event->modifiers = console_modifiers(key->dwControlKeyState);
            event->repeat = key->wRepeatCount == 0 ? 1 : key->wRepeatCount;
            return 0;
        }
        if (codepoint == 0) {
            uint32_t named = named_console_key(key->wVirtualKeyCode);
            if (key->wVirtualKeyCode == VK_SPACE &&
                (key->dwControlKeyState &
                 (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0) {
                finish_key(event, 0, console_modifiers(key->dwControlKeyState),
                           key->wRepeatCount == 0 ? 1 : key->wRepeatCount);
                return 0;
            }
            if (named == 0) continue;
            memset(event, 0, sizeof(*event));
            event->kind = KM_EVENT_KEY;
            event->codepoint = named;
            event->modifiers = console_modifiers(key->dwControlKeyState);
            event->repeat = key->wRepeatCount == 0 ? 1 : key->wRepeatCount;
            return 0;
        }
        modifiers = console_modifiers(key->dwControlKeyState);
        if ((key->dwControlKeyState & RIGHT_ALT_PRESSED) &&
            (key->dwControlKeyState & LEFT_CTRL_PRESSED) && codepoint >= 0x20) {
            modifiers &= ~(KM_MOD_CTRL | KM_MOD_ALT);
        }
        finish_key(event, codepoint, modifiers,
                   key->wRepeatCount == 0 ? 1 : key->wRepeatCount);
        return 0;
    }
}
