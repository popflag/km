#define _POSIX_C_SOURCE 200809L

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "editor.h"
#include "file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

int main(void)
{
    char directory[MAX_PATH];
    char path[MAX_PATH];
    char hard_link[MAX_PATH];
    char alias_path[MAX_PATH];
    char missing_lower[MAX_PATH];
    char missing_upper[MAX_PATH];
    char completion_prefix[MAX_PATH];
    char folder[MAX_PATH];
    char *completion = NULL;
    char temporary[MAX_PATH];
    KmPath *opaque = NULL;
    KmFile *file = NULL;
    KmFile *alias_file = NULL;
    KmFile *missing_file = NULL;
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    uint8_t *text = NULL;
    size_t len = 0;
    size_t alias_len = 0;
    uint8_t *alias_text = NULL;
    size_t missing_len = 0;
    uint8_t *missing_text = NULL;
    KmError error;
    FILE *stream;
    char actual[8] = {0};

    CHECK(GetTempPathA(MAX_PATH, directory) != 0);
    CHECK(GetTempFileNameA(directory, "km", 0, temporary) != 0);
    CHECK(DeleteFileA(temporary));
    CHECK(CreateDirectoryA(temporary, NULL));
    CHECK(snprintf(path, sizeof(path), "%s\\file.txt", temporary) > 0);
    stream = fopen(path, "wb");
    CHECK(stream != NULL);
    CHECK(fwrite("a\r\n", 1, 3, stream) == 3);
    CHECK(fclose(stream) == 0);
    CHECK(snprintf(completion_prefix, sizeof(completion_prefix), "%s\\fi",
                   temporary) > 0);
    CHECK(km_path_complete_utf8(completion_prefix, &completion, &error) ==
          KM_OK);
    CHECK(completion != NULL && strcmp(completion, path) == 0);
    free(completion);
    completion = NULL;
    CHECK(snprintf(folder, sizeof(folder), "%s\\folder", temporary) > 0);
    CHECK(CreateDirectoryA(folder, NULL));
    CHECK(snprintf(completion_prefix, sizeof(completion_prefix), "%s\\fol",
                   temporary) > 0);
    CHECK(km_path_complete_utf8(completion_prefix, &completion, &error) ==
          KM_OK);
    CHECK(completion != NULL);
    CHECK(snprintf(completion_prefix, sizeof(completion_prefix), "%s\\folder\\",
                   temporary) > 0);
    CHECK(strcmp(completion, completion_prefix) == 0);
    free(completion);
    CHECK(km_path_from_utf8(path, &opaque, &error) == KM_OK);
    CHECK(km_file_load(opaque, &file, &text, &len, &error) == KM_OK);
    CHECK(snprintf(alias_path, sizeof(alias_path), "%s\\.\\file.txt",
                   temporary) > 0);
    CHECK(km_path_from_utf8(alias_path, &opaque, &error) == KM_OK);
    CHECK(km_file_load(opaque, &alias_file, &alias_text, &alias_len, &error) ==
          KM_OK);
    CHECK(km_file_same_target(file, alias_file));
    free(alias_text);
    km_file_destroy(alias_file);
    CHECK(snprintf(missing_lower, sizeof(missing_lower), "%s\\missing.txt",
                   temporary) > 0);
    CHECK(snprintf(missing_upper, sizeof(missing_upper), "%s\\MISSING.txt",
                   temporary) > 0);
    CHECK(km_path_from_utf8(missing_lower, &opaque, &error) == KM_OK);
    CHECK(km_file_load(opaque, &alias_file, &alias_text, &alias_len, &error) ==
          KM_OK);
    CHECK(km_path_from_utf8(missing_upper, &opaque, &error) == KM_OK);
    CHECK(km_file_load(opaque, &missing_file, &missing_text, &missing_len,
                       &error) == KM_OK);
    CHECK(!km_file_same_target(alias_file, missing_file));
    free(missing_text);
    free(alias_text);
    km_file_destroy(missing_file);
    km_file_destroy(alias_file);
    CHECK(len == 2 && memcmp(text, "a\n", 2) == 0);
    CHECK(km_buffer_create_file(file, text, len, &buffer, &error) == KM_OK);
    free(text);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){2}, &error) == KM_OK);
    CHECK(km_view_insert_utf8_block(view, (const uint8_t *)"b", 1, &error) ==
          KM_OK);
    CHECK(km_buffer_save(buffer, &error) == KM_OK);
    CHECK(!km_buffer_is_modified(buffer));
    stream = fopen(path, "rb");
    CHECK(stream != NULL);
    CHECK(fread(actual, 1, sizeof(actual), stream) == 4);
    CHECK(fclose(stream) == 0);
    CHECK(memcmp(actual, "a\r\nb", 4) == 0);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
    CHECK(snprintf(hard_link, sizeof(hard_link), "%s\\hard.txt", temporary) > 0);
    CHECK(CreateHardLinkA(hard_link, path, NULL));
    CHECK(km_path_from_utf8(path, &opaque, &error) == KM_OK);
    CHECK(km_file_load(opaque, &file, &text, &len, &error) ==
          KM_ERR_UNSUPPORTED);
    CHECK(DeleteFileA(hard_link));
    CHECK(DeleteFileA(path));
    CHECK(RemoveDirectoryA(folder));
    CHECK(RemoveDirectoryA(temporary));
    puts("file tests passed");
    return 0;
}

