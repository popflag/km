#include "file.h"

#include "utf8proc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef enum {
    KM_EOL_LF,
    KM_EOL_CRLF,
    KM_EOL_CR
} KmEolStyle;

typedef struct {
    dev_t device;
    ino_t inode;
    time_t change_time;
    time_t modify_time;
    long change_nanoseconds;
    long modify_nanoseconds;
    off_t size;
    nlink_t links;
} KmFileIdentity;

struct KmPath {
    char *bytes;
    char *display;
};

struct KmFile {
    KmPath *path;
    KmFileIdentity identity;
    mode_t mode;
    KmEolStyle eol;
    bool bom;
    bool exists;
};

static KmStatus fail(KmError *error, KmStatus status, const char *operation,
                     int os_code)
{
    if (error != NULL) {
        error->code = status;
        error->os_code = (uint64_t)(unsigned)os_code;
        error->operation = operation;
    }
    return status;
}

static bool same_identity(const KmFileIdentity *left,
                          const KmFileIdentity *right)
{
    return left->device == right->device && left->inode == right->inode &&
           left->change_time == right->change_time &&
           left->modify_time == right->modify_time &&
           left->change_nanoseconds == right->change_nanoseconds &&
           left->modify_nanoseconds == right->modify_nanoseconds &&
           left->size == right->size && left->links == right->links;
}

static KmFileIdentity identity_from_stat(const struct stat *info)
{
    KmFileIdentity identity;

    identity.device = info->st_dev;
    identity.inode = info->st_ino;
    identity.change_time = info->st_ctime;
    identity.modify_time = info->st_mtime;
#if defined(__APPLE__)
    identity.change_nanoseconds = info->st_ctimespec.tv_nsec;
    identity.modify_nanoseconds = info->st_mtimespec.tv_nsec;
#else
    identity.change_nanoseconds = info->st_ctim.tv_nsec;
    identity.modify_nanoseconds = info->st_mtim.tv_nsec;
#endif
    identity.size = info->st_size;
    identity.links = info->st_nlink;
    return identity;
}

static bool valid_target(const struct stat *info)
{
    return S_ISREG(info->st_mode) && info->st_nlink == 1;
}

static char hex_digit(unsigned value)
{
    return (char)(value < 10 ? '0' + value : 'A' + value - 10);
}

static KmStatus make_display_name(const char *bytes, char **out,
                                  KmError *error)
{
    size_t len = strlen(bytes);
    size_t cap;
    size_t input = 0;
    size_t output = 0;
    char *display;

    if (len > (SIZE_MAX - 1) / 4) {
        return fail(error, KM_ERR_INVALID, "display file name", 0);
    }
    cap = len * 4 + 1;
    display = (char *)malloc(cap);
    if (display == NULL) return fail(error, KM_ERR_OOM, "display file name", 0);
    while (input < len) {
        utf8proc_int32_t codepoint;
        utf8proc_ssize_t count = utf8proc_iterate(
            (const uint8_t *)bytes + input, (utf8proc_ssize_t)(len - input),
            &codepoint);
        if (count > 0 && codepoint >= 0x20 && codepoint != 0x7f) {
            memcpy(display + output, bytes + input, (size_t)count);
            output += (size_t)count;
            input += (size_t)count;
        } else {
            unsigned byte = (unsigned)(unsigned char)bytes[input++];
            display[output++] = '\\';
            display[output++] = 'x';
            display[output++] = hex_digit(byte >> 4);
            display[output++] = hex_digit(byte & 0x0f);
        }
    }
    display[output] = '\0';
    *out = display;
    return KM_OK;
}

static KmStatus path_from_bytes(const char *value, KmPath **out_path,
                                KmError *error)
{
    KmPath *path;
    size_t len;
    KmStatus status;

    if (out_path == NULL || value == NULL || value[0] == '\0') {
        return fail(error, KM_ERR_INVALID, "file path", 0);
    }
    *out_path = NULL;
    len = strlen(value);
    path = (KmPath *)calloc(1, sizeof(*path));
    if (path == NULL) return fail(error, KM_ERR_OOM, "file path", 0);
    path->bytes = (char *)malloc(len + 1);
    if (path->bytes == NULL) {
        free(path);
        return fail(error, KM_ERR_OOM, "file path", 0);
    }
    memcpy(path->bytes, value, len + 1);
    status = make_display_name(value, &path->display, error);
    if (status != KM_OK) {
        free(path->bytes);
        free(path);
        return status;
    }
    *out_path = path;
    return KM_OK;
}

