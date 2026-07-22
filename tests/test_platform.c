#include "platform.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct {
    HANDLE saved_input;
    HANDLE saved_output;
    HANDLE input_read;
    HANDLE output_read;
    HANDLE output_write;
} Capture;

static int capture_begin(Capture *capture, const unsigned char *input,
                         size_t input_len)
{
    HANDLE input_write;
    DWORD written;

    capture->saved_input = GetStdHandle(STD_INPUT_HANDLE);
    capture->saved_output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!CreatePipe(&capture->input_read, &input_write, NULL, 0) ||
        !CreatePipe(&capture->output_read, &capture->output_write, NULL, 0)) {
        return -1;
    }
    if (input_len > MAXDWORD ||
        !WriteFile(input_write, input, (DWORD)input_len, &written, NULL) ||
        written != (DWORD)input_len) {
        CloseHandle(input_write);
        return -1;
    }
    CloseHandle(input_write);
    if (!SetStdHandle(STD_INPUT_HANDLE, capture->input_read) ||
        !SetStdHandle(STD_OUTPUT_HANDLE, capture->output_write)) {
        return -1;
    }
    return 0;
}

static int capture_end(Capture *capture, char *bytes, size_t cap, size_t *length)
{
    DWORD count = 0;

    (void)SetStdHandle(STD_INPUT_HANDLE, capture->saved_input);
    (void)SetStdHandle(STD_OUTPUT_HANDLE, capture->saved_output);
    CloseHandle(capture->input_read);
    CloseHandle(capture->output_write);
    if (cap > MAXDWORD ||
        !ReadFile(capture->output_read, bytes, (DWORD)cap, &count, NULL)) {
        CloseHandle(capture->output_read);
        return -1;
    }
    CloseHandle(capture->output_read);
    *length = count;
    return 0;
}
#else
#include <unistd.h>

typedef struct {
    int saved_input;
    int saved_output;
    int output_read;
} Capture;

static int capture_begin(Capture *capture, const unsigned char *input,
                         size_t input_len)
{
    int input_pipe[2];
    int output_pipe[2];

    capture->saved_input = dup(STDIN_FILENO);
    capture->saved_output = dup(STDOUT_FILENO);
    if (capture->saved_input < 0 || capture->saved_output < 0 ||
        pipe(input_pipe) != 0 || pipe(output_pipe) != 0) {
        return -1;
    }
    if (input_len > (size_t)PTRDIFF_MAX ||
        write(input_pipe[1], input, input_len) != (ssize_t)input_len) {
        close(input_pipe[1]);
        return -1;
    }
    close(input_pipe[1]);
    if (dup2(input_pipe[0], STDIN_FILENO) < 0 ||
        dup2(output_pipe[1], STDOUT_FILENO) < 0) {
        return -1;
    }
    close(input_pipe[0]);
    close(output_pipe[1]);
    capture->output_read = output_pipe[0];
    return 0;
}

static int capture_end(Capture *capture, char *bytes, size_t cap, size_t *length)
{
    ssize_t count;

    if (dup2(capture->saved_input, STDIN_FILENO) < 0 ||
        dup2(capture->saved_output, STDOUT_FILENO) < 0) {
        return -1;
    }
    close(capture->saved_input);
    close(capture->saved_output);
    count = read(capture->output_read, bytes, cap);
    close(capture->output_read);
    if (count < 0) {
        return -1;
    }
    *length = (size_t)count;
    return 0;
}
#endif

static int fail(const char *message)
{
    fprintf(stderr, "test_platform: %s\n", message);
    return 1;
}

#ifndef _WIN32
static int test_paste_limit(void)
{
    static const unsigned char start[] = {0x1b, '[', '2', '0', '0', '~'};
    static const unsigned char end[] = {0x1b, '[', '2', '0', '1', '~'};
    static const unsigned char near_end[] = {0x1b, '[', '2', '0', '1', 'x'};
    FILE *input = tmpfile();
    FILE *output = tmpfile();
    int saved_input = -1;
    int saved_output = -1;
    KmPlatform *platform = NULL;
    KmEvent event;
    char error[256] = {0};
    int result = 1;
    size_t i;

    if (input == NULL || output == NULL ||
        fwrite(start, 1, sizeof(start), input) != sizeof(start)) goto done;
    for (i = 0; i < 1024u * 1024u - sizeof(near_end); ++i) {
        if (fputc('x', input) == EOF) goto done;
    }
    if (fwrite(near_end, 1, sizeof(near_end), input) != sizeof(near_end) ||
        fwrite(end, 1, sizeof(end), input) != sizeof(end) ||
        fwrite(start, 1, sizeof(start), input) != sizeof(start)) goto done;
    for (i = 0; i < 1024u * 1024u + 1u; ++i) {
        if (fputc('y', input) == EOF) goto done;
    }
    if (fwrite(end, 1, sizeof(end), input) != sizeof(end) ||
        fflush(input) != 0 || fseek(input, 0, SEEK_SET) != 0) goto done;
    saved_input = dup(STDIN_FILENO);
    saved_output = dup(STDOUT_FILENO);
    if (saved_input < 0 || saved_output < 0 ||
        dup2(fileno(input), STDIN_FILENO) < 0 ||
        dup2(fileno(output), STDOUT_FILENO) < 0) goto done;
    if (km_platform_open(&platform, error, sizeof(error)) != 0) goto done;
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_PASTE || event.text_len != 1024u * 1024u ||
        memcmp(event.text + event.text_len - sizeof(near_end), near_end,
               sizeof(near_end)) != 0) goto done;
    if (km_platform_read_event(platform, &event, error, sizeof(error)) == 0 ||
        strstr(error, "exceeds 1 MiB") == NULL) goto done;
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_EOF) goto done;
    result = 0;

