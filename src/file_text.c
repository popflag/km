#include "file_text.h"

#include "utf8proc.h"

#include <stdlib.h>
#include <string.h>

static KmStatus fail(KmError *error, KmStatus status, const char *operation)
{
    if (error != NULL) {
        error->code = status;
        error->os_code = 0;
        error->operation = operation;
    }
    return status;
}

static bool valid_utf8(const uint8_t *bytes, size_t len)
{
    size_t offset = 0;

    while (offset < len) {
        utf8proc_int32_t codepoint;
        utf8proc_ssize_t count;

        if (len - offset > (size_t)PTRDIFF_MAX) return false;
        count = utf8proc_iterate(bytes + offset,
                                 (utf8proc_ssize_t)(len - offset),
                                 &codepoint);
        if (count <= 0) return false;
        offset += (size_t)count;
    }
    return true;
}

KmStatus km_file_text_decode(const uint8_t *bytes, size_t len,
                             KmFileTextFormat *out_format,
                             uint8_t **out_text, size_t *out_len,
                             KmError *error)
{
    KmFileTextFormat format = {KM_EOL_LF, false};
    size_t offset = 0;
    size_t output = 0;
    size_t lf = 0;
    size_t crlf = 0;
    size_t cr = 0;
    uint8_t *text;

    km_error_clear(error);
    if (out_format == NULL || out_text == NULL || out_len == NULL ||
        (bytes == NULL && len != 0)) {
        return fail(error, KM_ERR_INVALID, "normalize file");
    }
    *out_text = NULL;
    *out_len = 0;
    format.bom = len >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb &&
                 bytes[2] == 0xbf;
    if (format.bom) offset = 3;
    if (!valid_utf8(bytes == NULL ? NULL : bytes + offset, len - offset)) {
        return fail(error, KM_ERR_INVALID, "file UTF-8");
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
        return fail(error, KM_ERR_INVALID, "mixed file line endings");
    }
    format.eol = crlf != 0 ? KM_EOL_CRLF : cr != 0 ? KM_EOL_CR : KM_EOL_LF;
    text = len == offset ? NULL : (uint8_t *)malloc(len - offset);
    if (len != offset && text == NULL) {
        return fail(error, KM_ERR_OOM, "normalize file");
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
    *out_format = format;
    *out_text = text;
    *out_len = output;
    return KM_OK;
}

KmStatus km_file_text_write_document(const KmFileTextFormat *format,
                                     const KmDocument *document,
                                     void *context, KmFileTextWrite write,
                                     KmError *error)
{
    static const uint8_t bom[] = {0xef, 0xbb, 0xbf};
    uint8_t input[4096];
    uint8_t output[8192];
    KmTextIter iterator;
    bool eof = false;
    KmStatus status;

    km_error_clear(error);
    if (format == NULL || document == NULL || write == NULL ||
        (format->eol != KM_EOL_LF && format->eol != KM_EOL_CRLF &&
         format->eol != KM_EOL_CR)) {
        return fail(error, KM_ERR_INVALID, "encode file");
    }
    if (format->bom) {
        status = write(context, bom, sizeof(bom), error);
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
            if (input[i] == '\n' && format->eol == KM_EOL_CRLF) {
                output[output_len++] = '\r';
                output[output_len++] = '\n';
            } else if (input[i] == '\n' && format->eol == KM_EOL_CR) {
                output[output_len++] = '\r';
            } else {
                output[output_len++] = input[i];
            }
        }
        status = write(context, output, output_len, error);
        if (status != KM_OK) return status;
    }
    return KM_OK;
}
