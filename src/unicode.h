#ifndef KM_UNICODE_H
#define KM_UNICODE_H

#include "base.h"

#include <stdbool.h>

typedef struct {
    size_t end;
    uint8_t width;
    bool combining_only;
} KmGrapheme;

KmStatus km_unicode_decode(const uint8_t *text, size_t len, size_t offset,
                           int32_t *codepoint, size_t *consumed,
                           KmError *error);
int km_unicode_cell_width(int32_t codepoint);
KmStatus km_unicode_next_grapheme(const uint8_t *text, size_t len,
                                  size_t start, KmGrapheme *out,
                                  KmError *error);
KmStatus km_unicode_line_column_at(const uint8_t *text, size_t len,
                                   size_t start, size_t point,
                                   size_t *out_column, KmError *error);
KmStatus km_unicode_line_position_at_column(
    const uint8_t *text, size_t len, size_t start, size_t end, size_t target,
    size_t *out_position, size_t *out_column, KmError *error);

#endif
