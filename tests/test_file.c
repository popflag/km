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
    char temporary[MAX_PATH];
    KmPath *opaque = NULL;
    KmFile *file = NULL;
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    uint8_t *text = NULL;
    size_t len = 0;
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
    CHECK(km_path_from_utf8(path, &opaque, &error) == KM_OK);
    CHECK(km_file_load(opaque, &file, &text, &len, &error) == KM_OK);
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
    uint8_t disk[64];
    KmBuffer *buffer;
    KmView *view = NULL;
    KmError error;
    KmPath *opaque = NULL;
    KmFile *file = NULL;
    uint8_t *text = NULL;
    size_t len = 0;

    CHECK(mkdtemp(directory) != NULL);
    path_join(path, sizeof(path), directory, "visited.txt");
    path_join(other, sizeof(other), directory, "replacement.txt");
    path_join(bad, sizeof(bad), directory, "invalid.txt");
    path_join(mixed, sizeof(mixed), directory, "mixed.txt");
    path_join(link_path, sizeof(link_path), directory, "link.txt");
    path_join(cr_path, sizeof(cr_path), directory, "cr.txt");
    write_bytes(path, disk_initial, sizeof(disk_initial));

    buffer = load_buffer(path, &error);
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
    path_join(path, sizeof(path), directory, "visited.txt");
    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
    puts("file tests passed");
    return 0;
}

#endif
