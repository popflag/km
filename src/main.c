#include "platform.h"

#include <stdio.h>

static int is_quit(const KmEvent *event)
{
    if (event->kind != KM_EVENT_KEY) {
        return 0;
    }
    return (event->modifiers == 0 && event->codepoint == 'q') ||
           ((event->modifiers & KM_MOD_CTRL) != 0 &&
            (event->codepoint == 'g' || event->codepoint == 'G'));
}

int main(void)
{
    char error[256];
    KmPlatform *platform = NULL;
    KmEvent event;
    int result = 1;

    if (km_platform_open(&platform, error, sizeof(error)) != 0) {
        fprintf(stderr, "km: %s\n", error);
        return 1;
    }
    if (km_platform_draw_probe(platform, error, sizeof(error)) != 0) {
        goto done;
    }
    if (!km_platform_is_interactive(platform)) {
        result = 0;
        goto done;
    }

    for (;;) {
        if (km_platform_read_event(platform, &event, error, sizeof(error)) != 0) {
            goto done;
        }
        if (event.kind == KM_EVENT_RESIZE) {
            if (km_platform_draw_probe(platform, error, sizeof(error)) != 0) {
                goto done;
            }
        } else if (event.kind == KM_EVENT_EOF || is_quit(&event)) {
            break;
        }
    }
    result = 0;

done:
    km_platform_close(platform);
    if (result != 0) {
        fprintf(stderr, "km: %s\n", error);
    }
    return result;
}
