#ifndef KM_EVENT_H
#define KM_EVENT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    KM_EVENT_KEY,
    KM_EVENT_TEXT,
    KM_EVENT_PASTE,
    KM_EVENT_MOUSE,
    KM_EVENT_RESIZE,
    KM_EVENT_FOCUS,
    KM_EVENT_EOF
} KmEventKind;

enum {
    KM_MOD_CTRL = 1u << 0,
    KM_MOD_ALT = 1u << 1,
    KM_MOD_SHIFT = 1u << 2
};

enum {
    KM_KEY_ESCAPE = 0x110000u,
    KM_KEY_LEFT,
    KM_KEY_RIGHT,
    KM_KEY_UP,
    KM_KEY_DOWN,
    KM_KEY_HOME,
    KM_KEY_END,
    KM_KEY_DELETE
};

typedef struct {
    KmEventKind kind;
    uint32_t codepoint;
    uint32_t modifiers;
    uint32_t repeat;
    /* TEXT may use one scalar above or synchronous UTF-8 bytes here. */
    const uint8_t *text;
    size_t text_len;
    unsigned columns;
    unsigned rows;
} KmEvent;

#endif