KmStatus km_path_from_command_line(int argc, char **argv, KmPath **out_path,
                                   KmError *error)
{
    km_error_clear(error);
    if (out_path == NULL || argc < 1 || argv == NULL || argc > 2) {
        return fail(error, KM_ERR_INVALID, "command line", 0);
    }
    *out_path = NULL;
    return argc == 1 ? KM_OK : path_from_bytes(argv[1], out_path, error);
}

KmStatus km_path_from_utf8(const char *path, KmPath **out_path, KmError *error)
{
    km_error_clear(error);
    return path_from_bytes(path, out_path, error);
}

void km_path_destroy(KmPath *path)
{
    if (path == NULL) return;
    free(path->display);
    free(path->bytes);
    free(path);
}

static KmStatus read_all(int descriptor, size_t expected, uint8_t **out,
                         size_t *out_len, KmError *error)
{
    uint8_t *bytes = expected == 0 ? NULL : (uint8_t *)malloc(expected);
    size_t total = 0;
    unsigned char extra;

    if (expected != 0 && bytes == NULL) {
        return fail(error, KM_ERR_OOM, "read file", 0);
    }
    while (total < expected) {
        ssize_t count = read(descriptor, bytes + total, expected - total);
        if (count > 0) {
            total += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0) {
            int code = errno;
            free(bytes);
            return fail(error, KM_ERR_IO, "read file", code);
        } else {
            break;
        }
    }
    for (;;) {
        ssize_t count = read(descriptor, &extra, 1);
        if (count > 0) {
            free(bytes);
            return fail(error, KM_ERR_CONFLICT, "file changed while reading", 0);
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            int code = errno;
            free(bytes);
            return fail(error, KM_ERR_IO, "read file", code);
        }
        break;
    }
    if (total != expected) {
        free(bytes);
        return fail(error, KM_ERR_CONFLICT, "file changed while reading", 0);
    }
    *out = bytes;
    *out_len = total;
    return KM_OK;
}

static bool valid_utf8(const uint8_t *bytes, size_t len)
{
    size_t offset = 0;

    while (offset < len) {
        utf8proc_int32_t codepoint;
        utf8proc_ssize_t count = utf8proc_iterate(
            bytes + offset, (utf8proc_ssize_t)(len - offset), &codepoint);
        if (count <= 0) return false;
        offset += (size_t)count;
    }
    return true;
}

static KmStatus normalize_text(const uint8_t *bytes, size_t len,
                               KmFile *file, uint8_t **out_text,
                               size_t *out_len, KmError *error)
{
    size_t offset = 0;
    size_t output = 0;
    size_t lf = 0;
    size_t crlf = 0;
    size_t cr = 0;
    const uint8_t *body;
    uint8_t *text;

    file->bom = len >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb &&
                bytes[2] == 0xbf;
    if (file->bom) offset = 3;
    body = len == offset ? NULL : bytes + offset;
    if (!valid_utf8(body, len - offset)) {
        return fail(error, KM_ERR_INVALID, "file UTF-8", 0);
    }
    for (size_t i = offset; i < len; ++i) {
        if (bytes[i] == '\r') {
            if (i + 1 < len && bytes[i + 1] == '\n') {
                ++crlf;
                ++i;
            } else {
                ++cr;
            }
        } else if (bytes[i] == '\n') {
            ++lf;
        }
    }
    if ((lf != 0) + (crlf != 0) + (cr != 0) > 1) {
        return fail(error, KM_ERR_INVALID, "mixed file line endings", 0);
    }
    file->eol = crlf != 0 ? KM_EOL_CRLF : (cr != 0 ? KM_EOL_CR : KM_EOL_LF);
    text = len == offset ? NULL : (uint8_t *)malloc(len - offset);
    if (len != offset && text == NULL) {
        return fail(error, KM_ERR_OOM, "normalize file", 0);
    }
    while (offset < len) {
        if (bytes[offset] == '\r') {
            text[output++] = '\n';
            if (offset + 1 < len && bytes[offset + 1] == '\n') ++offset;
        } else {
            text[output++] = bytes[offset];
        }
        ++offset;
    }
    *out_text = text;
    *out_len = output;
    return KM_OK;
}

