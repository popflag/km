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

    km_error_clear(error);
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

static size_t previous_codepoint_start(const uint8_t *text, size_t offset)
{
    --offset;
    while (offset != 0 && (text[offset] & 0xc0u) == 0x80u) --offset;
    return offset;
}

static bool boundary_needs_context(int32_t previous, int32_t current)
{
    const utf8proc_property_t *left = utf8proc_get_property(previous);
    const utf8proc_property_t *right = utf8proc_get_property(current);

    return !utf8proc_grapheme_break(previous, current) ||
           (left->boundclass == UTF8PROC_BOUNDCLASS_ZWJ &&
            right->boundclass == UTF8PROC_BOUNDCLASS_EXTENDED_PICTOGRAPHIC) ||
           right->indic_conjunct_break ==
               UTF8PROC_INDIC_CONJUNCT_BREAK_CONSONANT;
}

KmStatus km_unicode_next_grapheme(const uint8_t *text, size_t len,
                                  size_t start, KmGrapheme *out,
                                  KmError *error)
{
    int32_t previous;
    int32_t state = 0;
    size_t consumed;
    size_t cluster_start = start;
    size_t end;
    size_t scan;
    int width = 0;
    bool force_wide = false;
    bool force_narrow = false;
    size_t regional_indicators = 0;
    KmStatus status;

    km_error_clear(error);
    if (out == NULL) return fail(error, KM_ERR_INVALID, "scan grapheme");
    status = km_unicode_decode(text, len, start, &previous, &consumed, error);
    if (status != KM_OK) return status;
    end = start + consumed;
    if (start != 0) {
        size_t previous_start = previous_codepoint_start(text, start);
        int32_t before;
        size_t before_len;

        status = km_unicode_decode(text, len, previous_start, &before,
                                   &before_len, error);
        if (status != KM_OK) return status;
        if (previous_start + before_len != start) {
            return fail(error, KM_ERR_INVALID, "scan grapheme");
        }
        if (boundary_needs_context(before, previous)) {
            size_t offset;

            /* ponytail: replay is O(n) only for context-sensitive starts;
               add an iterator state if long RI runs become measurable. */
            status = km_unicode_decode(text, len, 0, &before, &before_len,
                                       error);
            if (status != KM_OK) return status;
            cluster_start = 0;
            state = 0;
            for (offset = before_len; offset <= start;) {
                int32_t next;
                bool boundary;

                status = km_unicode_decode(text, len, offset, &next,
                                           &consumed, error);
                if (status != KM_OK) return status;
                boundary = utf8proc_grapheme_break_stateful(
                    before, next, &state);
                if (boundary) {
                    cluster_start = offset;
                    state = 0;
                }
                before = next;
                offset += consumed;
            }
        }
    }
    while (end < len) {
        int32_t next;
        status = km_unicode_decode(text, len, end, &next, &consumed, error);
        if (status != KM_OK) return status;
        if (utf8proc_grapheme_break_stateful(previous, next, &state)) break;
        previous = next;
        end += consumed;
    }
    for (scan = cluster_start; scan < end; scan += consumed) {
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
