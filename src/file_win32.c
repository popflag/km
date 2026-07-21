#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "file.h"

#include "utf8proc.h"

#include <stdbool.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef enum {
    KM_EOL_LF,
    KM_EOL_CRLF,
    KM_EOL_CR
} KmEolStyle;

typedef struct {
    DWORD volume;
    DWORD index_high;
    DWORD index_low;
    FILETIME write_time;
    DWORD size_high;
    DWORD size_low;
    DWORD links;
    DWORD attributes;
} KmFileIdentity;

struct KmPath {
    WCHAR *wide;
    char *display;
};

struct KmFile {
    KmPath *path;
    KmFileIdentity identity;
    DWORD attributes;
    KmEolStyle eol;
    bool bom;
    bool exists;
};

static volatile LONG temporary_counter;

static KmStatus fail(KmError *error, KmStatus status, const char *operation,
                     DWORD os_code)
{
    if (error != NULL) {
        error->code = status;
        error->os_code = os_code;
        error->operation = operation;
    }
    return status;
}

static bool same_identity(const KmFileIdentity *left,
                          const KmFileIdentity *right)
{
    return left->volume == right->volume &&
           left->index_high == right->index_high &&
           left->index_low == right->index_low &&
           CompareFileTime(&left->write_time, &right->write_time) == 0 &&
           left->size_high == right->size_high &&
           left->size_low == right->size_low && left->links == right->links &&
           left->attributes == right->attributes;
}

static KmFileIdentity identity_from_info(const BY_HANDLE_FILE_INFORMATION *info)
{
    KmFileIdentity identity;

    identity.volume = info->dwVolumeSerialNumber;
    identity.index_high = info->nFileIndexHigh;
    identity.index_low = info->nFileIndexLow;
    identity.write_time = info->ftLastWriteTime;
    identity.size_high = info->nFileSizeHigh;
    identity.size_low = info->nFileSizeLow;
    identity.links = info->nNumberOfLinks;
    identity.attributes = info->dwFileAttributes;
    return identity;
}

static bool valid_target(const BY_HANDLE_FILE_INFORMATION *info)
{
    return (info->dwFileAttributes &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
           info->nNumberOfLinks == 1;
}

static char hex_digit(unsigned value)
{
    return (char)(value < 10 ? '0' + value : 'A' + value - 10);
}

static KmStatus path_from_wide(const WCHAR *wide, KmPath **out_path,
                               KmError *error)
{
    int display_len;
    size_t wide_len;
    char *raw;
    size_t display_offset = 0;
    KmPath *path;

    if (wide == NULL || wide[0] == L'\0' || out_path == NULL) {
        return fail(error, KM_ERR_INVALID, "file path", 0);
    }
    *out_path = NULL;
    wide_len = wcslen(wide);
    if (wide_len > (SIZE_MAX / sizeof(WCHAR)) - 1) {
        return fail(error, KM_ERR_INVALID, "file path", 0);
    }
    path = (KmPath *)calloc(1, sizeof(*path));
    if (path == NULL) return fail(error, KM_ERR_OOM, "file path", 0);
    path->wide = (WCHAR *)malloc((wide_len + 1) * sizeof(WCHAR));
    if (path->wide == NULL) {
        free(path);
        return fail(error, KM_ERR_OOM, "file path", 0);
    }
    memcpy(path->wide, wide, (wide_len + 1) * sizeof(WCHAR));
    if (wide_len > (size_t)INT_MAX) {
        km_path_destroy(path);
        return fail(error, KM_ERR_INVALID, "display file name", 0);
    }
    display_len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
                                      (int)wide_len, NULL, 0, NULL, NULL);
    if (display_len <= 0) {
        DWORD code = GetLastError();
        km_path_destroy(path);
        return fail(error, KM_ERR_INVALID, "display file name", code);
    }
    raw = (char *)malloc((size_t)display_len);
    if (raw == NULL) {
        km_path_destroy(path);
        return fail(error, KM_ERR_OOM, "display file name", 0);
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int)wide_len,
                            raw, display_len, NULL, NULL) != display_len) {
        DWORD code = GetLastError();
        free(raw);
        km_path_destroy(path);
        return fail(error, KM_ERR_INVALID, "display file name", code);
    }
    if ((size_t)display_len > (SIZE_MAX - 1) / 4) {
        free(raw);
        km_path_destroy(path);
        return fail(error, KM_ERR_INVALID, "display file name", 0);
    }
    path->display = (char *)malloc((size_t)display_len * 4 + 1);
    if (path->display == NULL) {
        free(raw);
        km_path_destroy(path);
        return fail(error, KM_ERR_OOM, "display file name", 0);
    }
    for (int i = 0; i < display_len; ++i) {
        unsigned byte = (unsigned)(unsigned char)raw[i];
        if (byte < 0x20 || byte == 0x7f) {
            path->display[display_offset++] = '\\';
            path->display[display_offset++] = 'x';
            path->display[display_offset++] = hex_digit(byte >> 4);
            path->display[display_offset++] = hex_digit(byte & 0x0f);
        } else {
            path->display[display_offset++] = raw[i];
        }
    }
    free(raw);
    path->display[display_offset] = '\0';
    *out_path = path;
    return KM_OK;
}