KmStatus km_file_load(KmPath *path, KmFile **out_file, uint8_t **out_text,
                      size_t *out_len, KmError *error)
{
    KmFile *file;
    struct stat before;
    struct stat after;
    KmFileIdentity before_identity;
    KmFileIdentity after_identity;
    uint8_t *bytes = NULL;
    size_t len = 0;
    int descriptor = -1;
    KmStatus status = KM_OK;

    km_error_clear(error);
    if (path == NULL || out_file == NULL || out_text == NULL ||
        out_len == NULL) {
        km_path_destroy(path);
        return fail(error, KM_ERR_INVALID, "load file", 0);
    }
    *out_file = NULL;
    *out_text = NULL;
    *out_len = 0;
    file = (KmFile *)calloc(1, sizeof(*file));
    if (file == NULL) {
        km_path_destroy(path);
        return fail(error, KM_ERR_OOM, "load file", 0);
    }
    file->path = path;
    file->eol = KM_EOL_LF;
    if (lstat(path->bytes, &before) != 0) {
        if (errno != ENOENT) {
            status = fail(error, KM_ERR_IO, "inspect file", errno);
            goto fail;
        }
        *out_file = file;
        return KM_OK;
    }
    if (!valid_target(&before)) {
        status = fail(error, KM_ERR_UNSUPPORTED, "unsafe file target", 0);
        goto fail;
    }
    if (before.st_size < 0 || (uintmax_t)before.st_size > (uintmax_t)PTRDIFF_MAX) {
        status = fail(error, KM_ERR_INVALID, "file too large", 0);
        goto fail;
    }
    descriptor = open(path->bytes, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        status = fail(error, KM_ERR_IO, "open file", errno);
        goto fail;
    }
    if (fstat(descriptor, &after) != 0 || !valid_target(&after) ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
        status = fail(error, KM_ERR_CONFLICT, "file changed while opening", errno);
        goto fail;
    }
    status = read_all(descriptor, (size_t)after.st_size, &bytes, &len, error);
    if (status != KM_OK) goto fail;
    before_identity = identity_from_stat(&after);
    if (fstat(descriptor, &before) != 0) {
        status = fail(error, KM_ERR_CONFLICT, "file changed while reading", errno);
        goto fail;
    }
    after_identity = identity_from_stat(&before);
    if (!same_identity(&before_identity, &after_identity)) {
        status = fail(error, KM_ERR_CONFLICT, "file changed while reading", 0);
        goto fail;
    }
    if (close(descriptor) != 0) {
        descriptor = -1;
        status = fail(error, KM_ERR_IO, "close file", errno);
        goto fail;
    }
    descriptor = -1;
    file->identity = identity_from_stat(&before);
    file->mode = before.st_mode & 0777;
    file->exists = true;
    status = normalize_text(bytes, len, file, out_text, out_len, error);
    if (status != KM_OK) goto fail;
    free(bytes);
    *out_file = file;
    return KM_OK;

fail:
    if (descriptor >= 0) (void)close(descriptor);
    free(bytes);
    km_file_destroy(file);
    return status;
}

void km_file_destroy(KmFile *file)
{
    if (file == NULL) return;
    km_path_destroy(file->path);
    free(file);
}

const char *km_file_display_name(const KmFile *file)
{
    return file == NULL || file->path == NULL ? "" : file->path->display;
}

bool km_file_same_target(const KmFile *left, const KmFile *right)
{
    if (left == NULL || right == NULL || left->path == NULL ||
        right->path == NULL || left->exists != right->exists) {
        return false;
    }
    if (left->exists) {
        return left->identity.device == right->identity.device &&
               left->identity.inode == right->identity.inode;
    }
    return strcmp(left->path->bytes, right->path->bytes) == 0;
}

static KmStatus write_all(int descriptor, const uint8_t *bytes, size_t len,
                          KmError *error)
{
    size_t total = 0;

    while (total < len) {
        ssize_t count = write(descriptor, bytes + total, len - total);
        if (count > 0) {
            total += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return fail(error, KM_ERR_IO, "write file",
                        count < 0 ? errno : EIO);
        }
    }
    return KM_OK;
}

static KmStatus write_document(int descriptor, const KmFile *file,
                               const KmDocument *document, KmError *error)
{
    static const uint8_t bom[] = {0xef, 0xbb, 0xbf};
    uint8_t input[4096];
    uint8_t output[8192];
    KmTextIter iterator;
    bool eof = false;
    KmStatus status;

    if (file->bom) {
        status = write_all(descriptor, bom, sizeof(bom), error);
        if (status != KM_OK) return status;
    }
    status = km_document_iter_init(document, (KmBytePos){0}, &iterator, error);
    if (status != KM_OK) return status;
    while (!eof) {
        size_t count;
        size_t output_len = 0;
        status = km_text_iter_read(&iterator, input, sizeof(input), &count,
                                   &eof, error);
        if (status != KM_OK) return status;
        for (size_t i = 0; i < count; ++i) {
            if (input[i] == '\n' && file->eol == KM_EOL_CRLF) {
                output[output_len++] = '\r';
                output[output_len++] = '\n';
            } else if (input[i] == '\n' && file->eol == KM_EOL_CR) {
                output[output_len++] = '\r';
            } else {
                output[output_len++] = input[i];
            }
        }
        status = write_all(descriptor, output, output_len, error);
        if (status != KM_OK) return status;
    }
    return KM_OK;
}

