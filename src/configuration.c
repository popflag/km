#include "configuration.h"

#include "utf8proc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t tab_width;
    size_t sentence_end_spaces;
    unsigned ambiguous_width;
    size_t scroll_context_rows;
    const char *scratch_buffer_name;
    size_t mark_ring_capacity;
    size_t kill_ring_capacity;
    const char *line_number_separator;
    size_t line_number_padding;
    const char *style_default;
    const char *style_region;
    const char *style_line_number;
    const char *style_mode_line;
    size_t max_paste_bytes;
    int escape_timeout_ms;
    unsigned fallback_columns;
    unsigned fallback_rows;
    bool global_display_line_numbers_mode;
    bool mode_line_format;
} KmConfiguration;

static KmConfiguration configuration = {
    8u, 2u, 1u, 2u, "*scratch*", 16u, 60u, "\xe2\x94\x82", 1u,
    "\x1b[0m", "\x1b[0;7m", "\x1b[0;2m", "\x1b[0;1;7m",
    1024u * 1024u, 50, 80u, 24u, true, true,
};
static bool loaded;
static const char *invalid_setting;
static char *owned_scratch_buffer_name;
static char *owned_line_number_separator;
static char *owned_style_default;
static char *owned_style_region;
static char *owned_style_line_number;
static char *owned_style_mode_line;

static void reject(const char *setting)
{
    if (invalid_setting == NULL) invalid_setting = setting;
}

static void set_tab_width(size_t value)
{
    if (value == 0) reject("config.h: tab_width");
    else configuration.tab_width = value;
}

static void set_sentence_end_spaces(size_t value)
{
    if (value == 0) reject("config.h: sentence_end_spaces");
    else configuration.sentence_end_spaces = value;
}

static void set_ambiguous_width(unsigned value)
{
    if (value != 1u && value != 2u) reject("config.h: ambiguous_width");
    else configuration.ambiguous_width = value;
}

static void set_scroll_context_rows(size_t value)
{
    configuration.scroll_context_rows = value;
}

static void set_string(const char **target, char **owned, const char *value,
                       const char *setting)
{
    char *copy;
    size_t len;

    if (value == NULL || value[0] == '\0') {
        reject(setting);
        return;
    }
    len = strlen(value);
    if (len == SIZE_MAX || (copy = (char *)malloc(len + 1u)) == NULL) {
        reject("config.h: string allocation");
        return;
    }
    memcpy(copy, value, len + 1u);
    free(*owned);
    *owned = copy;
    *target = copy;
}

static void set_scratch_buffer_name(const char *value)
{
    set_string(&configuration.scratch_buffer_name, &owned_scratch_buffer_name,
               value, "config.h: scratch_buffer_name");
}

static void set_mark_ring_capacity(size_t value)
{
    if (value == 0) reject("config.h: mark_ring_capacity");
    else configuration.mark_ring_capacity = value;
}

static void set_kill_ring_capacity(size_t value)
{
    if (value == 0) reject("config.h: kill_ring_capacity");
    else configuration.kill_ring_capacity = value;
}

static void set_line_number_separator(const char *value)
{
    set_string(&configuration.line_number_separator,
               &owned_line_number_separator, value,
               "config.h: line_number_separator");
}

static void set_line_number_padding(size_t value)
{
    if (value > (SIZE_MAX - 2u) / 2u) {
        reject("config.h: line_number_padding");
    } else {
        configuration.line_number_padding = value;
    }
}

