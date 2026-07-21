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
KmStatus km_unicode_next_grapheme(const uint8_t *text, size_t len,
                                  size_t start, KmGrapheme *out,
                                  KmError *error);

#endif
