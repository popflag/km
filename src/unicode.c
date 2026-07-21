#include "unicode.h"

#include "utf8proc.h"

static KmStatus fail(KmError *error, KmStatus status, const char *operation)
{
    if (error != NULL) {
        error->code = status;
        error->os_code = 0;
        error->operation = operation;
    }
    return status;
}

KmStatus km_unicode_decode(const uint8_t *text, size_t len, size_t offset,
                           int32_t *codepoint, size_t *consumed,
                           KmError *error)
{
    utf8proc_int32_t value;
    utf8proc_ssize_t count;

    if (text == NULL || codepoint == NULL || consumed == NULL || offset >= len ||
        len - offset > (size_t)PTRDIFF_MAX) {
        return fail(error, KM_ERR_INVALID, "decode UTF-8");
    }
    count = utf8proc_iterate(text + offset, (utf8proc_ssize_t)(len - offset),
                             &value);
    if (count <= 0) return fail(error, KM_ERR_INVALID, "decode UTF-8");
    *codepoint = value;
    *consumed = (size_t)count;
    return KM_OK;
}

KmStatus km_unicode_next_grapheme(const uint8_t *text, size_t len,
                                  size_t start, KmGrapheme *out,
                                  KmError *error)
{
    int32_t previous;
    int32_t state = 0;
    size_t consumed;
    size_t end;
    size_t scan;
    int width = 0;
    bool force_wide = false;
    bool force_narrow = false;
    size_t regional_indicators = 0;
    KmStatus status;

    if (out == NULL) return fail(error, KM_ERR_INVALID, "scan grapheme");
    status = km_unicode_decode(text, len, start, &previous, &consumed, error);
    if (status != KM_OK) return status;
    end = start + consumed;
    while (end < len) {
        int32_t next;
        status = km_unicode_decode(text, len, end, &next, &consumed, error);
        if (status != KM_OK) return status;
        if (utf8proc_grapheme_break_stateful(previous, next, &state)) break;
        previous = next;
        end += consumed;
    }
    for (scan = start; scan < end; scan += consumed) {
        int32_t codepoint;
        int codepoint_width;
        status = km_unicode_decode(text, end, scan, &codepoint, &consumed,
                                   error);
        if (status != KM_OK) return status;
        codepoint_width = utf8proc_charwidth(codepoint);
        if (codepoint_width > width) width = codepoint_width;
        if (codepoint == 0x200d || codepoint == 0xfe0f) force_wide = true;
        if (codepoint == 0xfe0e) force_narrow = true;
        if (codepoint >= 0x1f1e6 && codepoint <= 0x1f1ff) {
            ++regional_indicators;
        }
    }
    out->end = end;
    out->combining_only = width < 1;
    if (force_narrow) {
        width = 1;
    } else if (force_wide || regional_indicators >= 2) {
        width = 2;
    } else if (width < 1) {
        width = 1;
    } else if (width > 2) {
        width = 2;
    }
    out->width = (uint8_t)width;
    return KM_OK;
}
