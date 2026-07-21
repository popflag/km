#ifndef KM_PLATFORM_H
#define KM_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

typedef struct KmPlatform KmPlatform;

typedef enum {
    KM_EVENT_KEY,
    KM_EVENT_RESIZE,
    KM_EVENT_EOF
} KmEventKind;

enum {
    KM_MOD_CTRL = 1u << 0,
    KM_MOD_ALT = 1u << 1,
    KM_MOD_SHIFT = 1u << 2
};

typedef struct {
    KmEventKind kind;
    uint32_t codepoint;
    uint32_t modifiers;
    uint32_t repeat;
    unsigned columns;
    unsigned rows;
} KmEvent;

int km_platform_open(KmPlatform **out, char *error, size_t error_cap);
void km_platform_close(KmPlatform *platform);
int km_platform_is_interactive(const KmPlatform *platform);
int km_platform_draw_probe(KmPlatform *platform, char *error, size_t error_cap);
int km_platform_read_event(KmPlatform *platform, KmEvent *event,
                           char *error, size_t error_cap);

#endif
