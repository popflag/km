#include "layout.h"
#include "file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static void check_glyph(const KmCellGrid *grid, size_t row, size_t column,
                        const uint8_t *expected, size_t expected_len)
{
    size_t len;
    const uint8_t *glyph = km_cell_grid_cell_glyph(grid, row, column, &len);

    CHECK(len == expected_len);
    CHECK(memcmp(glyph, expected, len) == 0);
}

static void test_unicode_and_controls(void)
{
    static const uint8_t text[] = {
        'A', 0, 'e', 0xcc, 0x81, 0xe4, 0xb8, 0xad, '\n', 'B',
    };
    static const uint8_t combining[] = {'e', 0xcc, 0x81};
    static const uint8_t cjk[] = {0xe4, 0xb8, 0xad};
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    KmCellGrid *grid = NULL;
    KmCommandLoop *loop = NULL;
    KmEvent event = {0};
    KmLayoutResult layout;
    KmError error;
    char *frame;
    size_t frame_len;

    CHECK(km_buffer_create_base(text, sizeof(text), &buffer, &error) == KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){5}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 4, 12, 0, NULL, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 0 && layout.cursor_column == 4);
    check_glyph(grid, 0, 0, (const uint8_t *)"A", 1);
    check_glyph(grid, 0, 1, (const uint8_t *)"^", 1);
    check_glyph(grid, 0, 2, (const uint8_t *)"@", 1);
    check_glyph(grid, 0, 3, combining, sizeof(combining));
    check_glyph(grid, 0, 4, cjk, sizeof(cjk));
    CHECK(km_cell_grid_cell(grid, 0, 4)->width == 2);
    CHECK(km_cell_grid_encode_frame_vt(grid, layout.cursor_row,
                                       layout.cursor_column, &frame,
                                       &frame_len, &error) == KM_OK);
    CHECK(memchr(frame, 0, frame_len) == NULL);
    free(frame);
    km_cell_grid_destroy(grid);

    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    event.kind = KM_EVENT_KEY;
    event.codepoint = ' ';
    event.modifiers = KM_MOD_CTRL;
    event.repeat = 1;
    CHECK(km_command_loop_dispatch(loop, view, &event, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){8}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 4, 12, 0, NULL, &grid, &layout,
                         &error) == KM_OK);
    CHECK(km_cell_grid_cell(grid, 0, 4)->style_id == KM_STYLE_REGION);
    CHECK(km_cell_grid_cell(grid, 0, 5)->style_id == KM_STYLE_REGION);
    km_cell_grid_destroy(grid);
    km_command_loop_destroy(loop);

    CHECK(km_view_set_point(view, (KmBytePos){9}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 4, 12, 0, "Quit", &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 0);
    check_glyph(grid, 1, 0, (const uint8_t *)"B", 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_wrap_and_viewport(void)
{
    static const uint8_t text[] = "abcdefghi";
    static const uint8_t wide_text[] = {
        'a', 'b', 'c', 0xe4, 0xb8, 0xad,
    };
    static const uint8_t combining[] = {'e', 0xcc, 0x81};
    static const uint8_t isolated_combining[] = {0xcc, 0x81};
    static const uint8_t displayed_combining[] = {
        0xe2, 0x97, 0x8c, 0xcc, 0x81,
    };
    static const uint8_t regional_indicator[] = {0xf0, 0x9f, 0x87, 0xa6};
    static const uint8_t flag_pair[] = {
        0xf0, 0x9f, 0x87, 0xa6, 0xf0, 0x9f, 0x87, 0xa7,
    };
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    KmCellGrid *grid = NULL;
    KmLayoutResult layout;
    KmError error;

    CHECK(km_buffer_create_base(text, sizeof(text) - 1, &buffer, &error) == KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){sizeof(text) - 1}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 3, 4, 0, NULL, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.scroll_row == 1);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 1);
    check_glyph(grid, 1, 0, (const uint8_t *)"i", 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(wide_text, sizeof(wide_text), &buffer, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){3}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 3, 4, 0, NULL, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 0);
    CHECK(km_cell_grid_cell(grid, 1, 0)->width == 2);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(combining, sizeof(combining), &buffer, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 4, 0, NULL, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 0 && layout.cursor_column == 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(isolated_combining, sizeof(isolated_combining),
                                &buffer, &error) == KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 4, 0, NULL, &grid, &layout,
                         &error) == KM_OK);
    check_glyph(grid, 0, 0, displayed_combining, sizeof(displayed_combining));
    CHECK(km_cell_grid_cell(grid, 0, 0)->width == 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(regional_indicator, sizeof(regional_indicator),
                                &buffer, &error) == KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 4, 0, NULL, &grid, &layout,
                         &error) == KM_OK);
    CHECK(km_cell_grid_cell(grid, 0, 0)->width == 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(flag_pair, sizeof(flag_pair), &buffer, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 4, 0, NULL, &grid, &layout,
                         &error) == KM_OK);
    CHECK(km_cell_grid_cell(grid, 0, 0)->width == 2);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_unicode_file_name_status(void)
{
    static const char name[] = "km-status-\xe4\xb8\xad-7f31c9.tmp";
    static const uint8_t cjk[] = {0xe4, 0xb8, 0xad};
    KmPath *path = NULL;
    KmFile *file = NULL;
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    KmCellGrid *grid = NULL;
    KmLayoutResult layout;
    uint8_t *text = NULL;
    size_t len = 0;
    KmError error;

    CHECK(km_path_from_utf8(name, &path, &error) == KM_OK);
    CHECK(km_file_load(path, &file, &text, &len, &error) == KM_OK);
    CHECK(len == 0);
    CHECK(km_buffer_create_file(file, text, len, &buffer, &error) == KM_OK);
    free(text);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 40, 0, NULL, &grid, &layout,
                         &error) == KM_OK);
    check_glyph(grid, 1, 13, cjk, sizeof(cjk));
    CHECK(km_cell_grid_cell(grid, 1, 13)->width == 2);
    CHECK(km_cell_grid_cell(grid, 1, 14)->flags == KM_CELL_CONTINUATION);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

int main(void)
{
    test_unicode_and_controls();
    test_wrap_and_viewport();
    test_unicode_file_name_status();
    puts("layout tests passed");
    return 0;
}