#else

#include "editor.h"
#include "file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static void write_bytes(const char *path, const uint8_t *bytes, size_t len)
{
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(bytes, 1, len, file) == len);
    CHECK(fclose(file) == 0);
}

static size_t read_bytes(const char *path, uint8_t *bytes, size_t cap)
{
    FILE *file = fopen(path, "rb");
    size_t len;

    CHECK(file != NULL);
    len = fread(bytes, 1, cap, file);
    CHECK(!ferror(file));
    CHECK(fclose(file) == 0);
    return len;
}

static void path_join(char *out, size_t cap, const char *directory,
                      const char *name)
{
    CHECK(snprintf(out, cap, "%s/%s", directory, name) > 0);
}

static KmBuffer *load_buffer(const char *name, KmError *error)
{
    KmPath *path = NULL;
    KmFile *file = NULL;
    KmBuffer *buffer = NULL;
    uint8_t *text = NULL;
    size_t len = 0;

    CHECK(km_path_from_utf8(name, &path, error) == KM_OK);
    CHECK(km_file_load(path, &file, &text, &len, error) == KM_OK);
    CHECK(km_buffer_create_file(file, text, len, &buffer, error) == KM_OK);
    free(text);
    return buffer;
}

static void check_document(const KmBuffer *buffer, const uint8_t *expected,
                           size_t len)
{
    const KmDocument *document = km_buffer_document(buffer);
    uint8_t *actual = len == 0 ? NULL : (uint8_t *)malloc(len);
    KmError error;

    CHECK(km_document_len(document) == len);
    CHECK(km_document_copy(document, (KmBytePos){0}, len, actual, &error) ==
          KM_OK);
    CHECK(len == 0 || memcmp(actual, expected, len) == 0);
    free(actual);
}

