#include "layout.h"
#include "file.h"
#include "unicode.h"
#include "configuration.h"

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

static void test_configuration_contract(void)
{
    KmError error;

    CHECK(km_configuration_validate(&error) == KM_OK);
    CHECK(km_config_tab_width() >= 1u);
    CHECK(km_config_sentence_end_spaces() >= 1u);
    CHECK(km_config_ambiguous_width() == 1u ||
          km_config_ambiguous_width() == 2u);
    CHECK(km_config_mark_ring_capacity() >= 1u);
    CHECK(km_config_kill_ring_capacity() >= 1u);
    CHECK(km_config_line_number_separator()[0] != '\0');
}

static void test_grapheme_scans_from_internal_boundaries(void)
{
    static const uint8_t combining[] = {'e', 0xcc, 0x81, 'x'};
    static const uint8_t regional_indicators[] = {
        0xf0, 0x9f, 0x87, 0xa6, 0xf0, 0x9f, 0x87, 0xa7,
        0xf0, 0x9f, 0x87, 0xa8,
    };
    static const uint8_t zwj[] = {
        0xf0, 0x9f, 0x91, 0xa9, 0xe2, 0x80, 0x8d,
        0xf0, 0x9f, 0x92, 0xbb,
    };
    static const uint8_t indic[] = {
        0xe0, 0xa4, 0x95, 0xe0, 0xa5, 0x8d, 0xe0, 0xa4, 0x95,
    };
    KmGrapheme grapheme;
    KmError error = {KM_ERR_IO, 42, "stale"};
    int32_t codepoint;
    size_t consumed;

    CHECK(km_unicode_decode((const uint8_t *)"a", 1, 0, &codepoint,
                            &consumed, &error) == KM_OK);
    CHECK(km_unicode_cell_width(0x00a1) ==
          (int)km_config_ambiguous_width());
    CHECK(error.code == KM_OK && error.os_code == 0 && error.operation == NULL);
    CHECK(km_unicode_next_grapheme(combining, sizeof(combining), 1,
                                   &grapheme, &error) == KM_OK);
    CHECK(grapheme.end == 3 && grapheme.width == 1 &&
          !grapheme.combining_only);
    CHECK(km_unicode_next_grapheme(regional_indicators,
                                   sizeof(regional_indicators), 4,
                                   &grapheme, &error) == KM_OK);
    CHECK(grapheme.end == 8 && grapheme.width == 2);
    CHECK(km_unicode_next_grapheme(zwj, sizeof(zwj), 4, &grapheme,
                                   &error) == KM_OK);
    CHECK(grapheme.end == sizeof(zwj) && grapheme.width == 2);
    CHECK(km_unicode_next_grapheme(zwj, sizeof(zwj), 7, &grapheme,
                                   &error) == KM_OK);
    CHECK(grapheme.end == sizeof(zwj) && grapheme.width == 2);
    CHECK(km_unicode_next_grapheme(indic, sizeof(indic), 3, &grapheme,
                                   &error) == KM_OK);
    CHECK(grapheme.end == sizeof(indic));
    CHECK(error.code == KM_OK && error.os_code == 0 && error.operation == NULL);
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
    CHECK(km_layout_view(buffer, view, 4, 12, 0, NULL, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 0 && layout.cursor_column == 8);
    check_glyph(grid, 0, 1, (const uint8_t *)"1", 1);
    check_glyph(grid, 0, 4, (const uint8_t *)"A", 1);
    check_glyph(grid, 0, 5, (const uint8_t *)"^", 1);
    check_glyph(grid, 0, 6, (const uint8_t *)"@", 1);
    check_glyph(grid, 0, 7, combining, sizeof(combining));
    check_glyph(grid, 0, 8, cjk, sizeof(cjk));
    CHECK(km_cell_grid_cell(grid, 0, 8)->width == 2);
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
    CHECK(km_layout_view(buffer, view, 4, 12, 0, NULL, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(km_cell_grid_cell(grid, 0, 8)->style_id == KM_STYLE_REGION);
    CHECK(km_cell_grid_cell(grid, 0, 9)->style_id == KM_STYLE_REGION);
    km_cell_grid_destroy(grid);
    km_command_loop_destroy(loop);

    CHECK(km_view_set_point(view, (KmBytePos){9}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 4, 12, 0, "Quit", NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 4);
    check_glyph(grid, 1, 4, (const uint8_t *)"B", 1);
    CHECK(km_cell_grid_cell(grid, 2, 11)->style_id == KM_STYLE_MODELINE);
    check_glyph(grid, 3, 0, (const uint8_t *)"Q", 1);
    km_cell_grid_destroy(grid);
    CHECK(km_layout_view(buffer, view, 4, 12, 0,
                         "Find: \xe4\xb8\xad", " {a | b}", true, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 3 && layout.cursor_column == 8);
    CHECK(km_cell_grid_cell(grid, 2, 11)->style_id == KM_STYLE_MODELINE);
    check_glyph(grid, 3, 0, (const uint8_t *)"F", 1);
    check_glyph(grid, 3, 6, cjk, sizeof(cjk));
    check_glyph(grid, 3, 9, (const uint8_t *)"{", 1);
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
    CHECK(km_layout_view(buffer, view, 4, 4, 0, NULL, NULL, false, &grid, &layout,
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
    CHECK(km_layout_view(buffer, view, 4, 7, 0, NULL, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 4);
    CHECK(km_cell_grid_cell(grid, 1, 4)->width == 2);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(combining, sizeof(combining), &buffer, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 8, 0, NULL, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 0 && layout.cursor_column == 5);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(isolated_combining, sizeof(isolated_combining),
                                &buffer, &error) == KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 4, 0, NULL, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    check_glyph(grid, 0, 3, displayed_combining, sizeof(displayed_combining));
    CHECK(km_cell_grid_cell(grid, 0, 3)->width == 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(regional_indicator, sizeof(regional_indicator),
                                &buffer, &error) == KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 8, 0, NULL, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(km_cell_grid_cell(grid, 0, 4)->width == 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base(flag_pair, sizeof(flag_pair), &buffer, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 8, 0, NULL, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(km_cell_grid_cell(grid, 0, 4)->width == 2);
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
    CHECK(km_layout_view(buffer, view, 2, 40, 0, NULL, NULL, false, &grid, &layout,
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
    CHECK(km_layout_view(buffer, view, 4, 8, 0, NULL, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.scroll_row == 9);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 5);
    check_glyph(grid, 1, 1, (const uint8_t *)"1", 1);
    check_glyph(grid, 1, 2, (const uint8_t *)"1", 1);
    check_glyph(grid, 1, 3, (const uint8_t *)"\xe2\x94\x82", 3);
    CHECK(km_cell_grid_cell(grid, 1, 3)->style_id == KM_STYLE_LINE_NUMBER);
    check_glyph(grid, 1, 5, (const uint8_t *)"x", 1);
    km_cell_grid_destroy(grid);
    CHECK(km_buffer_narrow(buffer, (KmBytePos){18},
                           (KmBytePos){sizeof(lines) - 1}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 5, 8, 0, NULL, NULL, false, &grid, &layout,
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
    CHECK(km_layout_view(buffer, view, 4, 7, 0, NULL, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 4);
    check_glyph(grid, 1, 1, (const uint8_t *)"2", 1);
    check_glyph(grid, 1, 2, (const uint8_t *)"\xe2\x94\x82", 3);
    check_glyph(grid, 1, 4, (const uint8_t *)"x", 1);
    km_cell_grid_destroy(grid);

    km_buffer_set_line_numbers_visible(buffer, false);
    CHECK(km_layout_view(buffer, view, 4, 7, 0, NULL, NULL, false, &grid,
                         &layout, &error) == KM_OK);
    CHECK(layout.cursor_row == 1 && layout.cursor_column == 0);
    check_glyph(grid, 1, 0, (const uint8_t *)"x", 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    CHECK(km_buffer_create_base((const uint8_t *)"a", 1, &buffer, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 2, 1, 0, NULL, NULL, false, &grid, &layout,
                         &error) == KM_OK);
    CHECK(layout.cursor_column == 0);
    check_glyph(grid, 0, 0, (const uint8_t *)"a", 1);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_rectangle_region_style(void)
{
    static const uint8_t text[] = "abcd\nabcd\nabcd";
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmCellGrid *grid = NULL;
    KmLayoutResult layout;
    KmEvent event = {0};
    KmError error;
    size_t row;

    CHECK(km_buffer_create_base(text, sizeof(text) - 1, &buffer, &error) == KM_OK);
    km_buffer_set_line_numbers_visible(buffer, false);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    event.kind = KM_EVENT_KEY;
    event.repeat = 1;
    event.codepoint = 'x';
    event.modifiers = KM_MOD_CTRL;
    CHECK(km_command_loop_dispatch(loop, view, &event, &error) == KM_OK);
    event.codepoint = ' ';
    event.modifiers = 0;
    CHECK(km_command_loop_dispatch(loop, view, &event, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){13}, &error) == KM_OK);
    CHECK(km_layout_view(buffer, view, 5, 8, 0, NULL, NULL, false, &grid,
                         &layout, &error) == KM_OK);
    for (row = 0; row < 3; ++row) {
        CHECK(km_cell_grid_cell(grid, row, 0)->style_id == KM_STYLE_DEFAULT);
        CHECK(km_cell_grid_cell(grid, row, 1)->style_id == KM_STYLE_REGION);
        CHECK(km_cell_grid_cell(grid, row, 2)->style_id == KM_STYLE_REGION);
        CHECK(km_cell_grid_cell(grid, row, 3)->style_id == KM_STYLE_DEFAULT);
    }
    km_cell_grid_destroy(grid);
    km_command_loop_destroy(loop);
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
    CHECK(km_view_point(view).v == 1);
    {
        KmCellGrid *grid = NULL;
        KmLayoutResult layout;
        CHECK(km_layout_view(buffer, view, 5, 4, scroll, NULL, NULL, false, &grid,
                             &layout, &error) == KM_OK);
        CHECK(layout.scroll_row == scroll);
        CHECK(layout.cursor_row <
              (km_config_mode_line_format() ? 3u : 4u));
        km_cell_grid_destroy(grid);
    }
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_recenter_scroll(void)
{
    KmLayoutResult layout = {0};
    KmError error;
    size_t scroll = 99;

    layout.scroll_row = 8;
    layout.cursor_row = 2;
    layout.visual_rows = 20;
    CHECK(km_layout_recenter_scroll(&layout, 7, false, 1, &scroll, &error) ==
          KM_OK);
    CHECK(scroll == 8);
    CHECK(km_layout_recenter_scroll(&layout, 7, true, 0, &scroll, &error) ==
          KM_OK);
    CHECK(scroll == 10);
    CHECK(km_layout_recenter_scroll(&layout, 7, true, -1, &scroll, &error) ==
          KM_OK);
    CHECK(scroll == 6);

    layout.scroll_row = 15;
    layout.cursor_row = 3;
    CHECK(km_layout_recenter_scroll(&layout, 7, false, 1, &scroll, &error) ==
          KM_OK);
    CHECK(scroll == 15);
    layout.scroll_row = SIZE_MAX;
    layout.cursor_row = 1;
    CHECK(km_layout_recenter_scroll(&layout, 7, false, 1, &scroll, &error) ==
          KM_ERR_INVALID);
}

static void test_move_to_window_line(void)
{
    static const uint8_t text[] = "0\n1\n2\n3\n4\n5\n";
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    KmError error;

    CHECK(km_buffer_create_base(text, sizeof(text) - 1, &buffer, &error) ==
          KM_OK);
    km_buffer_set_line_numbers_visible(buffer, false);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_layout_move_to_window_line(buffer, view, 7, 20, 2, false, 1,
                                        &error) == KM_OK);
    CHECK(km_view_point(view).v == 8);
    CHECK(km_layout_move_to_window_line(buffer, view, 7, 20, 2, true, 0,
                                        &error) == KM_OK);
    CHECK(km_view_point(view).v == 4);
    CHECK(km_layout_move_to_window_line(buffer, view, 7, 20, 2, true, -1,
                                        &error) == KM_OK);
    CHECK(km_view_point(view).v == 12);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_multi_window_frame(void)
{
    static const uint8_t first_text[] = "one\ntwo\nthree";
    static const uint8_t second_text[] = "alpha\nbeta";
    KmBuffer *first = NULL;
    KmBuffer *second = NULL;
    KmView *first_view = NULL;
    KmView *second_view = NULL;
    KmLayoutWindow windows[2];
    KmCellGrid *grid = NULL;
    KmLayoutResult layout;
    KmError error;

    CHECK(km_buffer_create_base(first_text, sizeof(first_text) - 1, &first,
                                &error) == KM_OK);
    CHECK(km_buffer_create_base(second_text, sizeof(second_text) - 1, &second,
                                &error) == KM_OK);
    CHECK(km_buffer_set_name(first, "first", &error) == KM_OK);
    CHECK(km_buffer_set_name(second, "second", &error) == KM_OK);
    CHECK(km_view_create(first, &first_view, &error) == KM_OK);
    CHECK(km_view_create(second, &second_view, &error) == KM_OK);
    windows[0] = (KmLayoutWindow){first, first_view, 0, 0};
    windows[1] = (KmLayoutWindow){second, second_view, 0, 0};

    CHECK(km_layout_frame(windows, 2, 1, 7, 20, "Ready", NULL, false,
                          &grid, &layout, &error) == KM_OK);
    CHECK(windows[0].rows == 3 && windows[1].rows == 3);
    CHECK(km_cell_grid_cell(grid, 2, 0)->style_id == KM_STYLE_MODELINE);
    CHECK(km_cell_grid_cell(grid, 5, 0)->style_id == KM_STYLE_MODELINE);
    check_glyph(grid, 2, 3, (const uint8_t *)"f", 1);
    check_glyph(grid, 5, 3, (const uint8_t *)"s", 1);
    check_glyph(grid, 6, 0, (const uint8_t *)"R", 1);
    CHECK(layout.cursor_row >= 3 && layout.cursor_row < 5);
    km_cell_grid_destroy(grid);

    CHECK(km_layout_frame(windows, 2, 1, 7, 20, "Find: ", " {a | b}", true,
                          &grid, &layout, &error) == KM_OK);
    CHECK(layout.cursor_row == 6);
    check_glyph(grid, 6, 0, (const uint8_t *)"F", 1);
    km_cell_grid_destroy(grid);

    CHECK(km_layout_frame(windows, 2, 1, 2, 20, NULL, NULL, false, &grid,
                          &layout, &error) == KM_OK);
    CHECK(windows[0].rows == 0 && windows[1].rows == 1);
    CHECK(layout.cursor_row == 0);
    km_cell_grid_destroy(grid);
    CHECK(km_layout_frame(windows, 2, 1, 7, 20, NULL, NULL, false, &grid,
                          &layout, &error) == KM_OK);
    CHECK(windows[0].rows == 3 && windows[1].rows == 3);
    km_cell_grid_destroy(grid);
    CHECK(km_view_destroy(second_view, &error) == KM_OK);
    CHECK(km_view_destroy(first_view, &error) == KM_OK);
    CHECK(km_buffer_destroy(second, &error) == KM_OK);
    CHECK(km_buffer_destroy(first, &error) == KM_OK);
}

int main(void)
{
    test_configuration_contract();
    test_grapheme_scans_from_internal_boundaries();
    test_unicode_and_controls();
    test_wrap_and_viewport();
    test_unicode_file_name_status();
    test_line_number_gutter();
    test_rectangle_region_style();
    test_scroll_commands();
    test_recenter_scroll();
    test_move_to_window_line();
    test_multi_window_frame();
    puts("layout tests passed");
    return 0;
}
