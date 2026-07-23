#ifndef KM_PLATFORM_H
#define KM_PLATFORM_H

#include "event.h"
#include "render.h"

#include <stddef.h>

typedef struct KmPlatform KmPlatform;

int km_platform_open(KmPlatform **out, char *error, size_t error_cap);
void km_platform_close(KmPlatform *platform);
int km_platform_is_interactive(const KmPlatform *platform);
void km_platform_size(const KmPlatform *platform, unsigned *columns,
                      unsigned *rows);
int km_platform_draw_grid(KmPlatform *platform, const KmCellGrid *front,
                          const KmCellGrid *grid,
                          size_t cursor_row, size_t cursor_column,
                          char *error, size_t error_cap);
int km_platform_draw_probe(KmPlatform *platform, char *error, size_t error_cap);
int km_platform_read_event(KmPlatform *platform, KmEvent *event,
                           char *error, size_t error_cap);

#endif
