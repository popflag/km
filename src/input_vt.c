#include "input_vt.h"

#include <stdlib.h>
#include <string.h>

#define KM_MAX_PASTE_BYTES (1024u * 1024u)
#define KM_VT_SEQUENCE_CAP 16u

typedef enum {
    KM_VT_GROUND,
    KM_VT_UTF8,
    KM_VT_ESCAPE,
    KM_VT_CSI,
    KM_VT_SS3,
    KM_VT_META_UTF8,
    KM_VT_CSI_DISCARD,
    KM_VT_PASTE,
    KM_VT_OSC
} KmInputVtState;

typedef enum {
    KM_PASTE_ERROR_NONE,
    KM_PASTE_ERROR_OOM,
    KM_PASTE_ERROR_LIMIT
} KmPasteError;

struct KmInputVt {
    KmInputVtState state;
    uint32_t utf8_value;
    uint32_t utf8_min;
    unsigned utf8_needed;
    uint8_t sequence[KM_VT_SEQUENCE_CAP];
    size_t sequence_len;
    uint8_t pending_byte;
    bool pending_byte_valid;
    bool eof_seen;
    bool osc_after_escape;
    uint8_t *paste;
    size_t paste_len;
    size_t paste_cap;
    size_t paste_match;
    KmPasteError paste_error;
};

static KmStatus fail(KmError *error, KmStatus status, const char *operation)
{
    if (error != NULL) {
        error->code = status;
        error->os_code = 0;
        error->operation = operation;
    }
    return status;
}

static void finish_named_key(KmEvent *event, uint32_t key)
{
    memset(event, 0, sizeof(*event));
    event->kind = KM_EVENT_KEY;
    event->codepoint = key;
    event->repeat = 1;
}

static void finish_scalar(KmEvent *event, uint32_t codepoint, bool meta)
{
    memset(event, 0, sizeof(*event));
    event->repeat = 1;
    if (codepoint == 0) {
        event->kind = KM_EVENT_KEY;
        event->codepoint = ' ';
        event->modifiers = KM_MOD_CTRL;
    } else if (codepoint == 8 || codepoint == 0x7f) {
        event->kind = KM_EVENT_KEY;
        event->codepoint = 0x7f;
    } else if (codepoint == '\t') {
        event->kind = KM_EVENT_KEY;
        event->codepoint = KM_KEY_TAB;
    } else if (codepoint == '\r' || codepoint == '\n') {
        event->kind = KM_EVENT_TEXT;
        event->codepoint = '\n';
    } else if (codepoint >= 1 && codepoint <= 26) {
        event->kind = KM_EVENT_KEY;
        event->codepoint = (uint32_t)('a' + codepoint - 1);
        event->modifiers = KM_MOD_CTRL;
    } else if (codepoint == 0x1f) {
        event->kind = KM_EVENT_KEY;
        event->codepoint = '/';
        event->modifiers = KM_MOD_CTRL;
    } else {
        event->kind = codepoint >= 0x20 ? KM_EVENT_TEXT : KM_EVENT_KEY;
        event->codepoint = codepoint;
    }
    if (meta) {
        if (event->kind == KM_EVENT_TEXT) event->kind = KM_EVENT_KEY;
        event->modifiers |= KM_MOD_ALT;
    }
}

static bool valid_scalar(uint32_t value, uint32_t minimum)
{
    return value >= minimum && value <= 0x10ffffu &&
           !(value >= 0xd800u && value <= 0xdfffu);
}