KmStatus km_path_from_command_line(int argc, char **argv, KmPath **out_path,
                                   KmError *error)
{
    LPWSTR *arguments;
    int count;
    KmStatus status;

    (void)argv;
    km_error_clear(error);
    if (out_path == NULL || argc < 1 || argc > 2) {
        return fail(error, KM_ERR_INVALID, "command line", 0);
    }
    *out_path = NULL;
    if (argc == 1) return KM_OK;
    arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == NULL) {
        return fail(error, KM_ERR_IO, "read command line", GetLastError());
    }
    if (count != 2) {
        LocalFree(arguments);
        return fail(error, KM_ERR_INVALID, "command line", 0);
    }
    status = path_from_wide(arguments[1], out_path, error);
    LocalFree(arguments);
    return status;
}

KmStatus km_path_from_utf8(const char *path, KmPath **out_path, KmError *error)
{
    int wide_len;
    WCHAR *wide;
    KmStatus status;
    size_t len;

    km_error_clear(error);
    if (path == NULL || out_path == NULL) {
        return fail(error, KM_ERR_INVALID, "file path", 0);
    }
    len = strlen(path);
    if (len > (size_t)INT_MAX) {
        return fail(error, KM_ERR_INVALID, "file path", 0);
    }
    wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                                   (int)len, NULL, 0);
    if (wide_len <= 0) {
        return fail(error, KM_ERR_INVALID, "file path", GetLastError());
    }
    wide = (WCHAR *)malloc(((size_t)wide_len + 1) * sizeof(WCHAR));
    if (wide == NULL) return fail(error, KM_ERR_OOM, "file path", 0);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, (int)len,
                            wide, wide_len) != wide_len) {
        DWORD code = GetLastError();
        free(wide);
        return fail(error, KM_ERR_INVALID, "file path", code);
    }
    wide[wide_len] = L'\0';
    status = path_from_wide(wide, out_path, error);
    free(wide);
    return status;
}

void km_path_destroy(KmPath *path)
{
    if (path == NULL) return;
    free(path->display);
    free(path->wide);
    free(path);
}

