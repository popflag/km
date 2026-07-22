#include "layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

int main(void)
{
    static const uint8_t line[] = {
        'a', 'b', 'c', '\t', 0xe4, 0xb8, 0xad,
        ' ', 'e', 0xcc, 0x81, ' ', 'x', 'y', 'z', '\n',
    };
    const size_t target = 8u * 1024u * 1024u;
    size_t lines = target / sizeof(line);
    size_t len = lines * sizeof(line);
    uint8_t *text = (uint8_t *)malloc(len);
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    KmCellGrid *grid = NULL;
    KmLayoutResult result;
    KmError error;
    size_t scroll = 0;
    clock_t start;
    clock_t end;
    volatile size_t checksum = 0;
    const unsigned iterations = 5;

    CHECK(text != NULL);
    for (size_t i = 0; i < lines; ++i) {
        memcpy(text + i * sizeof(line), line, sizeof(line));
    }
    CHECK(km_buffer_create_base(text, len, &buffer, &error) == KM_OK);
    free(text);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){(lines / 2) * sizeof(line)},
                            &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 24, 80, scroll, NULL, NULL, false,
                         &grid, &result, &error) == KM_OK);
    scroll = result.scroll_row;
    km_cell_grid_destroy(grid);

    start = clock();
    CHECK(start != (clock_t)-1);
    for (unsigned i = 0; i < iterations; ++i) {
        CHECK(km_layout_view(buffer, view, 24, 80, scroll, NULL, NULL, false,
                             &grid, &result, &error) == KM_OK);
        checksum += result.cursor_row + result.visual_rows;
        km_cell_grid_destroy(grid);
    }
    end = clock();
    CHECK(end != (clock_t)-1);
    printf("layout benchmark: %.2f ms/frame, %.2f MiB, checksum=%zu\n",
           1000.0 * (double)(end - start) /
               ((double)CLOCKS_PER_SEC * (double)iterations),
           (double)len / (1024.0 * 1024.0), (size_t)checksum);

    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
    return 0;
}