static bool start_scalar(KmInputVt *input, uint8_t byte, bool meta,
                         KmEvent *event)
{
    if (byte < 0x80) {
        if (meta) input->state = KM_VT_GROUND;
        finish_scalar(event, byte, meta);
        return true;
    }
    if (byte >= 0xc2 && byte <= 0xdf) {
        input->utf8_value = byte & 0x1fu;
        input->utf8_min = 0x80;
        input->utf8_needed = 1;
    } else if (byte >= 0xe0 && byte <= 0xef) {
        input->utf8_value = byte & 0x0fu;
        input->utf8_min = 0x800;
        input->utf8_needed = 2;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
        input->utf8_value = byte & 0x07u;
        input->utf8_min = 0x10000;
        input->utf8_needed = 3;
    } else {
        if (meta) input->state = KM_VT_GROUND;
        finish_scalar(event, 0xfffd, meta);
        return true;
    }
    input->state = meta ? KM_VT_META_UTF8 : KM_VT_UTF8;
    return false;
}

static bool continue_scalar(KmInputVt *input, uint8_t byte, KmEvent *event)
{
    bool meta = input->state == KM_VT_META_UTF8;
    uint32_t codepoint;

    if ((byte & 0xc0u) != 0x80u) {
        input->state = KM_VT_GROUND;
        input->utf8_needed = 0;
        input->pending_byte = byte;
        input->pending_byte_valid = true;
        finish_scalar(event, 0xfffd, meta);
        return true;
    }
    input->utf8_value = (input->utf8_value << 6) | (byte & 0x3fu);
    if (--input->utf8_needed != 0) return false;
    codepoint = input->utf8_value;
    input->state = KM_VT_GROUND;
    if (!valid_scalar(codepoint, input->utf8_min)) codepoint = 0xfffd;
    finish_scalar(event, codepoint, meta);
    return true;
}

static bool finish_sequence(KmInputVt *input, KmInputVtState state,
                            KmEvent *event)
{
    const uint8_t *sequence = input->sequence;
    size_t len = input->sequence_len;

    input->state = KM_VT_GROUND;
    if (len == 1) {
        switch (sequence[0]) {
        case 'A': finish_named_key(event, KM_KEY_UP); return true;
        case 'B': finish_named_key(event, KM_KEY_DOWN); return true;
        case 'C': finish_named_key(event, KM_KEY_RIGHT); return true;
        case 'D': finish_named_key(event, KM_KEY_LEFT); return true;
        case 'H': finish_named_key(event, KM_KEY_HOME); return true;
        case 'F': finish_named_key(event, KM_KEY_END); return true;
        default: break;
        }
    }
    if (state == KM_VT_CSI && len == 2 && sequence[1] == '~') {
        if (sequence[0] == '3') {
            finish_named_key(event, KM_KEY_DELETE);
            return true;
        }
        if (sequence[0] == '1' || sequence[0] == '7') {
            finish_named_key(event, KM_KEY_HOME);
            return true;
        }
        if (sequence[0] == '4' || sequence[0] == '8') {
            finish_named_key(event, KM_KEY_END);
            return true;
        }
    }
    if (state == KM_VT_CSI && len == 4 &&
        memcmp(sequence, "200~", 4) == 0) {
        input->state = KM_VT_PASTE;
        input->paste_len = 0;
        input->paste_match = 0;
        input->paste_error = KM_PASTE_ERROR_NONE;
        return false;
    }
    finish_named_key(event, KM_KEY_ESCAPE);
    return true;
}

static void append_paste_byte(KmInputVt *input, uint8_t byte)
{
    const size_t limit = KM_MAX_PASTE_BYTES + 6u;

    if (input->paste_error != KM_PASTE_ERROR_NONE) return;
    if (input->paste_len == limit) {
        input->paste_error = KM_PASTE_ERROR_LIMIT;
        return;
    }
    if (input->paste_len == input->paste_cap) {
        size_t capacity = input->paste_cap == 0 ? 256 : input->paste_cap * 2;
        uint8_t *paste;

        if (capacity > limit) capacity = limit;
        paste = (uint8_t *)realloc(input->paste, capacity);
        if (paste == NULL) {
            input->paste_error = KM_PASTE_ERROR_OOM;
            return;
        }
        input->paste = paste;
        input->paste_cap = capacity;
    }
    input->paste[input->paste_len++] = byte;
}