static char *temporary_template(const char *path)
{
    static const char name[] = ".km.XXXXXX";
    const char *slash = strrchr(path, '/');
    size_t prefix_len = slash == NULL ? 0 : (size_t)(slash - path) + 1;
    char *temporary;

    if (prefix_len > SIZE_MAX - sizeof(name)) return NULL;
    temporary = (char *)malloc(prefix_len + sizeof(name));
    if (temporary == NULL) return NULL;
    if (prefix_len != 0) memcpy(temporary, path, prefix_len);
    memcpy(temporary + prefix_len, name, sizeof(name));
    return temporary;
}

static void sync_parent_directory(const char *path)
{
    char *copy;
    char *slash;
    int descriptor;

    copy = (char *)malloc(strlen(path) + 1);
    if (copy == NULL) return;
    strcpy(copy, path);
    slash = strrchr(copy, '/');
    if (slash == NULL) {
        strcpy(copy, ".");
    } else if (slash == copy) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    descriptor = open(copy, O_RDONLY | O_CLOEXEC);
    if (descriptor >= 0) {
        (void)fsync(descriptor);
        (void)close(descriptor);
    }
    free(copy);
}

KmStatus km_file_save(KmFile *file, const KmDocument *document,
                      KmError *error)
{
    struct stat info;
    char *temporary;
    int descriptor = -1;
    bool renamed = false;
    KmStatus status;

    km_error_clear(error);
    if (file == NULL || document == NULL) {
        return fail(error, KM_ERR_INVALID, "save file", 0);
    }
    if (lstat(file->path->bytes, &info) == 0) {
        KmFileIdentity current = identity_from_stat(&info);
        if (!file->exists || !valid_target(&info) ||
            !same_identity(&current, &file->identity)) {
            return fail(error, KM_ERR_CONFLICT, "file changed on disk", 0);
        }
    } else if (errno != ENOENT) {
        return fail(error, KM_ERR_IO, "inspect file before save", errno);
    } else if (file->exists) {
        return fail(error, KM_ERR_CONFLICT, "file changed on disk", 0);
    }
    temporary = temporary_template(file->path->bytes);
    if (temporary == NULL) return fail(error, KM_ERR_OOM, "save file", 0);
    descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        status = fail(error, KM_ERR_IO, "create temporary file", errno);
        goto done;
    }
    if ((file->exists && fchmod(descriptor, file->mode) != 0) ||
        (!file->exists && fchmod(descriptor, 0600) != 0)) {
        status = fail(error, KM_ERR_IO, "set temporary file mode", errno);
        goto done;
    }
    status = write_document(descriptor, file, document, error);
    if (status != KM_OK) goto done;
    if (fsync(descriptor) != 0) {
        status = fail(error, KM_ERR_IO, "flush temporary file", errno);
        goto done;
    }
    if (close(descriptor) != 0) {
        descriptor = -1;
        status = fail(error, KM_ERR_IO, "close temporary file", errno);
        goto done;
    }
    descriptor = -1;
    if (file->exists) {
        if (rename(temporary, file->path->bytes) != 0) {
            status = fail(error, KM_ERR_IO, "replace file", errno);
            goto done;
        }
    } else if (link(temporary, file->path->bytes) != 0) {
        status = fail(error, errno == EEXIST ? KM_ERR_CONFLICT : KM_ERR_IO,
                      "create file", errno);
        goto done;
    }
    renamed = true;
    if (!file->exists) (void)unlink(temporary);
    sync_parent_directory(file->path->bytes);
    if (lstat(file->path->bytes, &info) != 0 || !valid_target(&info)) {
        status = fail(error, KM_ERR_IO, "inspect saved file", errno);
        goto done;
    }
    file->identity = identity_from_stat(&info);
    file->mode = info.st_mode & 0777;
    file->exists = true;
    status = KM_OK;

done:
    if (descriptor >= 0) (void)close(descriptor);
    if (!renamed) (void)unlink(temporary);
    free(temporary);
    return status;
}
