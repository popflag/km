/*
 * nob.h v3.10.0, vendored from tsoding/nob.h commit
 * c54fde95599c15b881472087d4956e290f397d65.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#if defined(_MSC_VER)
#define NOB_REBUILD_URSELF(binary_path, source_path) \
    "cl.exe", "/nologo", "/std:c17", nob_temp_sprintf("/Fe:%s", (binary_path)), source_path
#elif defined(__clang__)
#define NOB_REBUILD_URSELF(binary_path, source_path) \
    "clang", "-std=c17", "-x", "c", "-o", binary_path, source_path
#elif defined(__GNUC__)
#define NOB_REBUILD_URSELF(binary_path, source_path) \
    "gcc", "-std=c17", "-x", "c", "-o", binary_path, source_path
#else
#define NOB_REBUILD_URSELF(binary_path, source_path) \
    "cc", "-std=c17", "-x", "c", "-o", binary_path, source_path
#endif

#define NOB_IMPLEMENTATION
#include "nob.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define KM_PLATFORM_SOURCE "src/platform_win32.c"
#define KM_EXE_SUFFIX ".exe"
#define KM_UTF8PROC_OBJECT "build/utf8proc-msvc.obj"
#define KM_TOOLCHAIN_ID "msvc-c17-v1"
#else
#define KM_PLATFORM_SOURCE "src/platform_posix.c"
#define KM_EXE_SUFFIX ""
#if defined(__clang__)
#define KM_UTF8PROC_OBJECT "build/utf8proc-clang.o"
#define KM_TOOLCHAIN_ID "clang-c17-v1"
#elif defined(__GNUC__)
#define KM_UTF8PROC_OBJECT "build/utf8proc-gcc.o"
#define KM_TOOLCHAIN_ID "gcc-c17-v1"
#else
#define KM_UTF8PROC_OBJECT "build/utf8proc-cc.o"
#define KM_TOOLCHAIN_ID "cc-c17-v1"
#endif
#endif

#define KM_TOOLCHAIN_MARKER "build/.toolchain"

static bool toolchain_changed;
static bool sanitize;

static const char *toolchain_id(void)
{
    return sanitize ? KM_TOOLCHAIN_ID "-sanitize" : KM_TOOLCHAIN_ID;
}

static const char *const km_sources[] = {
    "src/base.c",
    "src/document.c",
    "src/editor.c",
    "src/render.c",
};

static char *join_path(const char *parent, const char *name)
{
    size_t parent_len = strlen(parent);
    size_t name_len = strlen(name);
    char *path;

    if (parent_len > SIZE_MAX - name_len - 2) return NULL;
    path = malloc(parent_len + name_len + 2);
    if (path == NULL) return NULL;
    memcpy(path, parent, parent_len);
    path[parent_len] = '/';
    memcpy(path + parent_len + 1, name, name_len + 1);
    return path;
}

#ifdef _WIN32
static bool remove_tree(const char *path)
{
    DWORD attributes = GetFileAttributesA(path);

    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
        nob_log(NOB_ERROR, "could not inspect %s: Windows error %lu",
                path, (unsigned long)code);
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        BOOL removed = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                           ? RemoveDirectoryA(path)
                           : DeleteFileA(path);
        if (!removed) {
            nob_log(NOB_ERROR,
                    "could not delete reparse point %s: Windows error %lu",
                    path, (unsigned long)GetLastError());
            return false;
        }
        return true;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        if (!DeleteFileA(path)) {
            nob_log(NOB_ERROR, "could not delete %s: Windows error %lu",
                    path, (unsigned long)GetLastError());
            return false;
        }
        return true;
    }
    {
        WIN32_FIND_DATAA data;
        char *pattern = join_path(path, "*");
        HANDLE find;
        bool ok = true;

        if (pattern == NULL) return false;
        find = FindFirstFileA(pattern, &data);
        free(pattern);
        if (find == INVALID_HANDLE_VALUE) {
            nob_log(NOB_ERROR, "could not enumerate %s: Windows error %lu",
                    path, (unsigned long)GetLastError());
            return false;
        }
        do {
            char *child;
            if (strcmp(data.cFileName, ".") == 0 ||
                strcmp(data.cFileName, "..") == 0) continue;
            child = join_path(path, data.cFileName);
            if (child == NULL || !remove_tree(child)) ok = false;
            free(child);
            if (!ok) break;
        } while (FindNextFileA(find, &data));
        FindClose(find);
        if (!ok) return false;
    }
    if (!RemoveDirectoryA(path)) {
        nob_log(NOB_ERROR, "could not delete directory %s: Windows error %lu",
                path, (unsigned long)GetLastError());
        return false;
    }
    return true;
}
#else
static bool remove_tree(const char *path)
{
    struct stat info;

    if (lstat(path, &info) != 0) {
        if (errno == ENOENT) return true;
        nob_log(NOB_ERROR, "could not inspect %s: %s", path, strerror(errno));
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        if (unlink(path) != 0) {
            nob_log(NOB_ERROR, "could not delete %s: %s", path, strerror(errno));
            return false;
        }
        return true;
    }
    {
        DIR *directory = opendir(path);
        struct dirent *entry;
        bool ok = true;

        if (directory == NULL) {
            nob_log(NOB_ERROR, "could not enumerate %s: %s", path, strerror(errno));
            return false;
        }
        while ((entry = readdir(directory)) != NULL) {
            char *child;
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) continue;
            child = join_path(path, entry->d_name);
            if (child == NULL || !remove_tree(child)) ok = false;
            free(child);
            if (!ok) break;
        }
        closedir(directory);
        if (!ok) return false;
    }
    if (rmdir(path) != 0) {
        nob_log(NOB_ERROR, "could not delete directory %s: %s", path, strerror(errno));
        return false;
    }
    return true;
}
#endif

static bool clean(void)
{
    return remove_tree("build");
}

static bool ensure_toolchain_marker(void)
{
    char current[64] = {0};
    FILE *file = fopen(KM_TOOLCHAIN_MARKER, "rb");

    if (file != NULL) {
        (void)fread(current, 1, sizeof(current) - 1, file);
        fclose(file);
        if (strcmp(current, toolchain_id()) == 0) return true;
    }
    file = fopen(KM_TOOLCHAIN_MARKER, "wb");
    if (file == NULL) {
        nob_log(NOB_ERROR, "could not write %s: %s",
                KM_TOOLCHAIN_MARKER, strerror(errno));
        return false;
    }
    if (fwrite(toolchain_id(), 1, strlen(toolchain_id()), file) !=
        strlen(toolchain_id())) {
        nob_log(NOB_ERROR, "could not write %s", KM_TOOLCHAIN_MARKER);
        fclose(file);
        return false;
    }
    if (fclose(file) != 0) {
        nob_log(NOB_ERROR, "could not close %s", KM_TOOLCHAIN_MARKER);
        return false;
    }
    toolchain_changed = true;
    return true;
}

static void append_common_flags(Nob_Cmd *cmd, bool project_code)
{
#if defined(_MSC_VER)
    nob_cmd_append(cmd, "/nologo", "/std:c17");
    if (project_code) {
        nob_cmd_append(cmd, "/W4", "/WX");
    } else {
        nob_cmd_append(cmd, "/W3");
    }
    nob_cmd_append(cmd, "/DUTF8PROC_STATIC", "/Isrc", "/Ithird_party/utf8proc");
#else
    nob_cmd_append(cmd, "-std=c17");
    if (project_code) {
        nob_cmd_append(cmd,
                       "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                       "-Wconversion", "-Wshadow", "-Wstrict-prototypes");
    } else {
        nob_cmd_append(cmd, "-Wall", "-Wextra", "-Wpedantic");
    }
    nob_cmd_append(cmd, "-DUTF8PROC_STATIC", "-Isrc", "-Ithird_party/utf8proc");
    if (sanitize) {
        nob_cmd_append(cmd, "-fsanitize=address,undefined",
                       "-fno-sanitize-recover=all",
                       "-fno-omit-frame-pointer");
    }
#ifndef _WIN32
    if (project_code) nob_cmd_append(cmd, "-D_POSIX_C_SOURCE=200809L");
#endif
#endif
}

static void append_compiler(Nob_Cmd *cmd)
{
#if defined(_MSC_VER)
    nob_cmd_append(cmd, "cl.exe");
#elif defined(__clang__)
    nob_cmd_append(cmd, "clang");
#elif defined(__GNUC__)
    nob_cmd_append(cmd, "gcc");
#else
    nob_cmd_append(cmd, "cc");
#endif
}

static bool build_utf8proc(void)
{
    const char *inputs[] = {
        "nob.c",
        "nob.h",
        KM_TOOLCHAIN_MARKER,
        "third_party/utf8proc/utf8proc.c",
        "third_party/utf8proc/utf8proc.h",
        "third_party/utf8proc/utf8proc_data.c",
    };
    if (!toolchain_changed) {
        int rebuild = nob_needs_rebuild(KM_UTF8PROC_OBJECT, inputs,
                                        NOB_ARRAY_LEN(inputs));
        if (rebuild < 0) return false;
        if (rebuild == 0) return true;
    }

    Nob_Cmd cmd = {0};
    append_compiler(&cmd);
    append_common_flags(&cmd, false);
#if defined(_MSC_VER)
    nob_cmd_append(&cmd, "/c", "third_party/utf8proc/utf8proc.c",
                   "/Fo:build/utf8proc-msvc.obj");
#else
    nob_cmd_append(&cmd, "-c", "third_party/utf8proc/utf8proc.c",
                   "-o", KM_UTF8PROC_OBJECT);
#endif
    return nob_cmd_run(&cmd, .dont_reset = false);
}

static bool require_file(const char *path)
{
    if (!nob_file_exists(path)) {
        nob_log(NOB_ERROR, "required source is missing: %s", path);
        return false;
    }
    return true;
}

static bool build_executable(const char *name, const char *entry,
                             const char *const *sources, size_t source_count,
                             bool with_platform)
{
    if (!require_file(entry)) return false;
    for (size_t i = 0; i < source_count; ++i) {
        if (!require_file(sources[i])) return false;
    }
    if (with_platform && !require_file(KM_PLATFORM_SOURCE)) return false;

    const char *output = nob_temp_sprintf("build/%s%s", name, KM_EXE_SUFFIX);
    const char *inputs[32];
    size_t input_count = 0;
    Nob_Cmd cmd = {0};

    inputs[input_count++] = entry;
    for (size_t i = 0; i < source_count; ++i) {
        if (nob_file_exists(sources[i])) inputs[input_count++] = sources[i];
    }
    if (with_platform) inputs[input_count++] = KM_PLATFORM_SOURCE;
    inputs[input_count++] = "src/base.h";
    inputs[input_count++] = "src/document.h";
    inputs[input_count++] = "src/editor.h";
    inputs[input_count++] = "src/platform.h";
    inputs[input_count++] = "src/render.h";
    inputs[input_count++] = "third_party/utf8proc/utf8proc.h";
    inputs[input_count++] = KM_UTF8PROC_OBJECT;
    inputs[input_count++] = KM_TOOLCHAIN_MARKER;
    inputs[input_count++] = "nob.c";
    inputs[input_count++] = "nob.h";
    if (!toolchain_changed) {
        int rebuild = nob_needs_rebuild(output, inputs, input_count);
        if (rebuild < 0) return false;
        if (rebuild == 0) return true;
    }

    append_compiler(&cmd);
    append_common_flags(&cmd, true);
    nob_cmd_append(&cmd, entry);
    for (size_t i = 0; i < source_count; ++i) nob_cmd_append(&cmd, sources[i]);
    if (with_platform) nob_cmd_append(&cmd, KM_PLATFORM_SOURCE);
    nob_cmd_append(&cmd, KM_UTF8PROC_OBJECT);
#if defined(_MSC_VER)
    nob_cmd_append(&cmd, "/Fo:build/", nob_temp_sprintf("/Fe:%s", output));
#else
    nob_cmd_append(&cmd, "-o", output);
#endif
    return nob_cmd_run(&cmd, .dont_reset = false);
}

static bool build_km(void)
{
    return build_executable("km", "src/main.c",
                            km_sources, NOB_ARRAY_LEN(km_sources), true);
}

static bool build_test_document(void)
{
    const char *sources[] = {"src/base.c", "src/document.c"};
    return build_executable("test_document", "tests/test_document.c",
                            sources, NOB_ARRAY_LEN(sources), false);
}

static bool build_test_editor(void)
{
    const char *sources[] = {"src/base.c", "src/document.c", "src/editor.c"};
    return build_executable("test_editor", "tests/test_editor.c",
                            sources, NOB_ARRAY_LEN(sources), false);
}

static bool build_test_platform(void)
{
    const char *sources[] = {"src/base.c", "src/render.c"};
    return build_executable("test_platform", "tests/test_platform.c",
                            sources, NOB_ARRAY_LEN(sources), true);
}

static bool build_test_render(void)
{
    const char *sources[] = {"src/base.c", "src/render.c"};
    return build_executable("test_render", "tests/test_render.c",
                            sources, NOB_ARRAY_LEN(sources), false);
}

#ifndef _WIN32
static bool build_test_platform_pty(void)
{
    return build_executable("test_platform_pty", "tests/test_platform_pty.c",
                            NULL, 0, false);
}
#endif

static bool run_test(const char *name)
{
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, nob_temp_sprintf("./build/%s%s", name, KM_EXE_SUFFIX));
    return nob_cmd_run(&cmd, .dont_reset = false);
}

static bool test(void)
{
    if (!build_km() || !build_test_document() || !build_test_editor() ||
        !build_test_render() ||
        !build_test_platform()
#ifndef _WIN32
        || !build_test_platform_pty()
#endif
    ) {
        return false;
    }
    if (!run_test("test_document") || !run_test("test_editor") ||
        !run_test("test_render") ||
        !run_test("test_platform")) return false;
#ifndef _WIN32
    if (!run_test("test_platform_pty")) return false;
#endif
    return true;
}

static void usage(const char *program)
{
    nob_log(NOB_INFO, "usage: %s [build|test|sanitize|clean]", program);
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF_PLUS(argc, argv, "nob.h");

    const char *program = nob_shift(argv, argc);
    const char *target = argc > 0 ? nob_shift(argv, argc) : "build";
    if (argc != 0) {
        usage(program);
        return 1;
    }
    if (strcmp(target, "clean") == 0) return clean() ? 0 : 1;
    if (strcmp(target, "build") != 0 && strcmp(target, "test") != 0 &&
        strcmp(target, "sanitize") != 0) {
        usage(program);
        return 1;
    }
#if defined(_MSC_VER)
    if (strcmp(target, "sanitize") == 0) {
        nob_log(NOB_ERROR, "sanitize target requires GCC or Clang");
        return 1;
    }
#else
    sanitize = strcmp(target, "sanitize") == 0;
#endif

    if (!nob_mkdir_if_not_exists("build") ||
        !ensure_toolchain_marker() || !build_utf8proc()) return 1;
    if (strcmp(target, "test") == 0 || strcmp(target, "sanitize") == 0)
        return test() ? 0 : 1;
    return build_km() ? 0 : 1;
}