static KmStatus process_paste(KmInputVt *input, uint8_t byte, KmEvent *event,
                              bool *out_event, KmError *error)
{
    static const uint8_t end_marker[] = {0x1b, '[', '2', '0', '1', '~'};

    append_paste_byte(input, byte);
    input->paste_match = byte == end_marker[input->paste_match]
                             ? input->paste_match + 1
                             : byte == end_marker[0] ? 1u : 0u;
    if (input->paste_match != sizeof(end_marker)) return KM_OK;
    input->state = KM_VT_GROUND;
    if (input->paste_error == KM_PASTE_ERROR_OOM) {
        input->paste_len = 0;
        return fail(error, KM_ERR_OOM, "read paste: out of memory");
    }
    if (input->paste_error == KM_PASTE_ERROR_LIMIT ||
        input->paste_len < sizeof(end_marker) ||
        input->paste_len - sizeof(end_marker) > KM_MAX_PASTE_BYTES) {
        input->paste_len = 0;
        return fail(error, KM_ERR_INVALID,
                    "read paste: input exceeds 1 MiB");
    }
    input->paste_len -= sizeof(end_marker);
    memset(event, 0, sizeof(*event));
    event->kind = KM_EVENT_PASTE;
    event->repeat = 1;
    event->text = input->paste;
    event->text_len = input->paste_len;
    *out_event = true;
    return KM_OK;
}

static KmStatus process_byte(KmInputVt *input, uint8_t byte, KmEvent *event,
                             bool *out_event, KmError *error)
{
    KmInputVtState state = input->state;

    switch (state) {
    case KM_VT_GROUND:
        if (byte == 0x1b) {
            input->state = KM_VT_ESCAPE;
        } else {
            *out_event = start_scalar(input, byte, false, event);
        }
        break;
    case KM_VT_UTF8:
    case KM_VT_META_UTF8:
        *out_event = continue_scalar(input, byte, event);
        break;
    case KM_VT_ESCAPE:
        if (byte == '[' || byte == 'O') {
            input->state = byte == '[' ? KM_VT_CSI : KM_VT_SS3;
            input->sequence_len = 0;
        } else if (byte == ']') {
            input->state = KM_VT_OSC;
            input->osc_after_escape = false;
        } else {
            *out_event = start_scalar(input, byte, true, event);
        }
        break;
    case KM_VT_CSI:
    case KM_VT_SS3:
        input->sequence[input->sequence_len++] = byte;
        if (byte >= 0x40 && byte <= 0x7e) {
            *out_event = finish_sequence(input, state, event);
        } else if (input->sequence_len == KM_VT_SEQUENCE_CAP) {
            input->state = KM_VT_CSI_DISCARD;
            finish_named_key(event, KM_KEY_ESCAPE);
            *out_event = true;
        }
        break;
    case KM_VT_CSI_DISCARD:
        if (byte >= 0x40 && byte <= 0x7e) input->state = KM_VT_GROUND;
        break;
    case KM_VT_PASTE:
        return process_paste(input, byte, event, out_event, error);
    case KM_VT_OSC:
        if (input->osc_after_escape) {
            if (byte == '\\' || byte == 0x07) {
                input->state = KM_VT_GROUND;
                input->osc_after_escape = false;
            } else {
                input->osc_after_escape = byte == 0x1b;
            }
        } else if (byte == 0x07) {
            input->state = KM_VT_GROUND;
        } else if (byte == 0x1b) {
            input->osc_after_escape = true;
        }
        break;
    }
    return KM_OK;
}

