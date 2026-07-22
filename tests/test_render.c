#include "render.h"
#include "configuration.h"

#include "utf8proc.h"

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

static void test_utf8proc_version(void)
{
    CHECK(UTF8PROC_VERSION_MAJOR == 2);
    CHECK(UTF8PROC_VERSION_MINOR == 11);
    CHECK(UTF8PROC_VERSION_PATCH == 3);
    CHECK(strcmp(utf8proc_version(), "2.11.3") == 0);
    CHECK(strcmp(utf8proc_unicode_version(), "17.0.0") == 0);
}

static void test_probe_grid_and_output(void)
{
    KmCellGrid *grid = NULL;
    KmError error;
    const KmCell *lead;
    const KmCell *continuation;
    const uint8_t *glyph;
    char *output;
    size_t glyph_len;
    size_t output_len;

    CHECK(km_render_probe_grid_create(91, 37, true, &grid, &error) == KM_OK);
    CHECK(km_cell_grid_rows(grid) == 6);
    CHECK(km_cell_grid_columns(grid) == 91);
    glyph = km_cell_grid_cell_glyph(grid, 3, 11, &glyph_len);
    CHECK(glyph_len == 3 && memcmp(glyph, "e\xCC\x81", glyph_len) == 0);
    lead = km_cell_grid_cell(grid, 4, 5);
    continuation = km_cell_grid_cell(grid, 4, 6);
    CHECK(lead != NULL && lead->width == 2 && lead->glyph_len == 3);
    CHECK(continuation != NULL && continuation->width == 0 && continuation->glyph_len == 0);
    CHECK(km_cell_grid_encode_vt(grid, true, &output, &output_len, &error) == KM_OK);
    CHECK(output_len != 0 && strstr(output, "\x1b[2J\x1b[H") == output);
    CHECK(strstr(output, "size: 91x37") != NULL);
    CHECK(strstr(output, "CJK: \xE6\x96\x87\xE6\x9C\xAC") != NULL);
    free(output);
    km_cell_grid_destroy(grid);

    grid = NULL;
    CHECK(km_render_probe_grid_create(0, 0, false, &grid, &error) == KM_OK);
    CHECK(km_cell_grid_encode_vt(grid, false, &output, &output_len, &error) == KM_OK);
    CHECK(output_len != 0 && strchr(output, '\x1b') == NULL);
    CHECK(strstr(output, "ASCII: abc XYZ 123") != NULL);
    CHECK(strstr(output, "e\xCC\x81") != NULL);
    CHECK(strstr(output, "\xE6\x96\x87\xE6\x9C\xAC") != NULL);
    free(output);
    km_cell_grid_destroy(grid);
}

static void test_wide_cell_overwrite(void)
{
    static const uint8_t wide[] = {0xe6, 0x96, 0x87};
    static const uint8_t narrow[] = {'x'};
    KmCellGrid *grid = NULL;
    KmError error;
    char *output;
    size_t output_len;

    CHECK(km_cell_grid_create(1, 2, &grid, &error) == KM_OK);
    CHECK(km_cell_grid_put(grid, 0, 0, wide, sizeof(wide), 2, 0, &error) == KM_OK);
    CHECK(km_cell_grid_put(grid, 0, 0, narrow, sizeof(narrow), 1, 0, &error) == KM_OK);
    CHECK(km_cell_grid_cell(grid, 0, 1)->flags == 0);
    CHECK(km_cell_grid_encode_vt(grid, false, &output, &output_len, &error) == KM_OK);
    CHECK(strcmp(output, "x \n") == 0);
    free(output);

    CHECK(km_cell_grid_put(grid, 0, 0, wide, sizeof(wide), 2, 0, &error) == KM_OK);
    CHECK(km_cell_grid_put(grid, 0, 1, narrow, sizeof(narrow), 1, 0, &error) == KM_OK);
    CHECK(km_cell_grid_cell(grid, 0, 0)->glyph_len == 0);
    CHECK(km_cell_grid_encode_vt(grid, false, &output, &output_len, &error) == KM_OK);
    CHECK(strcmp(output, " x\n") == 0);
    free(output);
    km_cell_grid_destroy(grid);
}

static void test_interactive_frame(void)
{
    KmCellGrid *grid = NULL;
    KmError error;
    char *output;
    char *styled;
    size_t output_len;
    size_t styled_len;

    CHECK(km_cell_grid_create(2, 4, &grid, &error) == KM_OK);
    CHECK(km_cell_grid_put(grid, 1, 3, (const uint8_t *)"x", 1, 1, 0,
                           &error) == KM_OK);
    CHECK(km_cell_grid_put(grid, 0, 0, (const uint8_t *)"r", 1, 1,
                           KM_STYLE_REGION, &error) == KM_OK);
    CHECK(km_cell_grid_put(grid, 0, 1, (const uint8_t *)"l", 1, 1,
                           KM_STYLE_LINE_NUMBER, &error) == KM_OK);
    CHECK(km_cell_grid_put(grid, 0, 2, (const uint8_t *)"m", 1, 1,
                           KM_STYLE_MODELINE, &error) == KM_OK);
    CHECK(km_cell_grid_encode_frame_vt(grid, 1, 3, &output, &output_len,
                                       &error) == KM_OK);
    CHECK(strstr(output, "\x1b[2;1H   x") != NULL);
    styled_len = strlen("\x1b[1;1H") + strlen(km_config_style_region()) +
                 strlen(km_config_style_line_number()) +
                 strlen(km_config_style_mode_line()) +
                 strlen(km_config_style_default()) + 3u;
    styled = (char *)malloc(styled_len + 1u);
    CHECK(styled != NULL);
    CHECK((size_t)snprintf(styled, styled_len + 1u, "%s%sr%sl%sm%s",
                           "\x1b[1;1H", km_config_style_region(),
                           km_config_style_line_number(),
                           km_config_style_mode_line(),
                           km_config_style_default()) == styled_len);
    CHECK(strstr(output, styled) != NULL);
    free(styled);
    CHECK(strstr(output, "\x1b[2;4H\x1b[?25h") != NULL);
    CHECK(output_len != 0 && output[output_len - 1] == 'h');
    CHECK(memchr(output, '\n', output_len) == NULL);
    free(output);
    km_cell_grid_destroy(grid);
}

int main(void)
{
    test_utf8proc_version();
    test_probe_grid_and_output();
    test_wide_cell_overwrite();
    test_interactive_frame();
    puts("render tests passed");
    return 0;
}
