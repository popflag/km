#ifndef KM_CONFIGURATION_H
#define KM_CONFIGURATION_H

#include "base.h"

#include <stdbool.h>

KmStatus km_configuration_validate(KmError *error);

size_t km_config_tab_width(void);
size_t km_config_sentence_end_spaces(void);
unsigned km_config_ambiguous_width(void);
size_t km_config_scroll_context_rows(void);
const char *km_config_scratch_buffer_name(void);
size_t km_config_mark_ring_capacity(void);
size_t km_config_kill_ring_capacity(void);
const char *km_config_line_number_separator(void);
size_t km_config_line_number_padding(void);
const char *km_config_style_default(void);
const char *km_config_style_region(void);
const char *km_config_style_line_number(void);
const char *km_config_style_mode_line(void);
size_t km_config_max_paste_bytes(void);
int km_config_escape_timeout_ms(void);
unsigned km_config_fallback_columns(void);
unsigned km_config_fallback_rows(void);
bool km_config_global_display_line_numbers_mode(void);
void km_config_set_global_display_line_numbers_mode(bool enabled);
bool km_config_mode_line_format(void);

#endif
