#ifndef KM_INPUT_VT_H
#define KM_INPUT_VT_H

#include "base.h"
#include "event.h"

#include <stdbool.h>

typedef struct KmInputVt KmInputVt;

KmStatus km_input_vt_create(KmInputVt **out_input, KmError *error);
void km_input_vt_destroy(KmInputVt *input);

KmStatus km_input_vt_feed(KmInputVt *input, const uint8_t *bytes, size_t len,
                          bool eof, size_t *out_consumed, KmEvent *event,
                          bool *out_event, KmError *error);
KmStatus km_input_vt_timeout(KmInputVt *input, KmEvent *event,
                             bool *out_event, KmError *error);
bool km_input_vt_wants_timeout(const KmInputVt *input);
bool km_input_vt_in_paste(const KmInputVt *input);

#endif
