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

int main(void)
{
    static const unsigned char input[] = {
        0x07, 0xC2, 'A', 0xE2, 0x82
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
        event.kind != KM_EVENT_KEY || event.codepoint != 0xFFFD) {
        km_platform_close(platform);
        return fail("invalid UTF-8 did not produce replacement");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_KEY || event.codepoint != 'A') {
        km_platform_close(platform);
        return fail("valid byte after invalid UTF-8 was lost");
    }
    if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0 ||
        event.kind != KM_EVENT_KEY || event.codepoint != 0xFFFD) {
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
    return 0;
}