static KmStatus finish_eof(KmInputVt *input, KmEvent *event,
                           bool *out_event, KmError *error)
{
    switch (input->state) {
    case KM_VT_UTF8:
        input->state = KM_VT_GROUND;
        input->utf8_needed = 0;
        finish_scalar(event, 0xfffd, false);
        *out_event = true;
        return KM_OK;
    case KM_VT_META_UTF8:
        input->state = KM_VT_GROUND;
        input->utf8_needed = 0;
        finish_scalar(event, 0xfffd, true);
        *out_event = true;
        return KM_OK;
    case KM_VT_ESCAPE:
    case KM_VT_CSI:
    case KM_VT_SS3:
        input->state = KM_VT_GROUND;
        finish_named_key(event, KM_KEY_ESCAPE);
        *out_event = true;
        return KM_OK;
    case KM_VT_PASTE:
        input->state = KM_VT_GROUND;
        input->paste_len = 0;
        return fail(error, KM_ERR_INVALID,
                    "read paste: incomplete delimiter");
    case KM_VT_OSC:
    case KM_VT_CSI_DISCARD:
        input->state = KM_VT_GROUND;
        break;
    case KM_VT_GROUND:
        break;
    }
    memset(event, 0, sizeof(*event));
    event->kind = KM_EVENT_EOF;
    *out_event = true;
    return KM_OK;
}

KmStatus km_input_vt_create(KmInputVt **out_input, KmError *error)
{
    KmInputVt *input;

    km_error_clear(error);
    if (out_input == NULL) {
        return fail(error, KM_ERR_INVALID, "VT input create");
    }
    *out_input = NULL;
    input = (KmInputVt *)calloc(1, sizeof(*input));
    if (input == NULL) return fail(error, KM_ERR_OOM, "VT input create");
    *out_input = input;
    return KM_OK;
}

void km_input_vt_destroy(KmInputVt *input)
{
    if (input == NULL) return;
    free(input->paste);
    free(input);
}

KmStatus km_input_vt_feed(KmInputVt *input, const uint8_t *bytes, size_t len,
                          bool eof, size_t *out_consumed, KmEvent *event,
                          bool *out_event, KmError *error)
{
    KmStatus status;

    km_error_clear(error);
    if (out_consumed != NULL) *out_consumed = 0;
    if (out_event != NULL) *out_event = false;
    if (input == NULL || out_consumed == NULL || event == NULL ||
        out_event == NULL || (bytes == NULL && len != 0) ||
        (input->eof_seen && len != 0 && !eof)) {
        return fail(error, KM_ERR_INVALID, "VT input feed");
    }
    if (eof) input->eof_seen = true;
    if (input->pending_byte_valid) {
        uint8_t pending = input->pending_byte;
        input->pending_byte_valid = false;
        status = process_byte(input, pending, event, out_event, error);
        if (status != KM_OK || *out_event) return status;
    }
    while (*out_consumed < len) {
        uint8_t byte = bytes[*out_consumed];
        ++*out_consumed;
        status = process_byte(input, byte, event, out_event, error);
        if (status != KM_OK || *out_event) return status;
    }
    if (input->eof_seen) {
        return finish_eof(input, event, out_event, error);
    }
    return KM_OK;
}

KmStatus km_input_vt_timeout(KmInputVt *input, KmEvent *event,
                             bool *out_event, KmError *error)
{
    bool meta;

    km_error_clear(error);
    if (out_event != NULL) *out_event = false;
    if (input == NULL || event == NULL || out_event == NULL) {
        return fail(error, KM_ERR_INVALID, "VT input timeout");
    }
    if (!km_input_vt_wants_timeout(input)) return KM_OK;
    meta = input->state == KM_VT_META_UTF8;
    input->state = KM_VT_GROUND;
    input->utf8_needed = 0;
    if (meta) {
        finish_scalar(event, 0xfffd, true);
    } else {
        finish_named_key(event, KM_KEY_ESCAPE);
    }
    *out_event = true;
    return KM_OK;
}

bool km_input_vt_wants_timeout(const KmInputVt *input)
{
    return input != NULL &&
           (input->state == KM_VT_ESCAPE || input->state == KM_VT_CSI ||
            input->state == KM_VT_SS3 ||
            input->state == KM_VT_META_UTF8);
}

bool km_input_vt_in_paste(const KmInputVt *input)
{
    return input != NULL && input->state == KM_VT_PASTE;
}
