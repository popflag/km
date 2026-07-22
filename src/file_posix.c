#include "file.h"
#include "file_text.h"

#include "utf8proc.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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
    KmFileTextFormat format;
    bool exists;
    bool identity_pending;
};

#ifdef KM_FILE_TESTING
static bool test_fail_post_commit_refresh;

void km_file_test_fail_post_commit_refresh_once(void)
{
    test_fail_post_commit_refresh = true;
}
#endif

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

static bool same_commit_witness(const KmFileIdentity *current,
                                const KmFileIdentity *witness)
{
    return current->device == witness->device &&
           current->inode == witness->inode &&
           current->modify_time == witness->modify_time &&
           current->modify_nanoseconds == witness->modify_nanoseconds &&
           current->size == witness->size &&
           current->links == witness->links;
}

static bool fail_post_commit_refresh(void)
{
#ifdef KM_FILE_TESTING
    if (test_fail_post_commit_refresh) {
        test_fail_post_commit_refresh = false;
        return true;
    }
#endif
    return false;
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

static bool strict_utf8_name(const char *name, size_t len)
{
    size_t offset = 0;

    while (offset < len) {
        utf8proc_int32_t codepoint;
        utf8proc_ssize_t consumed;
        if (len - offset > (size_t)PTRDIFF_MAX) return false;
        consumed = utf8proc_iterate(
            (const utf8proc_uint8_t *)name + offset,
            (utf8proc_ssize_t)(len - offset), &codepoint);
        if (consumed <= 0 || codepoint < 0x20 || codepoint == 0x7f) {
            return false;
        }
        offset += (size_t)consumed;
    }
    return true;
}

static size_t utf8_common_prefix(const char *left, size_t left_len,
                                 const char *right, size_t right_len)
{
    size_t common = left_len < right_len ? left_len : right_len;
    size_t offset = 0;

    while (offset < common && left[offset] == right[offset]) ++offset;
    common = offset;
    while (common != 0 && common < left_len &&
           ((unsigned char)left[common] & 0xc0u) == 0x80u) {
        --common;
    }
    return common;
}

static KmStatus completion_entry_is_directory(const char *directory,
                                              const char *name,
                                              bool *out_directory,
                                              KmError *error)
{
    size_t directory_len = strlen(directory);
    size_t name_len = strlen(name);
    char *path;
    struct stat info;
    int code;

    *out_directory = false;
    if (name_len > SIZE_MAX - 2 ||
        directory_len > SIZE_MAX - name_len - 2) {
        return fail(error, KM_ERR_OOM, "complete file path", 0);
    }
    path = (char *)malloc(directory_len + name_len + 2);
    if (path == NULL) return fail(error, KM_ERR_OOM, "complete file path", 0);
    memcpy(path, directory, directory_len);
    if (directory_len != 0 && directory[directory_len - 1] != '/') {
        path[directory_len++] = '/';
    }
    memcpy(path + directory_len, name, name_len + 1);
    if (stat(path, &info) != 0) {
        code = errno;
        free(path);
        return fail(error, KM_ERR_IO, "complete file path", code);
    }
    *out_directory = S_ISDIR(info.st_mode);
    free(path);
    return KM_OK;
}

static int compare_completion(const void *left, const void *right)
{
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return strcmp(*a, *b);
}

void km_path_completions_destroy(KmPathCompletions *completions)
{
    size_t i;
    if (completions == NULL) return;
    for (i = 0; i < completions->count; ++i) free(completions->items[i]);
    free(completions->items);
    free(completions->common);
    *completions = (KmPathCompletions){0};
}

KmStatus km_path_completions_utf8(const char *prefix,
                                  KmPathCompletions *out_completions,
                                  KmError *error)
{
    const char *slash;
    const char *name_prefix;
    size_t head_len;
    size_t prefix_len;
    char *directory = NULL;
    KmPathCompletions completions = {0};
    size_t capacity = 0;
    DIR *stream = NULL;
    int read_error = 0;
    KmStatus status = KM_OK;

    km_error_clear(error);
    if (prefix == NULL || out_completions == NULL) {
        return fail(error, KM_ERR_INVALID, "complete file path", 0);
    }
    *out_completions = (KmPathCompletions){0};
    if (!strict_utf8_name(prefix, strlen(prefix))) {
        return fail(error, KM_ERR_INVALID, "complete file path", 0);
    }
    slash = strrchr(prefix, '/');
    head_len = slash == NULL ? 0 : (size_t)(slash - prefix) + 1;
    name_prefix = prefix + head_len;
    prefix_len = strlen(name_prefix);
    if (head_len == 0) {
        directory = (char *)malloc(2);
        if (directory != NULL) memcpy(directory, ".", 2);
    } else {
        size_t directory_len = head_len == 1 ? 1 : head_len - 1;
        directory = (char *)malloc(directory_len + 1);
        if (directory != NULL) {
            memcpy(directory, prefix, directory_len);
            directory[directory_len] = '\0';
        }
    }
    if (directory == NULL) return fail(error, KM_ERR_OOM, "complete file path", 0);
    stream = opendir(directory);
    if (stream == NULL) {
        int code = errno;
        free(directory);
        if (code == ENOENT || code == ENOTDIR) return KM_OK;
        return fail(error, KM_ERR_IO, "complete file path", code);
    }
    for (;;) {
        struct dirent *entry;
        size_t name_len;
        errno = 0;
        entry = readdir(stream);
        if (entry == NULL) {
            read_error = errno;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        name_len = strlen(entry->d_name);
        if (!strict_utf8_name(entry->d_name, name_len) ||
            name_len < prefix_len ||
            (prefix_len != 0 &&
             memcmp(entry->d_name, name_prefix, prefix_len) != 0)) {
            continue;
        }
        {
            bool is_directory;
            size_t suffix;
            char *candidate;
            char **items;

            status = completion_entry_is_directory(
                directory, entry->d_name, &is_directory, error);
            if (status != KM_OK) break;
            suffix = is_directory ? 1 : 0;
            if (name_len > SIZE_MAX - head_len ||
                suffix > SIZE_MAX - head_len - name_len ||
                head_len + name_len + suffix == SIZE_MAX) {
                status = fail(error, KM_ERR_OOM, "complete file path", 0);
                break;
            }
            candidate = (char *)malloc(head_len + name_len + suffix + 1);
            if (candidate == NULL) {
                status = fail(error, KM_ERR_OOM, "complete file path", 0);
                break;
            }
            memcpy(candidate, prefix, head_len);
            memcpy(candidate + head_len, entry->d_name, name_len);
            if (suffix != 0) candidate[head_len + name_len] = '/';
            candidate[head_len + name_len + suffix] = '\0';
            if (completions.count == capacity) {
                size_t new_capacity = capacity == 0 ? 8 : capacity * 2;
                if (new_capacity < capacity ||
                    new_capacity > SIZE_MAX / sizeof(*items)) {
                    free(candidate);
                    status = fail(error, KM_ERR_OOM, "complete file path", 0);
                    break;
                }
                items = (char **)realloc(
                    completions.items, new_capacity * sizeof(*items));
                if (items == NULL) {
                    free(candidate);
                    status = fail(error, KM_ERR_OOM, "complete file path", 0);
                    break;
                }
                completions.items = items;
                capacity = new_capacity;
            }
            completions.items[completions.count++] = candidate;
        }
    }
    if (status == KM_OK && read_error != 0) {
        status = fail(error, KM_ERR_IO, "complete file path", read_error);
    }
    if (closedir(stream) != 0 && status == KM_OK) {
        status = fail(error, KM_ERR_IO, "complete file path", errno);
    }
    if (status == KM_OK && completions.count != 0) {
        size_t common_len;
        size_t i;
        qsort(completions.items, completions.count,
              sizeof(*completions.items), compare_completion);
        common_len = strlen(completions.items[0]);
        for (i = 1; i < completions.count; ++i) {
            common_len = utf8_common_prefix(
                completions.items[0], common_len,
                completions.items[i], strlen(completions.items[i]));
        }
        completions.common = (char *)malloc(common_len + 1);
        if (completions.common == NULL) {
            status = fail(error, KM_ERR_OOM, "complete file path", 0);
        } else {
            memcpy(completions.common, completions.items[0], common_len);
            completions.common[common_len] = '\0';
        }
    }
    if (status != KM_OK) {
        km_path_completions_destroy(&completions);
    } else {
        *out_completions = completions;
    }
    free(directory);
    return status;
}

KmStatus km_path_complete_utf8(const char *prefix, char **out_completion,
                               KmError *error)
{
    KmPathCompletions completions = {0};
    KmStatus status;

    if (out_completion == NULL) {
        return fail(error, KM_ERR_INVALID, "complete file path", 0);
    }
    *out_completion = NULL;
    status = km_path_completions_utf8(prefix, &completions, error);
    if (status == KM_OK) {
        *out_completion = completions.common;
        completions.common = NULL;
    }
    km_path_completions_destroy(&completions);
    return status;
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
    file->format.eol = KM_EOL_LF;
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
    if (fstat(descriptor, &after) != 0) {
        status = fail(error, KM_ERR_IO, "inspect opened file", errno);
        goto fail;
    }
    if (!valid_target(&after)) {
        status = fail(error, KM_ERR_UNSUPPORTED, "unsafe file target", 0);
        goto fail;
    }
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
        status = fail(error, KM_ERR_CONFLICT, "file changed while opening", 0);
        goto fail;
    }
    status = read_all(descriptor, (size_t)after.st_size, &bytes, &len, error);
    if (status != KM_OK) goto fail;
    before_identity = identity_from_stat(&after);
    if (fstat(descriptor, &before) != 0) {
        status = fail(error, KM_ERR_IO, "inspect read file", errno);
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
    status = km_file_text_decode(bytes, len, &file->format,
                                 out_text, out_len, error);
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

static KmStatus write_file_text(void *context, const uint8_t *bytes,
                                size_t len, KmError *error)
{
    return write_all(*(const int *)context, bytes, len, error);
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
                      KmFileSaveResult *result, KmError *error)
{
    struct stat info;
    struct stat saved_info;
    KmFileIdentity witness;
    mode_t witness_mode;
    char *temporary;
    int descriptor = -1;
    bool committed = false;
    bool was_existing;
    KmStatus status;

    km_error_clear(error);
    if (result != NULL) *result = (KmFileSaveResult){0};
    if (file == NULL || document == NULL || result == NULL) {
        return fail(error, KM_ERR_INVALID, "save file", 0);
    }
    if (lstat(file->path->bytes, &info) == 0) {
        KmFileIdentity current = identity_from_stat(&info);
        if (file->identity_pending) {
            if (!valid_target(&info) ||
                !same_commit_witness(&current, &file->identity) ||
                (info.st_mode & 0777) != file->mode) {
                return fail(error, KM_ERR_CONFLICT,
                            "file changed after committed save", 0);
            }
            file->identity = current;
            file->mode = info.st_mode & 0777;
            file->exists = true;
            file->identity_pending = false;
        } else if (!file->exists || !valid_target(&info) ||
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
    status = km_file_text_write_document(&file->format, document, &descriptor,
                                         write_file_text, error);
    if (status != KM_OK) goto done;
    if (fsync(descriptor) != 0) {
        status = fail(error, KM_ERR_IO, "flush temporary file", errno);
        goto done;
    }
    if (fstat(descriptor, &saved_info) != 0 || !valid_target(&saved_info)) {
        status = fail(error, KM_ERR_IO, "inspect temporary file",
                      errno);
        goto done;
    }
    witness = identity_from_stat(&saved_info);
    witness_mode = saved_info.st_mode & 0777;
    was_existing = file->exists;
    if (was_existing) {
        if (rename(temporary, file->path->bytes) != 0) {
            status = fail(error, KM_ERR_IO, "replace file", errno);
            goto done;
        }
    } else if (link(temporary, file->path->bytes) != 0) {
        status = fail(error, errno == EEXIST ? KM_ERR_CONFLICT : KM_ERR_IO,
                      "create file", errno);
        goto done;
    }
    committed = true;
    result->committed = true;
    file->identity = witness;
    file->mode = witness_mode;
    file->exists = true;
    file->identity_pending = true;
    if (!was_existing && unlink(temporary) != 0) {
        status = fail(error, KM_ERR_IO,
                      "saved content committed; remove temporary link",
                      errno);
    }
    sync_parent_directory(file->path->bytes);
    if (fail_post_commit_refresh()) {
        if (status == KM_OK) {
            status = fail(error, KM_ERR_IO,
                          "saved content committed; inspect file", EIO);
        }
    } else if (fstat(descriptor, &saved_info) != 0 ||
               !valid_target(&saved_info)) {
        if (status == KM_OK) {
            status = fail(error, KM_ERR_IO,
                          "saved content committed; inspect file", errno);
        }
    } else {
        file->identity = identity_from_stat(&saved_info);
        file->mode = saved_info.st_mode & 0777;
        file->identity_pending = false;
    }
    if (close(descriptor) != 0) {
        descriptor = -1;
        if (status == KM_OK) {
            status = fail(error, KM_ERR_IO,
                          "saved content committed; close file", errno);
        }
    } else {
        descriptor = -1;
    }

done:
    if (descriptor >= 0) (void)close(descriptor);
    if (!committed) (void)unlink(temporary);
    free(temporary);
    return status;
}
