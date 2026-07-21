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
    CHECK(km_layout_view(buffer, view, 4, 12, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 0 && layout.cursor_column == 7);
    check_glyph(grid, 0, 1, (const uint8_t *)"1", 1);
    check_glyph(grid, 0, 3, (const uint8_t *)"A", 1);
    check_glyph(grid, 0, 4, (const uint8_t *)"^", 1);
    check_glyph(grid, 0, 5, (const uint8_t *)"@", 1);
    check_glyph(grid, 0, 6, combining, sizeof(combining));
    check_glyph(grid, 0, 7, cjk, sizeof(cjk));
    CHECK(km_cell_grid_cell(grid, 0, 7)->width == 2);
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
    CHECK(km_layout_view(buffer, view, 4, 12, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(km_cell_grid_cell(grid, 0, 7)->style_id == KM_STYLE_REGION);
    CHECK(km_cell_grid_cell(grid, 0, 8)->style_id == KM_STYLE_REGION);
    km_cell_grid_destroy(grid);
    km_command_loop_destroy(loop);

    CHECK(km_view_set_point(view, (KmBytePos){9}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 4, 12, 0, "Quit", false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 3);
    check_glyph(grid, 1, 3, (const uint8_t *)"B", 1);
    CHECK(km_cell_grid_cell(grid, 2, 11)->style_id == KM_STYLE_MODELINE);
    check_glyph(grid, 3, 0, (const uint8_t *)"Q", 1);
    km_cell_grid_destroy(grid);
    CHECK(km_layout_view(buffer, view, 4, 12, 0,
                         "Find: \xe4\xb8\xad", true, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 3 && layout.cursor_column == 8);
    CHECK(km_cell_grid_cell(grid, 2, 11)->style_id == KM_STYLE_MODELINE);
    check_glyph(grid, 3, 0, (const uint8_t *)"F", 1);
    check_glyph(grid, 3, 6, cjk, sizeof(cjk));
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
    CHECK(km_layout_view(buffer, view, 4, 4, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.scroll_row == 8);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 3);
    check_glyph(grid, 0, 3, (const uint8_t *)"i", 1);
    check_glyph(grid, 0, 0, (const uint8_t *)" ", 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(wide_text, sizeof(wide_text), &buffer, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){3}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 4, 7, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 3);
    CHECK(km_cell_grid_cell(grid, 1, 3)->width == 2);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(combining, sizeof(combining), &buffer, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 8, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 0 && layout.cursor_column == 4);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(isolated_combining, sizeof(isolated_combining),
                                &buffer, &error) == KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 4, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    check_glyph(grid, 0, 3, displayed_combining, sizeof(displayed_combining));
    CHECK(km_cell_grid_cell(grid, 0, 3)->width == 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(regional_indicator, sizeof(regional_indicator),
                                &buffer, &error) == KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 8, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(km_cell_grid_cell(grid, 0, 3)->width == 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(flag_pair, sizeof(flag_pair), &buffer, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 8, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(km_cell_grid_cell(grid, 0, 3)->width == 2);
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
    CHECK(km_layout_view(buffer, view, 2, 40, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    check_glyph(grid, 1, 13, cjk, sizeof(cjk));
    CHECK(km_cell_grid_cell(grid, 1, 13)->width == 2);
    CHECK(km_cell_grid_cell(grid, 1, 14)->flags == KM_CELL_CONTINUATION);
    CHECK(km_cell_grid_cell(grid, 1, 13)->style_id == KM_STYLE_MODELINE);
    CHECK(km_cell_grid_cell(grid, 1, 39)->style_id == KM_STYLE_MODELINE);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_line_number_gutter(void)
{
    static const uint8_t lines[] = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\nx\ny";
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    KmCellGrid *grid = NULL;
    KmLayoutResult layout;
    KmError error;

    CHECK(km_buffer_create_base(lines, sizeof(lines) - 1, &buffer, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){20}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 4, 8, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.scroll_row == 9);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 4);
    check_glyph(grid, 1, 1, (const uint8_t *)"1", 1);
    check_glyph(grid, 1, 2, (const uint8_t *)"1", 1);
    check_glyph(grid, 1, 4, (const uint8_t *)"x", 1);
    km_cell_grid_destroy(grid);
    CHECK(km_buffer_narrow(buffer, (KmBytePos){18},
                           (KmBytePos){sizeof(lines) - 1}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 5, 8, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    check_glyph(grid, 0, 1, (const uint8_t *)"1", 1);
    check_glyph(grid, 0, 2, (const uint8_t *)"0", 1);
    CHECK(layout.hard_line == 10);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base((const uint8_t *)"abcd\nx", 6, &buffer,
                                &error) == KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){5}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 4, 7, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 3);
    check_glyph(grid, 1, 1, (const uint8_t *)"2", 1);
    check_glyph(grid, 1, 3, (const uint8_t *)"x", 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base((const uint8_t *)"a", 1, &buffer, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 1, 0, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_column == 0);
    check_glyph(grid, 0, 0, (const uint8_t *)"a", 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_scroll_commands(void)
{
    static const uint8_t lines[] = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9";
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    size_t scroll = 0;
    KmError error;

    CHECK(km_buffer_create_base(lines, sizeof(lines) - 1, &buffer, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_scroll_view(buffer, view, 7, 8, scroll, true, false, 1,
                                &scroll, &error) == KM_OK);
    CHECK(scroll == 3);
    CHECK(km_view_point(view).v == 6);
    CHECK(km_layout_scroll_view(buffer, view, 7, 8, scroll, false, false, 1,
                                &scroll, &error) == KM_OK);
    CHECK(scroll == 0);
    CHECK(km_view_point(view).v == 6);
    CHECK(km_layout_scroll_view(buffer, view, 7, 8, scroll, true, true, 2,
                                &scroll, &error) == KM_OK);
    CHECK(scroll == 2);
    CHECK(km_view_point(view).v == 6);
    CHECK(km_layout_scroll_view(buffer, view, 7, 8, scroll, true, true, -1,
                                &scroll, &error) == KM_OK);
    CHECK(scroll == 1);
    CHECK(km_layout_scroll_view(buffer, view, 7, 8, scroll, false, true, 1,
                                &scroll, &error) == KM_OK);
    CHECK(scroll == 0);
    CHECK(km_layout_scroll_view(buffer, view, 7, 8, scroll, false, false, 1,
                                &scroll, &error) == KM_ERR_INVALID);
    CHECK(scroll == 0);
    CHECK(km_view_point(view).v == 6);
    CHECK(km_view_set_point(view, (KmBytePos){0}, &error) == KM_OK);
    CHECK(km_layout_scroll_view(buffer, view, 7, 8, scroll, true, false, 1,
                                &scroll, &error) == KM_OK);
    CHECK(scroll == 3);
    CHECK(km_layout_scroll_view(buffer, view, 7, 8, scroll, true, false, 1,
                                &scroll, &error) == KM_OK);
    CHECK(scroll == 5);
    CHECK(km_view_point(view).v == 10);
    CHECK(km_layout_scroll_view(buffer, view, 7, 8, scroll, true, false, 1,
                                &scroll, &error) == KM_ERR_INVALID);
    CHECK(scroll == 5);
    CHECK(km_view_set_point(view, (KmBytePos){18}, &error) == KM_OK);
    scroll = 5;
    CHECK(km_layout_scroll_view(buffer, view, 7, 8, scroll, false, false, 1,
                                &scroll, &error) == KM_OK);
    CHECK(scroll == 2);
    CHECK(km_view_point(view).v == 12);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base((const uint8_t *)"abcdef", 6, &buffer,
                                &error) == KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    scroll = 0;
    CHECK(km_layout_scroll_view(buffer, view, 6, 4, scroll, true, false, 1,
                                &scroll, &error) == KM_OK);
    CHECK(scroll == 2);
    CHECK(km_view_point(view).v == 2);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base((const uint8_t *)"\t", 1, &buffer,
                                &error) == KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    scroll = 0;
    CHECK(km_layout_scroll_view(buffer, view, 5, 4, scroll, true, false, 1,
                                &scroll, &error) == KM_OK);
    CHECK(scroll == 6);
    CHECK(km_view_point(view).v == 1);
    {
        KmCellGrid *grid = NULL;
        KmLayoutResult layout;
        CHECK(km_layout_view(buffer, view, 5, 4, scroll, NULL, false, &grid,
                             &layout, &error) == KM_OK);
        CHECK(layout.scroll_row == 6);
        CHECK(layout.cursor_row == 2);
        km_cell_grid_destroy(grid);
    }
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

int main(void)
{
    test_unicode_and_controls();
    test_wrap_and_viewport();
    test_unicode_file_name_status();
    test_line_number_gutter();
    test_scroll_commands();
    puts("layout tests passed");
    return 0;
}