int main(void)
{
    static const uint8_t disk_initial[] = {
        0xef, 0xbb, 0xbf, 'A', '\r', '\n',
        0xe4, 0xb8, 0xad, '\r', '\n',
    };
    static const uint8_t internal_initial[] = {
        'A', '\n', 0xe4, 0xb8, 0xad, '\n',
    };
    static const uint8_t disk_saved[] = {
        0xef, 0xbb, 0xbf, 'A', '\r', '\n',
        0xe4, 0xb8, 0xad, '\r', '\n', 'Z',
    };
    static const uint8_t invalid_utf8[] = {0xc0, 0x80};
    static const uint8_t mixed_eol[] = {'a', '\n', 'b', '\r', '\n'};
    static const uint8_t new_content[] = {'n', 0, '\n'};
    static const uint8_t cr_content[] = {'a', '\r', 'b'};
    static const uint8_t cr_internal[] = {'a', '\n', 'b'};
    char directory[] = "/tmp/km-file-test-XXXXXX";
    char path[512];
    char other[512];
    char bad[512];
    char mixed[512];
    char link_path[512];
    char cr_path[512];
    char alias_path[512];
    char completion_dir[512];
    char unicode_dir[512];
    char invalid_dir[512];
    char completion_path[512];
    char invalid_path[512];
    char *completion = NULL;
    uint8_t disk[64];
    KmBuffer *buffer;
    KmBuffer *alias_buffer;
    KmView *view = NULL;
    KmError error;
    KmPath *opaque = NULL;
    KmFile *file = NULL;
    uint8_t *text = NULL;
    size_t len = 0;

    CHECK(mkdtemp(directory) != NULL);
    path_join(completion_dir, sizeof(completion_dir), directory, "complete");
    path_join(unicode_dir, sizeof(unicode_dir), directory, "unicode");
    path_join(invalid_dir, sizeof(invalid_dir), directory, "invalid-name");
    CHECK(mkdir(completion_dir, 0700) == 0);
    CHECK(mkdir(unicode_dir, 0700) == 0);
    CHECK(mkdir(invalid_dir, 0700) == 0);
    path_join(completion_path, sizeof(completion_path), completion_dir,
              "alpha.txt");
    write_bytes(completion_path, (const uint8_t *)"", 0);
    path_join(completion_path, sizeof(completion_path), completion_dir,
              "alpine.txt");
    write_bytes(completion_path, (const uint8_t *)"", 0);
    path_join(completion_path, sizeof(completion_path), completion_dir,
              "folder");
    CHECK(mkdir(completion_path, 0700) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/al", completion_dir) > 0);
    CHECK(km_path_complete_utf8(path, &completion, &error) == KM_OK);
    CHECK(completion != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/alp", completion_dir) > 0);
    CHECK(strcmp(completion, path) == 0);
    free(completion);
    completion = NULL;
    CHECK(snprintf(path, sizeof(path), "%s/fol", completion_dir) > 0);
    CHECK(km_path_complete_utf8(path, &completion, &error) == KM_OK);
    CHECK(snprintf(path, sizeof(path), "%s/folder/", completion_dir) > 0);
    CHECK(completion != NULL && strcmp(completion, path) == 0);
    free(completion);
    completion = NULL;
    CHECK(snprintf(completion_path, sizeof(completion_path),
                   "%s/\xc3\xa9.txt", unicode_dir) > 0);
    write_bytes(completion_path, (const uint8_t *)"", 0);
    CHECK(snprintf(completion_path, sizeof(completion_path),
                   "%s/\xc3\xaa.txt", unicode_dir) > 0);
    write_bytes(completion_path, (const uint8_t *)"", 0);
    CHECK(snprintf(path, sizeof(path), "%s/", unicode_dir) > 0);
    CHECK(km_path_complete_utf8(path, &completion, &error) == KM_OK);
    CHECK(completion != NULL && strcmp(completion, path) == 0);
    free(completion);
    completion = NULL;
    CHECK(snprintf(invalid_path, sizeof(invalid_path), "%s/", invalid_dir) > 0);
    {
        size_t invalid_len = strlen(invalid_path);
        CHECK(invalid_len + 2 < sizeof(invalid_path));
        invalid_path[invalid_len] = (char)0xff;
        invalid_path[invalid_len + 1] = '\0';
    }
    write_bytes(invalid_path, (const uint8_t *)"", 0);
    CHECK(km_path_complete_utf8(invalid_path, &completion, &error) ==
          KM_ERR_INVALID);
    CHECK(completion == NULL);
    CHECK(snprintf(path, sizeof(path), "%s/", invalid_dir) > 0);
    CHECK(km_path_complete_utf8(path, &completion, &error) == KM_OK);
    CHECK(completion == NULL);
    path_join(path, sizeof(path), directory, "visited.txt");
    path_join(other, sizeof(other), directory, "replacement.txt");
    path_join(bad, sizeof(bad), directory, "invalid.txt");
    path_join(mixed, sizeof(mixed), directory, "mixed.txt");
    path_join(link_path, sizeof(link_path), directory, "link.txt");
    path_join(cr_path, sizeof(cr_path), directory, "cr.txt");
    CHECK(snprintf(alias_path, sizeof(alias_path), "%s/./visited.txt",
                   directory) > 0);
    write_bytes(path, disk_initial, sizeof(disk_initial));

    buffer = load_buffer(path, &error);
    alias_buffer = load_buffer(alias_path, &error);
    CHECK(km_buffer_visits_same_file(buffer, alias_buffer));
    CHECK(km_buffer_destroy(alias_buffer, &error) == KM_OK);
    CHECK(km_buffer_is_visited(buffer));
    CHECK(strcmp(km_buffer_name(buffer), path) == 0);
    CHECK(!km_buffer_is_modified(buffer));
    check_document(buffer, internal_initial, sizeof(internal_initial));
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){sizeof(internal_initial)},
                            &error) == KM_OK);
    CHECK(km_view_insert_utf8_block(view, (const uint8_t *)"Z", 1, &error) ==
          KM_OK);
    CHECK(km_buffer_is_modified(buffer));
    CHECK(km_buffer_save(buffer, &error) == KM_OK);
    CHECK(!km_buffer_is_modified(buffer));
    CHECK(read_bytes(path, disk, sizeof(disk)) == sizeof(disk_saved));
    CHECK(memcmp(disk, disk_saved, sizeof(disk_saved)) == 0);
    CHECK(km_view_undo(view, &error) == KM_OK);
    CHECK(km_buffer_is_modified(buffer));
    CHECK(km_view_redo(view, &error) == KM_OK);
    CHECK(!km_buffer_is_modified(buffer));

    CHECK(km_view_insert_utf8_block(view, (const uint8_t *)"Q", 1, &error) ==
          KM_OK);
    write_bytes(other, (const uint8_t *)"external", 8);
    CHECK(rename(other, path) == 0);
    CHECK(km_buffer_save(buffer, &error) == KM_ERR_CONFLICT);
    CHECK(km_buffer_is_modified(buffer));
    CHECK(read_bytes(path, disk, sizeof(disk)) == 8);
    CHECK(memcmp(disk, "external", 8) == 0);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    path_join(path, sizeof(path), directory, "new.txt");
    buffer = load_buffer(path, &error);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_insert_utf8_block(view, new_content, sizeof(new_content),
                                    &error) == KM_OK);
    CHECK(km_buffer_save(buffer, &error) == KM_OK);
    CHECK(!km_buffer_is_modified(buffer));
    CHECK(read_bytes(path, disk, sizeof(disk)) == sizeof(new_content));
    CHECK(memcmp(disk, new_content, sizeof(new_content)) == 0);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    write_bytes(cr_path, cr_content, sizeof(cr_content));
    buffer = load_buffer(cr_path, &error);
    check_document(buffer, cr_internal, sizeof(cr_internal));
    CHECK(km_buffer_save(buffer, &error) == KM_OK);
    CHECK(read_bytes(cr_path, disk, sizeof(disk)) == sizeof(cr_content));
    CHECK(memcmp(disk, cr_content, sizeof(cr_content)) == 0);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    write_bytes(bad, invalid_utf8, sizeof(invalid_utf8));
    CHECK(km_path_from_utf8(bad, &opaque, &error) == KM_OK);
    CHECK(km_file_load(opaque, &file, &text, &len, &error) == KM_ERR_INVALID);
    opaque = NULL;
    write_bytes(mixed, mixed_eol, sizeof(mixed_eol));
    CHECK(km_path_from_utf8(mixed, &opaque, &error) == KM_OK);
    CHECK(km_file_load(opaque, &file, &text, &len, &error) == KM_ERR_INVALID);
    opaque = NULL;

    CHECK(symlink(path, link_path) == 0);
    CHECK(km_path_from_utf8(link_path, &opaque, &error) == KM_OK);
    CHECK(km_file_load(opaque, &file, &text, &len, &error) ==
          KM_ERR_UNSUPPORTED);
    opaque = NULL;

    CHECK(unlink(link_path) == 0);
    CHECK(link(path, link_path) == 0);
    CHECK(km_path_from_utf8(path, &opaque, &error) == KM_OK);
    CHECK(km_file_load(opaque, &file, &text, &len, &error) ==
          KM_ERR_UNSUPPORTED);
    opaque = NULL;
    CHECK(unlink(link_path) == 0);
    CHECK(unlink(path) == 0);
    CHECK(unlink(bad) == 0);
    CHECK(unlink(mixed) == 0);
    CHECK(unlink(cr_path) == 0);
    CHECK(unlink(invalid_path) == 0);
    CHECK(rmdir(invalid_dir) == 0);
    CHECK(snprintf(completion_path, sizeof(completion_path),
                   "%s/\xc3\xa9.txt", unicode_dir) > 0);
    CHECK(unlink(completion_path) == 0);
    CHECK(snprintf(completion_path, sizeof(completion_path),
                   "%s/\xc3\xaa.txt", unicode_dir) > 0);
    CHECK(unlink(completion_path) == 0);
    CHECK(rmdir(unicode_dir) == 0);
    path_join(completion_path, sizeof(completion_path), completion_dir,
              "alpha.txt");
    CHECK(unlink(completion_path) == 0);
    path_join(completion_path, sizeof(completion_path), completion_dir,
              "alpine.txt");
    CHECK(unlink(completion_path) == 0);
    path_join(completion_path, sizeof(completion_path), completion_dir,
              "folder");
    CHECK(rmdir(completion_path) == 0);
    CHECK(rmdir(completion_dir) == 0);
    path_join(path, sizeof(path), directory, "visited.txt");
    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
    puts("file tests passed");
    return 0;
}

#endif
