#ifndef KM_FILE_TEXT_H
#define KM_FILE_TEXT_H

#include "document.h"

#include <stdbool.h>

typedef enum {
    KM_EOL_LF,
    KM_EOL_CRLF,
    KM_EOL_CR
} KmEolStyle;

typedef struct {
    KmEolStyle eol;
    bool bom;
} KmFileTextFormat;

typedef KmStatus (*KmFileTextWrite)(void *context, const uint8_t *bytes,
                                    size_t len, KmError *error);

KmStatus km_file_text_decode(const uint8_t *bytes, size_t len,
                             KmFileTextFormat *out_format,
                             uint8_t **out_text, size_t *out_len,
                             KmError *error);
KmStatus km_file_text_write_document(const KmFileTextFormat *format,
                                     const KmDocument *document,
                                     void *context, KmFileTextWrite write,
                                     KmError *error);

#endif
