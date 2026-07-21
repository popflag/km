#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int same_termios(const struct termios *left,
                        const struct termios *right)
{
    return left->c_iflag == right->c_iflag &&
           left->c_oflag == right->c_oflag &&
           left->c_cflag == right->c_cflag &&
           left->c_lflag == right->c_lflag &&
           memcmp(left->c_cc, right->c_cc, sizeof(left->c_cc)) == 0;
}

static int wait_for_output(int master, char *output, size_t cap,
                           size_t *length, int timeout_ms)
{
    struct pollfd descriptor = {master, POLLIN, 0};
    int ready = poll(&descriptor, 1, timeout_ms);
    ssize_t count;

    if (ready <= 0 || (descriptor.revents & POLLIN) == 0) return -1;
    count = read(master, output + *length, cap - *length - 1);
    if (count <= 0) return -1;
    *length += (size_t)count;
    output[*length] = '\0';
    return 0;
}

static int wait_for_text(int master, char *output, size_t cap, size_t *length,
                         const char *text, int timeout_ms)
{
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        int slice = timeout_ms - elapsed;
        if (strstr(output, text) != NULL) return 0;
        if (slice > 100) slice = 100;
        (void)wait_for_output(master, output, cap, length, slice);
        elapsed += slice;
    }
    return strstr(output, text) == NULL ? -1 : 0;
}

static int run_probe(int terminate_with_signal, const char *path)
{
    int master = -1;
    int slave = -1;
    char *slave_name;
    struct termios before;
    struct termios after;
    struct winsize size = {37, 91, 0, 0};
    pid_t child;
    char output[8192] = {0};
    size_t output_len = 0;
    int status = 0;
    int iteration;
    int result = 1;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) goto done;
    slave_name = ptsname(master);
    if (slave_name == NULL) goto done;
    slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0 || tcgetattr(slave, &before) != 0) goto done;

    child = fork();
    if (child < 0) goto done;
    if (child == 0) {
        (void)setsid();
        (void)ioctl(slave, TIOCSCTTY, 0);
        (void)dup2(slave, STDIN_FILENO);
        (void)dup2(slave, STDOUT_FILENO);
        (void)dup2(slave, STDERR_FILENO);
        close(master);
        if (slave > STDERR_FILENO) close(slave);
        if (path == NULL) {
            execl("./build/km", "./build/km", (char *)NULL);
        } else {
            execl("./build/km", "./build/km", path, (char *)NULL);
        }
        _exit(127);
    }

    if (wait_for_text(master, output, sizeof(output), &output_len,
                      path == NULL ? "*scratch*" : path, 2000) != 0)
        goto kill_child;
    if (ioctl(master, TIOCSWINSZ, &size) != 0) goto kill_child;
    if (wait_for_text(master, output, sizeof(output), &output_len,
                      "\x1b[37;1H", 1000) != 0) {
        goto kill_child;
    }
    if (terminate_with_signal) {
        if (kill(child, SIGTERM) != 0) goto kill_child;
    } else if (path == NULL) {
        static const unsigned char edit[] = {
            'a', 'b', 'c', 0x02, 0x04, 0x7f,
            0x18, 'u', 0x18, 0x12,
            '\r', 0xe4, 0xb8, 0xad, 0x07, 'q',
            0x18, 0x03, 0x18, 0x03,
        };
        if (write(master, edit, sizeof(edit)) != (ssize_t)sizeof(edit)) {
            goto kill_child;
        }
        if (wait_for_text(master, output, sizeof(output), &output_len,
                          "\x1b[1;1Ha\x1b[2;1H\xe4\xb8\xadq", 2000) != 0) {
            goto kill_child;
        }
    } else {
        static const unsigned char edit[] = {
            0x1b, '[', 'B', 0x1b, '[', 'C', 0x00, 0x1b, '[', 'C',
            0x17, 0x19, 0xe4, 0xb8, 0xad, 0x18, 0x13,
        };
        static const unsigned char exit_keys[] = {0x18, 0x03};
        if (write(master, edit, sizeof(edit)) != (ssize_t)sizeof(edit)) {
            goto kill_child;
        }
        if (wait_for_text(master, output, sizeof(output), &output_len,
                          "Wrote", 2000) != 0 ||
            write(master, exit_keys, sizeof(exit_keys)) !=
                (ssize_t)sizeof(exit_keys)) {
            goto kill_child;
        }
    }

    for (iteration = 0; iteration < 30; ++iteration) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited < 0) goto kill_child;
        (void)wait_for_output(master, output, sizeof(output), &output_len, 100);
    }
    if (iteration == 30) goto kill_child;
    while (output_len + 1 < sizeof(output) &&
           wait_for_output(master, output, sizeof(output), &output_len, 0) == 0) {
    }
    if (tcgetattr(slave, &after) != 0 || !same_termios(&before, &after)) goto done;
    if (strstr(output, "\x1b[?1049h") == NULL ||
        strstr(output, "\x1b[?1049l") == NULL ||
        strstr(output, "\x1b[1;1H") == NULL ||
        strstr(output, path == NULL ? "*scratch*" : path) == NULL) goto done;
    if (terminate_with_signal) {
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) goto done;
    } else if (strstr(output, "abc") == NULL || !WIFEXITED(status) ||
               WEXITSTATUS(status) != 0) {
        goto done;
    }
    result = 0;
    goto done;

kill_child:
    (void)kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);

done:
    if (result != 0 && output_len != 0) {
        fprintf(stderr, "test_platform_pty captured %zu bytes: %s\n",
                output_len, output);
    }
    if (slave >= 0) close(slave);
    if (master >= 0) close(master);
    return result;
}

int main(void)
{
    char path[] = "/tmp/km-pty-file-XXXXXX";
    static const char initial[] = "abc\nxyz";
    static const char expected[] = "abc\nxy\xe4\xb8\xadz";
    char actual[32] = {0};
    int descriptor;
    ssize_t count;

    if (run_probe(0, NULL) != 0) {
        fprintf(stderr, "test_platform_pty: normal/resize probe failed: %s\n",
                strerror(errno));
        return 1;
    }
    descriptor = mkstemp(path);
    if (descriptor < 0 ||
        write(descriptor, initial, sizeof(initial) - 1) !=
            (ssize_t)(sizeof(initial) - 1) ||
        close(descriptor) != 0) {
        fprintf(stderr, "test_platform_pty: create file probe failed: %s\n",
                strerror(errno));
        return 1;
    }
    if (run_probe(0, path) != 0) {
        (void)unlink(path);
        fprintf(stderr, "test_platform_pty: file/save probe failed: %s\n",
                strerror(errno));
        return 1;
    }
    descriptor = open(path, O_RDONLY);
    count = descriptor < 0 ? -1 : read(descriptor, actual, sizeof(actual));
    if (descriptor >= 0) (void)close(descriptor);
    (void)unlink(path);
    if (count != (ssize_t)(sizeof(expected) - 1) ||
        memcmp(actual, expected, sizeof(expected) - 1) != 0) {
        fprintf(stderr, "test_platform_pty: saved bytes were incorrect\n");
        return 1;
    }
    if (run_probe(1, NULL) != 0) {
        fprintf(stderr, "test_platform_pty: signal restore probe failed: %s\n",
                strerror(errno));
        return 1;
    }
    return 0;
}