#define DEFINE_STYLE_SETTER(name, field)                                  \
    static void set_##name(const char *value)                             \
    {                                                                      \
        set_string(&configuration.field, &owned_##field, value,           \
                   "config.h: " #name);                                  \
    }

DEFINE_STYLE_SETTER(style_default, style_default)
DEFINE_STYLE_SETTER(style_region, style_region)
DEFINE_STYLE_SETTER(style_line_number, style_line_number)
DEFINE_STYLE_SETTER(style_mode_line, style_mode_line)

#undef DEFINE_STYLE_SETTER

static void set_max_paste_bytes(size_t value)
{
    if (value > SIZE_MAX - 6u) reject("config.h: max_paste_bytes");
    else configuration.max_paste_bytes = value;
}

static void set_escape_timeout_ms(int value)
{
    if (value < 0) reject("config.h: escape_timeout_ms");
    else configuration.escape_timeout_ms = value;
}

static void set_fallback_columns(unsigned value)
{
    if (value == 0) reject("config.h: fallback_columns");
    else configuration.fallback_columns = value;
}

static void set_fallback_rows(unsigned value)
{
    if (value == 0) reject("config.h: fallback_rows");
    else configuration.fallback_rows = value;
}

static void set_global_display_line_numbers_mode(int enabled)
{
    configuration.global_display_line_numbers_mode = enabled > 0;
}

static void set_mode_line_format(int enabled)
{
    configuration.mode_line_format = enabled != 0;
}

static bool valid_buffer_name(const char *value)
{
    const uint8_t *bytes = (const uint8_t *)value;
    size_t len = strlen(value);
    size_t offset = 0;

    if (len == 0 || len > (size_t)PTRDIFF_MAX) return false;
    while (offset < len) {
        utf8proc_int32_t codepoint;
        utf8proc_ssize_t consumed = utf8proc_iterate(
            bytes + offset, (utf8proc_ssize_t)(len - offset), &codepoint);
        if (consumed <= 0 || codepoint < 0x20 || codepoint == 0x7f) {
            return false;
        }
        offset += (size_t)consumed;
    }
    return true;
}

static bool valid_separator(const char *value)
{
    const uint8_t *bytes = (const uint8_t *)value;
    utf8proc_int32_t codepoint;
    size_t len = strlen(value);
    utf8proc_ssize_t consumed;
    int width;

    if (len == 0 || len > (size_t)PTRDIFF_MAX) return false;
    consumed = utf8proc_iterate(bytes, (utf8proc_ssize_t)len, &codepoint);
    if (consumed <= 0 || (size_t)consumed != len) return false;
    width = utf8proc_charwidth(codepoint);
    if (configuration.ambiguous_width == 2u &&
        utf8proc_charwidth_ambiguous(codepoint)) {
        width = 2;
    }
    return width == 1;
}

static bool valid_sgr(const char *value)
{
    size_t i;

    if (value[0] != '\x1b' || value[1] != '[') return false;
    for (i = 2; value[i] != '\0'; ++i) {
        if (value[i] == 'm') return value[i + 1u] == '\0';
        if ((value[i] < '0' || value[i] > '9') && value[i] != ';' &&
            value[i] != ':') {
            return false;
        }
    }
    return false;
}

static void load(void)
{
    if (loaded) return;
    loaded = true;

#define setq(name, value) set_##name((value))
#define global_display_line_numbers_mode(enabled) \
    set_global_display_line_numbers_mode((enabled))
#define bind_key(map, code, mods, command) ((void)0)
#define bind_key2(map, code1, mods1, code2, mods2, command) ((void)0)
#include "../config.h"
#undef bind_key2
#undef bind_key
#undef global_display_line_numbers_mode
#undef setq

    if (!valid_buffer_name(configuration.scratch_buffer_name)) {
        reject("config.h: scratch_buffer_name");
    }
    if (!valid_separator(configuration.line_number_separator)) {
        reject("config.h: line_number_separator");
    }
    if (!valid_sgr(configuration.style_default)) {
        reject("config.h: style_default");
    }
    if (!valid_sgr(configuration.style_region)) {
        reject("config.h: style_region");
    }
    if (!valid_sgr(configuration.style_line_number)) {
        reject("config.h: style_line_number");
    }
    if (!valid_sgr(configuration.style_mode_line)) {
        reject("config.h: style_mode_line");
    }
}

KmStatus km_configuration_validate(KmError *error)
{
    load();
    km_error_clear(error);
    if (invalid_setting == NULL) return KM_OK;
    if (error != NULL) {
        error->code = KM_ERR_INVALID;
        error->operation = invalid_setting;
    }
    return KM_ERR_INVALID;
}

#define DEFINE_ACCESSOR(type, name, field) \
    type km_config_##name(void)             \
    {                                       \
        load();                             \
        return configuration.field;         \
    }

DEFINE_ACCESSOR(size_t, tab_width, tab_width)
DEFINE_ACCESSOR(size_t, sentence_end_spaces, sentence_end_spaces)
DEFINE_ACCESSOR(unsigned, ambiguous_width, ambiguous_width)
DEFINE_ACCESSOR(size_t, scroll_context_rows, scroll_context_rows)
DEFINE_ACCESSOR(const char *, scratch_buffer_name, scratch_buffer_name)
DEFINE_ACCESSOR(size_t, mark_ring_capacity, mark_ring_capacity)
DEFINE_ACCESSOR(size_t, kill_ring_capacity, kill_ring_capacity)
DEFINE_ACCESSOR(const char *, line_number_separator, line_number_separator)
DEFINE_ACCESSOR(size_t, line_number_padding, line_number_padding)
DEFINE_ACCESSOR(const char *, style_default, style_default)
DEFINE_ACCESSOR(const char *, style_region, style_region)
DEFINE_ACCESSOR(const char *, style_line_number, style_line_number)
DEFINE_ACCESSOR(const char *, style_mode_line, style_mode_line)
DEFINE_ACCESSOR(size_t, max_paste_bytes, max_paste_bytes)
DEFINE_ACCESSOR(int, escape_timeout_ms, escape_timeout_ms)
DEFINE_ACCESSOR(unsigned, fallback_columns, fallback_columns)
DEFINE_ACCESSOR(unsigned, fallback_rows, fallback_rows)
DEFINE_ACCESSOR(bool, global_display_line_numbers_mode,
                global_display_line_numbers_mode)
DEFINE_ACCESSOR(bool, mode_line_format, mode_line_format)

#undef DEFINE_ACCESSOR

void km_config_set_global_display_line_numbers_mode(bool enabled)
{
    load();
    configuration.global_display_line_numbers_mode = enabled;
}