static KmStatus read_all(HANDLE handle, size_t expected, uint8_t **out,
                         size_t *out_len, KmError *error)
{
    uint8_t *bytes = expected == 0 ? NULL : (uint8_t *)malloc(expected);
    size_t total = 0;
    uint8_t extra;
    DWORD count;

    if (expected != 0 && bytes == NULL) {
        return fail(error, KM_ERR_OOM, "read file", 0);
    }
    while (total < expected) {
        DWORD request = expected - total > MAXDWORD
                            ? MAXDWORD
                            : (DWORD)(expected - total);
        if (!ReadFile(handle, bytes + total, request, &count, NULL)) {
            DWORD code = GetLastError();
            free(bytes);
            return fail(error, KM_ERR_IO, "read file", code);
        }
        if (count == 0) break;
        total += count;
    }
    if (!ReadFile(handle, &extra, 1, &count, NULL)) {
        DWORD code = GetLastError();
        free(bytes);
        return fail(error, KM_ERR_IO, "read file", code);
    }
    if (total != expected || count != 0) {
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
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION before;
    BY_HANDLE_FILE_INFORMATION after;
    ULARGE_INTEGER size;
    uint8_t *bytes = NULL;
    size_t len = 0;
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
    {
        DWORD path_attributes = GetFileAttributesW(path->wide);
        if (path_attributes != INVALID_FILE_ATTRIBUTES &&
            (path_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            status = fail(error, KM_ERR_UNSUPPORTED, "unsafe file target", 0);
            goto failed;
        }
    }
    handle = CreateFileW(path->wide, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            *out_file = file;
            return KM_OK;
        }
        status = fail(error, KM_ERR_IO, "open file", code);
        goto failed;
    }
    if (!GetFileInformationByHandle(handle, &before) || !valid_target(&before)) {
        DWORD code = GetLastError();
        status = fail(error, KM_ERR_UNSUPPORTED, "unsafe file target", code);
        goto failed;
    }
    size.HighPart = before.nFileSizeHigh;
    size.LowPart = before.nFileSizeLow;
    if (size.QuadPart > (ULONGLONG)PTRDIFF_MAX) {
        status = fail(error, KM_ERR_INVALID, "file too large", 0);
        goto failed;
    }
    status = read_all(handle, (size_t)size.QuadPart, &bytes, &len, error);
    if (status != KM_OK) goto failed;
    if (!GetFileInformationByHandle(handle, &after) ||
        !same_identity(&(KmFileIdentity){before.dwVolumeSerialNumber,
                                        before.nFileIndexHigh,
                                        before.nFileIndexLow,
                                        before.ftLastWriteTime,
                                        before.nFileSizeHigh,
                                        before.nFileSizeLow,
                                        before.nNumberOfLinks,
                                        before.dwFileAttributes},
                       &(KmFileIdentity){after.dwVolumeSerialNumber,
                                        after.nFileIndexHigh,
                                        after.nFileIndexLow,
                                        after.ftLastWriteTime,
                                        after.nFileSizeHigh,
                                        after.nFileSizeLow,
                                        after.nNumberOfLinks,
                                        after.dwFileAttributes})) {
        status = fail(error, KM_ERR_CONFLICT, "file changed while reading",
                      GetLastError());
        goto failed;
    }
    if (!CloseHandle(handle)) {
        handle = INVALID_HANDLE_VALUE;
        status = fail(error, KM_ERR_IO, "close file", GetLastError());
        goto failed;
    }
    handle = INVALID_HANDLE_VALUE;
    file->identity = identity_from_info(&after);
    file->attributes = after.dwFileAttributes;
    file->exists = true;
    status = normalize_text(bytes, len, file, out_text, out_len, error);
    if (status != KM_OK) goto failed;
    free(bytes);
    *out_file = file;
    return KM_OK;

failed:
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
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

static KmStatus write_all(HANDLE handle, const uint8_t *bytes, size_t len,
                          KmError *error)
{
    size_t total = 0;

    while (total < len) {
        DWORD request = len - total > MAXDWORD ? MAXDWORD : (DWORD)(len - total);
        DWORD written;
        if (!WriteFile(handle, bytes + total, request, &written, NULL)) {
            return fail(error, KM_ERR_IO, "write file", GetLastError());
        }
        if (written == 0) return fail(error, KM_ERR_IO, "write file", 0);
        total += written;
    }
    return KM_OK;
}

static KmStatus write_document(HANDLE handle, const KmFile *file,
                               const KmDocument *document, KmError *error)
{
    static const uint8_t bom[] = {0xef, 0xbb, 0xbf};
    uint8_t input[4096];
    uint8_t output[8192];
    KmTextIter iterator;
    bool eof = false;
    KmStatus status;

    if (file->bom) {
        status = write_all(handle, bom, sizeof(bom), error);
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
        status = write_all(handle, output, output_len, error);
        if (status != KM_OK) return status;
    }
    return KM_OK;
}

static WCHAR *temporary_name(const WCHAR *path)
{
    const WCHAR *backslash = wcsrchr(path, L'\\');
    const WCHAR *slash = wcsrchr(path, L'/');
    const WCHAR *separator = backslash == NULL ||
                                     (slash != NULL && slash > backslash)
                                 ? slash
                                 : backslash;
    size_t prefix_len = separator == NULL ? 0 : (size_t)(separator - path) + 1;
    WCHAR *temporary;
    LONG count = InterlockedIncrement(&temporary_counter);
    int suffix_len;

    if (prefix_len > SIZE_MAX / sizeof(WCHAR) - 48) return NULL;
    temporary = (WCHAR *)malloc((prefix_len + 48) * sizeof(WCHAR));
    if (temporary == NULL) return NULL;
    if (prefix_len != 0) {
        memcpy(temporary, path, prefix_len * sizeof(WCHAR));
    }
    suffix_len = swprintf(temporary + prefix_len, 48, L".km.%lu.%ld.tmp",
                          (unsigned long)GetCurrentProcessId(), (long)count);
    if (suffix_len < 0) {
        free(temporary);
        return NULL;
    }
    return temporary;
}

static KmStatus current_identity(const WCHAR *path, KmFileIdentity *identity,
                                 bool *exists, KmError *error)
{
    DWORD path_attributes = GetFileAttributesW(path);
    HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;

    *exists = false;
    if (path_attributes != INVALID_FILE_ATTRIBUTES &&
        (path_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return fail(error, KM_ERR_UNSUPPORTED, "unsafe file target", 0);
    }
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return KM_OK;
        }
        return fail(error, KM_ERR_IO, "inspect file", code);
    }
    if (!GetFileInformationByHandle(handle, &info)) {
        DWORD code = GetLastError();
        CloseHandle(handle);
        return fail(error, KM_ERR_IO, "inspect file", code);
    }
    CloseHandle(handle);
    if (!valid_target(&info)) {
        return fail(error, KM_ERR_UNSUPPORTED, "unsafe file target", 0);
    }
    *identity = identity_from_info(&info);
    *exists = true;
    return KM_OK;
}

KmStatus km_file_save(KmFile *file, const KmDocument *document,
                      KmError *error)
{
    KmFileIdentity current;
    bool exists;
    WCHAR *temporary = NULL;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool replaced = false;
    KmStatus status;

    km_error_clear(error);
    if (file == NULL || document == NULL) {
        return fail(error, KM_ERR_INVALID, "save file", 0);
    }
    status = current_identity(file->path->wide, &current, &exists, error);
    if (status != KM_OK) return status;
    if (exists != file->exists ||
        (exists && !same_identity(&current, &file->identity))) {
        return fail(error, KM_ERR_CONFLICT, "file changed on disk", 0);
    }
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        free(temporary);
        temporary = temporary_name(file->path->wide);
        if (temporary == NULL) return fail(error, KM_ERR_OOM, "save file", 0);
        handle = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) {
            break;
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        status = fail(error, KM_ERR_IO, "create temporary file", GetLastError());
        goto done;
    }
    status = write_document(handle, file, document, error);
    if (status != KM_OK) goto done;
    if (!FlushFileBuffers(handle)) {
        status = fail(error, KM_ERR_IO, "flush temporary file", GetLastError());
        goto done;
    }
    if (!CloseHandle(handle)) {
        handle = INVALID_HANDLE_VALUE;
        status = fail(error, KM_ERR_IO, "close temporary file", GetLastError());
        goto done;
    }
    handle = INVALID_HANDLE_VALUE;
    if (file->exists) {
        if (!ReplaceFileW(file->path->wide, temporary, NULL,
                          REPLACEFILE_WRITE_THROUGH, NULL, NULL)) {
            status = fail(error, KM_ERR_IO, "replace file", GetLastError());
            goto done;
        }
    } else if (!MoveFileExW(temporary, file->path->wide,
                            MOVEFILE_WRITE_THROUGH)) {
        status = fail(error, KM_ERR_IO, "replace file", GetLastError());
        goto done;
    }
    replaced = true;
    if (file->exists) {
        DWORD preserved = file->attributes &
                          (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                           FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
                           FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
        if (preserved == 0) preserved = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileAttributesW(file->path->wide, preserved)) {
            status = fail(error, KM_ERR_IO, "set saved file attributes",
                          GetLastError());
            goto done;
        }
    }
    status = current_identity(file->path->wide, &file->identity,
                              &file->exists, error);
    if (status != KM_OK || !file->exists) {
        if (status == KM_OK) {
            status = fail(error, KM_ERR_IO, "inspect saved file", 0);
        }
        goto done;
    }
    file->attributes = file->identity.attributes;
    status = KM_OK;

done:
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    if (!replaced && temporary != NULL) {
        (void)SetFileAttributesW(temporary, FILE_ATTRIBUTE_NORMAL);
        (void)DeleteFileW(temporary);
    }
    free(temporary);
    return status;
}