done:
    km_platform_close(platform);
    if (saved_input >= 0) {
        (void)dup2(saved_input, STDIN_FILENO);
        close(saved_input);
    }
    if (saved_output >= 0) {
        (void)dup2(saved_output, STDOUT_FILENO);
        close(saved_output);
    }
    if (input != NULL) fclose(input);
    if (output != NULL) fclose(output);
    return result;
}
#endif

int main(void)
{
    static const unsigned char input[] = {
        0x07, 0x1f, '\t', 0xC2, 'A', '\r', 0x7f, 0x00,
        0x1b, '[', 'A', 0x1b, '[', '3', '~', 0x1b, '3',
        0x1b, '[', '2', '0', '0', '~', 'p', 0, 'q',
        0x1b, '[', '2', '0', '1', '~',
        0x1b, ']', 't', 'i', 't', 'l', 'e', 0x07, 'O',
        0x1b, '[',
        '1', '1', '1', '1', '1', '1', '1', '1', '1', '1',
        '1', '1', '1', '1', '1', '1', '1', '1', '1', '1', 'z', 'X',
        0xE2, 0x82
    };
    Capture capture;
    KmPlatform *platform = NULL;
    KmEvent event;
    char error[256];
    char output[512];
    size_t length;

    if (capture_begin(&capture, input, sizeof(input)) != 0) {
        return fail("could not redirect standard handles");
    }
    if (km_platform_open(&platform, error, sizeof(error)) != 0) {
        return fail(error);
    }
    if (km_platform_is_interactive(platform)) {
        km_platform_close(platform);
        return fail("pipes were detected as an interactive terminal");
    }
    if (km_platform_draw_probe(platform, error, sizeof(error)) != 0) {
        km_platform_close(platform);
        return fail(error);
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0) {
        km_platform_close(platform);
        return fail(error);
    }
    if (event.kind != KM_EVENT_KEY || event.codepoint != 'g' ||
        event.modifiers != KM_MOD_CTRL || event.repeat != 1) {
        km_platform_close(platform);
        return fail("C-g was not normalized as a control key event");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_KEY || event.codepoint != '/' ||
        event.modifiers != KM_MOD_CTRL) {
        km_platform_close(platform);
        return fail("C-/ was not normalized as a control key event");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_KEY || event.codepoint != KM_KEY_TAB ||
        event.modifiers != 0) {
        km_platform_close(platform);
        return fail("Tab was not normalized as a named key event");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_TEXT || event.codepoint != 0xFFFD) {
        km_platform_close(platform);
        return fail("invalid UTF-8 did not produce replacement");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_TEXT || event.codepoint != 'A') {
        km_platform_close(platform);
        return fail("valid byte after invalid UTF-8 was lost");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_TEXT || event.codepoint != '\n') {
        km_platform_close(platform);
        return fail("Return was not normalized as LF text");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_KEY || event.codepoint != 0x7f) {
        km_platform_close(platform);
        return fail("Backspace was not normalized as a key event");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_KEY || event.codepoint != ' ' ||
        event.modifiers != KM_MOD_CTRL) {
        km_platform_close(platform);
        return fail("C-Space was not normalized as a control key event");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_KEY || event.codepoint != KM_KEY_UP) {
        km_platform_close(platform);
        return fail("cursor-up sequence was not normalized");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_KEY || event.codepoint != KM_KEY_DELETE) {
        km_platform_close(platform);
        return fail("Delete sequence was not normalized");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_KEY || event.codepoint != '3' ||
        event.modifiers != KM_MOD_ALT) {
        km_platform_close(platform);
        return fail("Meta key was not normalized");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_PASTE || event.text_len != 3 ||
        memcmp(event.text, "p\0q", 3) != 0) {
        km_platform_close(platform);
        return fail("bracketed paste was not normalized as one event");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_TEXT || event.codepoint != 'O') {
        km_platform_close(platform);
        return fail("OSC payload leaked into text events");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_KEY || event.codepoint != KM_KEY_ESCAPE ||
        km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_TEXT || event.codepoint != 'X') {
        km_platform_close(platform);
        return fail("overlong CSI bytes leaked into text events");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_TEXT || event.codepoint != 0xFFFD) {
        km_platform_close(platform);
        return fail("truncated UTF-8 did not produce replacement");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0) {
        km_platform_close(platform);
        return fail(error);
    }
    if (event.kind != KM_EVENT_EOF) {
        km_platform_close(platform);
        return fail("closed redirected input did not produce EOF");
    }
    km_platform_close(platform);

    if (capture_end(&capture, output, sizeof(output) - 1, &length) != 0) {
        return fail("could not collect redirected output");
    }
    output[length] = '\0';
    if (strchr(output, '\x1b') != NULL) {
        return fail("non-interactive output contained a VT escape");
    }
    if (strstr(output, "ASCII: abc XYZ 123") == NULL ||
        strstr(output, "e\xCC\x81") == NULL ||
        strstr(output, "\xE6\x96\x87\xE6\x9C\xAC") == NULL) {
        return fail("probe output did not contain all text samples");
    }
#ifndef _WIN32
    if (test_paste_limit() != 0) {
        return fail("oversized bracketed paste was not rejected cleanly");
    }
#endif
    return 0;
}
