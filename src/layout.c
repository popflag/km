#include "layout.h"
#include "configuration.h"

#include "unicode.h"

#include "utf8proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *text;
    size_t len;
    size_t point;
    size_t columns;
    size_t gutter_width;
    size_t line_number_base;
    size_t row;
    size_t column;
    size_t hard_line;
    size_t hard_column;
    size_t cursor_row;
    size_t cursor_column;
    size_t cursor_hard_line;
    size_t cursor_hard_column;
    size_t visual_rows;
    size_t seek_row;
    size_t seek_offset;
    size_t seek_cursor_row;
    size_t scroll_row;
    size_t content_rows;
    size_t region_start;
    size_t region_end;
    uint16_t style_id;
    KmCellGrid *grid;
    bool cursor_set;
    bool region_active;
    bool seek_active;
    bool seek_set;
} KmLayoutPass;

static KmStatus fail(KmError *error, KmStatus status, const char *operation)
{
    if (error != NULL) {
        error->code = status;
        error->os_code = 0;
        error->operation = operation;
    }
    return status;
}

static void normalize_cursor(const KmLayoutPass *pass, size_t *row,
                             size_t *column)
{
    if (*column == pass->columns) {
        ++*row;
        *column = 0;
    }
}

static void record_cursor(KmLayoutPass *pass, size_t offset,
                          size_t row, size_t column)
{
    size_t visual_rows;

    normalize_cursor(pass, &row, &column);
    visual_rows = row + 1;
    if (visual_rows > pass->visual_rows) pass->visual_rows = visual_rows;
    if (pass->seek_active && !pass->seek_set && row >= pass->seek_row) {
        pass->seek_offset = offset;
        pass->seek_cursor_row = row;
        pass->seek_set = true;
    }
    if (offset == pass->point) {
        pass->cursor_row = row;
        pass->cursor_column = pass->gutter_width + column;
        pass->cursor_hard_line = pass->hard_line;
        pass->cursor_hard_column = pass->hard_column;
        pass->cursor_set = true;
    }
}

static KmStatus draw_line_number(KmLayoutPass *pass, bool numbered,
                                 KmError *error)
{
    const uint8_t *separator =
        (const uint8_t *)km_config_line_number_separator();
    size_t padding = km_config_line_number_padding();
    char digits[32];
    size_t digit_len = 0;
    size_t digit_offset = 0;
    size_t number_columns =
        pass->gutter_width > padding + 1u
                                ? pass->gutter_width - padding - 1u
                                : 0;
    size_t separator_column =
        pass->gutter_width > padding
                                  ? pass->gutter_width - padding - 1u
                                  : 0;
    size_t start = number_columns;
    size_t column;

    if (numbered && number_columns != 0) {
        size_t line_number;
        if (pass->line_number_base == SIZE_MAX ||
            pass->hard_line > SIZE_MAX - pass->line_number_base - 1) {
            return fail(error, KM_ERR_INVALID, "format line number");
        }
        line_number = pass->line_number_base + pass->hard_line + 1;
        int length = snprintf(digits, sizeof(digits), "%llu",
                              (unsigned long long)line_number);
        if (length < 0 || (size_t)length >= sizeof(digits)) {
            return fail(error, KM_ERR_INVALID, "format line number");
        }
        digit_len = (size_t)length;
        if (digit_len > number_columns) {
            digit_offset = digit_len - number_columns;
            digit_len = number_columns;
            start = 0;
        } else {
            start = number_columns - digit_len;
        }
    }
    if (pass->grid == NULL || pass->row < pass->scroll_row ||
        pass->row - pass->scroll_row >= pass->content_rows) {
        return KM_OK;
    }
    for (column = 0; column < pass->gutter_width; ++column) {
        uint8_t space = ' ';
        const uint8_t *glyph = &space;
        size_t glyph_len = 1;
        if (numbered && column >= start && column < start + digit_len) {
            glyph = (const uint8_t *)&digits[digit_offset + column - start];
        } else if (column == separator_column) {
            glyph = separator;
            glyph_len = strlen((const char *)separator);
        }
        {
            KmStatus status = km_cell_grid_put(
                pass->grid, pass->row - pass->scroll_row, column,
                glyph, glyph_len, 1, KM_STYLE_LINE_NUMBER, error);
            if (status != KM_OK) return status;
        }
    }
    return KM_OK;
}

static KmStatus wrap_before(KmLayoutPass *pass, size_t width, KmError *error)
{
    if (pass->column == pass->columns ||
        width > pass->columns - pass->column) {
        ++pass->row;
        pass->column = 0;
        return draw_line_number(pass, false, error);
    }
    return KM_OK;
}

static KmStatus draw_glyph(KmLayoutPass *pass, const uint8_t *glyph,
                           size_t glyph_len, uint8_t width, KmError *error)
{
    if (pass->grid != NULL && pass->row >= pass->scroll_row &&
        pass->row - pass->scroll_row < pass->content_rows) {
        return km_cell_grid_put(pass->grid, pass->row - pass->scroll_row,
                                pass->gutter_width + pass->column,
                                glyph, glyph_len, width,
                                pass->style_id, error);
    }
    return KM_OK;
}

static void select_region_style(KmLayoutPass *pass, size_t start, size_t end)
{
    pass->style_id = pass->region_active && start < pass->region_end &&
                             end > pass->region_start
                         ? KM_STYLE_REGION
                         : KM_STYLE_DEFAULT;
}

static KmStatus draw_ascii_cell(KmLayoutPass *pass, uint8_t glyph,
                                KmError *error)
{
    KmStatus status;

    status = wrap_before(pass, 1, error);
    if (status != KM_OK) return status;
    status = draw_glyph(pass, &glyph, 1, 1, error);
    if (status != KM_OK) return status;
    ++pass->column;
    ++pass->hard_column;
    return KM_OK;
}

static bool is_ascii_control(utf8proc_int32_t codepoint)
{
    return (codepoint >= 0 && codepoint < 0x20) || codepoint == 0x7f;
}

static KmStatus draw_control(KmLayoutPass *pass, utf8proc_int32_t codepoint,
                             KmError *error)
{
    uint8_t second = codepoint == 0x7f
                         ? (uint8_t)'?'
                         : (uint8_t)('@' + codepoint);
    KmStatus status = draw_ascii_cell(pass, '^', error);

    if (status != KM_OK) return status;
    return draw_ascii_cell(pass, second, error);
}

static KmStatus record_inside_grapheme(KmLayoutPass *pass, size_t start,
                                       size_t end, uint8_t width,
                                       KmError *error)
{
    size_t scan = start;
    size_t prefix_width = 0;

    while (scan < end) {
        utf8proc_int32_t codepoint;
        size_t consumed;
        int codepoint_width;
        KmStatus status = km_unicode_decode(pass->text, end, scan, &codepoint,
                                            &consumed, error);
        if (status != KM_OK) return status;
        codepoint_width = km_unicode_cell_width(codepoint);
        if (codepoint_width > 0 && (size_t)codepoint_width > prefix_width) {
            prefix_width = (size_t)codepoint_width;
        }
        scan += consumed;
        if (scan < end && scan == pass->point) {
            size_t row = pass->row;
            size_t column = pass->column +
                            (prefix_width < width ? prefix_width : width);
            normalize_cursor(pass, &row, &column);
            pass->cursor_row = row;
            pass->cursor_column = pass->gutter_width + column;
            pass->cursor_hard_line = pass->hard_line;
            pass->cursor_hard_column = pass->hard_column + prefix_width;
            pass->cursor_set = true;
        }
    }
    return KM_OK;
}

static KmStatus run_layout_pass(KmLayoutPass *pass, KmError *error)
{
    static const uint8_t replacement[] = {0xef, 0xbf, 0xbd};
    size_t offset = 0;
    KmStatus status;

    record_cursor(pass, 0, 0, 0);
    status = draw_line_number(pass, true, error);
    if (status != KM_OK) return status;
    while (offset < pass->len) {
        utf8proc_int32_t codepoint;
        size_t consumed;
        status = km_unicode_decode(pass->text, pass->len, offset,
                                   &codepoint, &consumed, error);
        if (status != KM_OK) return status;
        if (codepoint == '\n') {
            bool already_wrapped = pass->column == pass->columns;
            if (already_wrapped) {
                ++pass->row;
                pass->column = 0;
            }
            record_cursor(pass, offset, pass->row, pass->column);
            if (!already_wrapped) ++pass->row;
            pass->column = 0;
            ++pass->hard_line;
            pass->hard_column = 0;
            offset += consumed;
            status = draw_line_number(pass, true, error);
            if (status != KM_OK) return status;
            record_cursor(pass, offset, pass->row, pass->column);
        } else if (codepoint == '\t') {
            size_t spaces;
            record_cursor(pass, offset, pass->row, pass->column);
            select_region_style(pass, offset, offset + consumed);
            spaces = km_config_tab_width() -
                     (pass->hard_column % km_config_tab_width());
            while (spaces-- != 0) {
                status = draw_ascii_cell(pass, ' ', error);
                if (status != KM_OK) return status;
            }
            offset += consumed;
            record_cursor(pass, offset, pass->row, pass->column);
        } else if (is_ascii_control(codepoint)) {
            record_cursor(pass, offset, pass->row, pass->column);
            select_region_style(pass, offset, offset + consumed);
            status = draw_control(pass, codepoint, error);
            if (status != KM_OK) return status;
            offset += consumed;
            record_cursor(pass, offset, pass->row, pass->column);
        } else {
            KmGrapheme grapheme;
            size_t end;
            uint8_t width;
            const uint8_t *glyph;
            size_t glyph_len;
            bool too_wide;
            bool combining_only;
            uint8_t *combined = NULL;

            status = km_unicode_next_grapheme(pass->text, pass->len, offset,
                                              &grapheme, error);
            if (status != KM_OK) return status;
            end = grapheme.end;
            width = grapheme.width;
            combining_only = grapheme.combining_only;
            select_region_style(pass, offset, end);
            too_wide = width > pass->columns;
            if (too_wide) width = 1;
            status = wrap_before(pass, width, error);
            if (status != KM_OK) return status;
            record_cursor(pass, offset, pass->row, pass->column);
            status = record_inside_grapheme(pass, offset, end, width, error);
            if (status != KM_OK) return status;
            glyph = pass->text + offset;
            glyph_len = end - offset;
            if (combining_only && pass->grid != NULL) {
                static const uint8_t dotted_circle[] = {0xe2, 0x97, 0x8c};
                if (glyph_len > SIZE_MAX - sizeof(dotted_circle)) {
                    return fail(error, KM_ERR_OOM, "layout combining glyph");
                }
                combined = (uint8_t *)malloc(sizeof(dotted_circle) + glyph_len);
                if (combined == NULL) {
                    return fail(error, KM_ERR_OOM, "layout combining glyph");
                }
                memcpy(combined, dotted_circle, sizeof(dotted_circle));
                memcpy(combined + sizeof(dotted_circle), glyph, glyph_len);
                glyph = combined;
                glyph_len += sizeof(dotted_circle);
            } else if (too_wide ||
                utf8proc_category(codepoint) == UTF8PROC_CATEGORY_CC) {
                glyph = replacement;
                glyph_len = sizeof(replacement);
                width = 1;
            }
            status = draw_glyph(pass, glyph, glyph_len, width, error);
            free(combined);
            if (status != KM_OK) return status;
            pass->column += width;
            pass->hard_column += width;
            offset = end;
            record_cursor(pass, offset, pass->row, pass->column);
        }
    }
    if (!pass->cursor_set) {
        return fail(error, KM_ERR_INVALID, "layout point");
    }
    return KM_OK;
}

static KmStatus put_status_text(KmCellGrid *grid, size_t row, size_t columns,
                                size_t *column, const char *text,
                                uint16_t style_id,
                                KmError *error)
{
    static const uint8_t replacement[] = {0xef, 0xbf, 0xbd};
    size_t len = strlen(text);
    size_t offset = 0;

    while (offset < len && *column < columns) {
        int32_t codepoint;
        size_t consumed;
        const uint8_t *glyph;
        size_t glyph_len;
        uint8_t width;
        KmStatus status = km_unicode_decode((const uint8_t *)text, len, offset,
                                            &codepoint, &consumed, error);
        if (status != KM_OK) return status;
        if ((codepoint >= 0 && codepoint < 0x20) || codepoint == 0x7f) {
            static const uint8_t question[] = {'?'};
            glyph = question;
            glyph_len = sizeof(question);
            width = 1;
            offset += consumed;
        } else {
            KmGrapheme grapheme;
            status = km_unicode_next_grapheme((const uint8_t *)text, len,
                                              offset, &grapheme, error);
            if (status != KM_OK) return status;
            if (grapheme.combining_only) {
                glyph = replacement;
                glyph_len = sizeof(replacement);
                width = 1;
            } else {
                glyph = (const uint8_t *)text + offset;
                glyph_len = grapheme.end - offset;
                width = grapheme.width;
            }
            offset = grapheme.end;
        }
        if (width > columns - *column) break;
        status = km_cell_grid_put(grid, row, *column, glyph, glyph_len, width,
                                  style_id, error);
        if (status != KM_OK) return status;
        *column += width;
    }
    return KM_OK;
}

static KmStatus put_status(KmCellGrid *grid, size_t row, size_t columns,
                           const KmLayoutResult *result, bool modified,
                           const char *name, bool region_active,
                           KmError *error)
{
    char location[96];
    int length = snprintf(location, sizeof(location), "  L%llu C%llu",
                          (unsigned long long)result->hard_line + 1u,
                          (unsigned long long)result->hard_column + 1u);
    size_t column = 0;
    KmStatus status;
    size_t fill;

    if (length < 0 || (size_t)length >= sizeof(location)) {
        return fail(error, KM_ERR_INVALID, "format status line");
    }
    for (fill = 0; fill < columns; ++fill) {
        status = km_cell_grid_put(grid, row, fill, (const uint8_t *)" ", 1, 1,
                                  KM_STYLE_MODELINE, error);
        if (status != KM_OK) return status;
    }
    status = put_status_text(grid, row, columns, &column,
                             modified ? "** " : "-- ", KM_STYLE_MODELINE,
                             error);
    if (status == KM_OK) {
        status = put_status_text(grid, row, columns, &column, name,
                                 KM_STYLE_MODELINE, error);
    }
    if (status == KM_OK && region_active) {
        status = put_status_text(grid, row, columns, &column, " [Region]",
                                 KM_STYLE_MODELINE, error);
    }
    if (status == KM_OK) {
        status = put_status_text(grid, row, columns, &column, location,
                                 KM_STYLE_MODELINE, error);
    }
    return status;
}

static size_t content_row_count(size_t rows)
{
    size_t chrome_rows = km_config_mode_line_format() ? 2u : 1u;
    return rows > chrome_rows ? rows - chrome_rows : 1u;
}

static KmStatus line_number_base(const KmDocument *document, KmBytePos start,
                                 size_t *out_base, KmError *error)
{
    /* ponytail: redraw-time scan; add a line index only after profiling. */
    uint8_t bytes[4096];
    KmTextIter iterator;
    size_t remaining = start.v;
    size_t base = 0;
    KmStatus status = km_document_iter_init(document, (KmBytePos){0},
                                            &iterator, error);

    if (status != KM_OK) return status;
    while (remaining != 0) {
        size_t count;
        bool eof;
        size_t capacity = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
        status = km_text_iter_read(&iterator, bytes, capacity, &count, &eof,
                                   error);
        if (status != KM_OK) return status;
        for (size_t i = 0; i < count; ++i) {
            if (bytes[i] == '\n') ++base;
        }
        remaining -= count;
        if (count == 0 || eof) break;
    }
    if (remaining != 0) return fail(error, KM_ERR_INVALID, "line number");
    *out_base = base;
    return KM_OK;
}

static size_t line_number_gutter(const uint8_t *text, size_t len,
                                 size_t base, size_t columns)
{
    size_t lines = 1;
    size_t digits = 1;
    size_t i;
    size_t value;
    size_t gutter;

    for (i = 0; i < len; ++i) {
        if (text[i] == '\n' && lines != SIZE_MAX) ++lines;
    }
    value = base <= SIZE_MAX - lines ? base + lines : SIZE_MAX;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    if (km_config_line_number_padding() >
        (SIZE_MAX - digits - 1u) / 2u) {
        gutter = SIZE_MAX;
    } else {
        gutter = digits + km_config_line_number_padding() * 2u + 1u;
    }
    if (columns <= 1) return 0;
    if (gutter >= columns) gutter = columns - 1;
    return gutter;
}

KmStatus km_layout_view(const KmBuffer *buffer, const KmView *view,
                        size_t rows, size_t columns, size_t scroll_row,
                        const char *message, const char *completion,
                        bool prompt_active,
                        KmCellGrid **out_grid,
                        KmLayoutResult *out_result, KmError *error)
{
    const KmDocument *document;
    KmBytePos start;
    KmBytePos end;
    KmBytePos point;
    KmBytePos mark;
    uint8_t *text = NULL;
    size_t len;
    size_t content_rows;
    size_t gutter_width;
    size_t text_columns;
    size_t line_base;
    KmCellGrid *grid = NULL;
    KmLayoutPass pass_template = {0};
    KmLayoutPass pass = {0};
    KmLayoutResult result = {0};
    KmStatus status;

    km_error_clear(error);
    if (out_grid == NULL || out_result == NULL || buffer == NULL ||
        view == NULL || km_view_buffer(view) != buffer || rows == 0 ||
        columns == 0) {
        return fail(error, KM_ERR_INVALID, "layout view");
    }
    *out_grid = NULL;
    *out_result = (KmLayoutResult){0};
    document = km_buffer_document(buffer);
    start = km_buffer_accessible_start(buffer);
    end = km_buffer_accessible_end(buffer);
    point = km_view_point(view);
    mark = km_buffer_mark(buffer);
    if (document == NULL || start.v > point.v || point.v > end.v) {
        return fail(error, KM_ERR_INVALID, "layout view");
    }
    len = end.v - start.v;
    if (len != 0) {
        /* ponytail: full redraw copy; add visible-line caches after profiling. */
        text = (uint8_t *)malloc(len);
        if (text == NULL) return fail(error, KM_ERR_OOM, "layout text");
        status = km_document_copy(document, start, len, text, error);
        if (status != KM_OK) goto fail;
    }
    content_rows = content_row_count(rows);
    status = line_number_base(document, start, &line_base, error);
    if (status != KM_OK) goto fail;
    gutter_width = km_buffer_line_numbers_visible(buffer)
                       ? line_number_gutter(text, len, line_base, columns)
                       : 0;
    text_columns = columns - gutter_width;
    status = km_cell_grid_create(rows, columns, &grid, error);
    if (status != KM_OK) goto fail;
    pass_template.text = text;
    pass_template.len = len;
    pass_template.point = point.v - start.v;
    pass_template.columns = text_columns;
    pass_template.gutter_width = gutter_width;
    pass_template.line_number_base = line_base;
    pass_template.scroll_row = scroll_row;
    pass_template.content_rows = content_rows;
    pass_template.grid = grid;
    pass_template.region_active = km_buffer_mark_active(buffer);
    pass_template.region_start =
        mark.v < point.v ? mark.v - start.v : point.v - start.v;
    pass_template.region_end =
        mark.v < point.v ? point.v - start.v : mark.v - start.v;
    pass = pass_template;
    status = run_layout_pass(&pass, error);
    if (status != KM_OK) goto fail;
    if (pass.cursor_row < scroll_row) {
        scroll_row = pass.cursor_row;
    } else if (pass.cursor_row - scroll_row >= content_rows) {
        scroll_row = pass.cursor_row - content_rows + 1;
    }
    if (scroll_row != pass_template.scroll_row) {
        km_cell_grid_destroy(grid);
        grid = NULL;
        status = km_cell_grid_create(rows, columns, &grid, error);
        if (status != KM_OK) goto fail;
        pass_template.scroll_row = scroll_row;
        pass_template.grid = grid;
        pass = pass_template;
        status = run_layout_pass(&pass, error);
        if (status != KM_OK) goto fail;
    }
    result.cursor_row = pass.cursor_row - scroll_row;
    result.cursor_column = pass.cursor_column;
    result.scroll_row = scroll_row;
    result.visual_rows = pass.visual_rows;
    result.hard_line = line_base + pass.cursor_hard_line;
    result.hard_column = pass.cursor_hard_column;

    if (!km_config_mode_line_format() && rows > 1) {
        size_t echo_column = 0;
        status = put_status_text(grid, rows - 1, columns, &echo_column,
                                 message == NULL ? "" : message,
                                 KM_STYLE_DEFAULT, error);
        if (status != KM_OK) goto fail;
        if (prompt_active) {
            result.cursor_row = rows - 1;
            result.cursor_column = echo_column < columns
                                       ? echo_column
                                       : columns - 1;
            status = put_status_text(grid, rows - 1, columns, &echo_column,
                                     completion == NULL ? "" : completion,
                                     KM_STYLE_DEFAULT, error);
            if (status != KM_OK) goto fail;
        }
    } else if (km_config_mode_line_format() && rows > 2) {
        size_t echo_column = 0;
        status = put_status(grid, rows - 2, columns, &result,
                            km_buffer_is_modified(buffer),
                            km_buffer_name(buffer), km_buffer_mark_active(buffer),
                            error);
        if (status != KM_OK) goto fail;
        status = put_status_text(grid, rows - 1, columns, &echo_column,
                                 message == NULL ? "" : message,
                                 KM_STYLE_DEFAULT, error);
        if (status != KM_OK) goto fail;
        if (prompt_active) {
            result.cursor_row = rows - 1;
            result.cursor_column = echo_column < columns
                                       ? echo_column
                                       : columns - 1;
            status = put_status_text(grid, rows - 1, columns, &echo_column,
                                     completion == NULL ? "" : completion,
                                     KM_STYLE_DEFAULT, error);
            if (status != KM_OK) goto fail;
        }
    } else if (rows > 1 &&
               (prompt_active || (message != NULL && message[0] != '\0'))) {
        size_t echo_column = 0;
        status = put_status_text(grid, rows - 1, columns, &echo_column,
                                 message == NULL ? "" : message,
                                 KM_STYLE_DEFAULT, error);
        if (status != KM_OK) goto fail;
        if (prompt_active) {
            result.cursor_row = rows - 1;
            result.cursor_column = echo_column < columns
                                       ? echo_column
                                       : columns - 1;
            status = put_status_text(grid, rows - 1, columns, &echo_column,
                                     completion == NULL ? "" : completion,
                                     KM_STYLE_DEFAULT, error);
            if (status != KM_OK) goto fail;
        }
    } else if (rows > 1) {
        status = put_status(grid, rows - 1, columns, &result,
                            km_buffer_is_modified(buffer),
                            km_buffer_name(buffer), km_buffer_mark_active(buffer),
                            error);
        if (status != KM_OK) goto fail;
    } else if (prompt_active || (message != NULL && message[0] != '\0')) {
        size_t echo_column = 0;
        status = put_status_text(grid, 0, columns, &echo_column,
                                 message == NULL ? "" : message,
                                 KM_STYLE_DEFAULT, error);
        if (status != KM_OK) goto fail;
        if (prompt_active) {
            result.cursor_row = 0;
            result.cursor_column = echo_column < columns
                                       ? echo_column
                                       : columns - 1;
            status = put_status_text(grid, 0, columns, &echo_column,
                                     completion == NULL ? "" : completion,
                                     KM_STYLE_DEFAULT, error);
            if (status != KM_OK) goto fail;
        }
    }
    free(text);
    *out_grid = grid;
    *out_result = result;
    return KM_OK;

fail:
    free(text);
    km_cell_grid_destroy(grid);
    return status;
}

static KmStatus copy_grid_rows(KmCellGrid *destination, size_t destination_row,
                               const KmCellGrid *source, size_t source_row,
                               size_t row_count, KmError *error)
{
    size_t columns = km_cell_grid_columns(destination);
    size_t row;

    if (columns != km_cell_grid_columns(source) ||
        destination_row > km_cell_grid_rows(destination) ||
        row_count > km_cell_grid_rows(destination) - destination_row ||
        source_row > km_cell_grid_rows(source) ||
        row_count > km_cell_grid_rows(source) - source_row) {
        return fail(error, KM_ERR_INVALID, "copy layout grid");
    }
    for (row = 0; row < row_count; ++row) {
        size_t column;
        for (column = 0; column < columns; ++column) {
            const KmCell *cell =
                km_cell_grid_cell(source, source_row + row, column);
            const uint8_t *glyph;
            size_t glyph_len;
            KmStatus status;

            if (cell == NULL || cell->width == 0 ||
                (cell->flags & KM_CELL_CONTINUATION) != 0) {
                continue;
            }
            glyph = km_cell_grid_cell_glyph(source, source_row + row, column,
                                             &glyph_len);
            if (glyph == NULL) continue;
            status = km_cell_grid_put(destination, destination_row + row,
                                      column, glyph, glyph_len, cell->width,
                                      cell->style_id, error);
            if (status != KM_OK) return status;
        }
    }
    return KM_OK;
}

KmStatus km_layout_frame(KmLayoutWindow *windows, size_t window_count,
                         size_t selected_window, size_t rows, size_t columns,
                         const char *message, const char *completion,
                         bool prompt_active, KmCellGrid **out_grid,
                         KmLayoutResult *out_result, KmError *error)
{
    KmCellGrid *frame = NULL;
    KmLayoutResult selected = {0};
    size_t echo_rows;
    size_t available_rows;
    size_t minimum_rows;
    size_t visible_count;
    size_t base_rows;
    size_t extra_rows;
    size_t top = 0;
    size_t i;
    KmStatus status;

    km_error_clear(error);
    if (windows == NULL || window_count == 0 ||
        selected_window >= window_count || rows == 0 || columns == 0 ||
        out_grid == NULL || out_result == NULL) {
        return fail(error, KM_ERR_INVALID, "layout frame");
    }
    echo_rows = rows > 1 ? 1u : 0u;
    available_rows = rows - echo_rows;
    minimum_rows = km_config_mode_line_format() ? 2u : 1u;
    visible_count = window_count <= available_rows / minimum_rows
                        ? window_count
                        : 1u;
    *out_grid = NULL;
    *out_result = (KmLayoutResult){0};
    status = km_cell_grid_create(rows, columns, &frame, error);
    if (status != KM_OK) return status;
    base_rows = available_rows / visible_count;
    extra_rows = available_rows % visible_count;

    for (i = 0; i < window_count; ++i) {
        size_t visible_index = visible_count == 1 ? 0 : i;
        size_t window_rows;
        size_t layout_rows;
        KmCellGrid *window_grid = NULL;
        KmLayoutResult layout;

        windows[i].rows = 0;
        if (windows[i].buffer == NULL || windows[i].view == NULL ||
            km_view_buffer(windows[i].view) != windows[i].buffer) {
            status = fail(error, KM_ERR_INVALID, "layout frame");
            goto fail;
        }
        if (visible_count == 1 && window_count != 1 && i != selected_window) {
            continue;
        }
        window_rows = base_rows + (visible_index < extra_rows ? 1u : 0u);
        layout_rows = window_rows + echo_rows;
        status = km_layout_view(
            windows[i].buffer, windows[i].view, layout_rows, columns,
            windows[i].scroll_row, i == selected_window ? message : NULL,
            i == selected_window ? completion : NULL,
            i == selected_window && prompt_active, &window_grid, &layout,
            error);
        if (status != KM_OK) goto fail;
        status = copy_grid_rows(frame, top, window_grid, 0, window_rows, error);
        if (status == KM_OK && i == selected_window && echo_rows != 0) {
            status = copy_grid_rows(frame, rows - 1, window_grid, window_rows,
                                    1, error);
        }
        km_cell_grid_destroy(window_grid);
        if (status != KM_OK) goto fail;
        windows[i].scroll_row = layout.scroll_row;
        windows[i].rows = window_rows;
        if (i == selected_window) {
            selected = layout;
            if (prompt_active && echo_rows != 0) {
                selected.cursor_row = rows - 1;
            } else {
                selected.cursor_row += top;
            }
        }
        top += window_rows;
    }
    *out_grid = frame;
    *out_result = selected;
    return KM_OK;

fail:
    km_cell_grid_destroy(frame);
    return status;
}

KmStatus km_layout_scroll_view(const KmBuffer *buffer, KmView *view,
                               size_t rows, size_t columns,
                               size_t scroll_row, bool forward,
                               bool has_argument, int64_t argument,
                               size_t *out_scroll_row, KmError *error)
{
    const KmDocument *document;
    KmBytePos start;
    KmBytePos end;
    KmBytePos point;
    uint8_t *text = NULL;
    size_t len;
    size_t line_base;
    size_t gutter_width;
    size_t content_rows;
    size_t max_scroll;
    size_t delta;
    size_t target;
    uint64_t magnitude;
    KmLayoutPass pass = {0};
    KmStatus status;

    km_error_clear(error);
    if (buffer == NULL || view == NULL || km_view_buffer(view) != buffer ||
        rows == 0 || columns == 0 || out_scroll_row == NULL) {
        return fail(error, KM_ERR_INVALID, "scroll view");
    }
    *out_scroll_row = scroll_row;
    document = km_buffer_document(buffer);
    start = km_buffer_accessible_start(buffer);
    end = km_buffer_accessible_end(buffer);
    point = km_view_point(view);
    if (document == NULL || start.v > point.v || point.v > end.v) {
        return fail(error, KM_ERR_INVALID, "scroll view");
    }
    len = end.v - start.v;
    if (len != 0) {
        text = (uint8_t *)malloc(len);
        if (text == NULL) return fail(error, KM_ERR_OOM, "scroll text");
        status = km_document_copy(document, start, len, text, error);
        if (status != KM_OK) goto done;
    }
    status = line_number_base(document, start, &line_base, error);
    if (status != KM_OK) goto done;
    gutter_width = km_buffer_line_numbers_visible(buffer)
                       ? line_number_gutter(text, len, line_base, columns)
                       : 0;
    content_rows = content_row_count(rows);
    pass.text = text;
    pass.len = len;
    pass.point = point.v - start.v;
    pass.columns = columns - gutter_width;
    pass.gutter_width = gutter_width;
    pass.line_number_base = line_base;
    pass.content_rows = content_rows;
    status = run_layout_pass(&pass, error);
    if (status != KM_OK) goto done;
    max_scroll = pass.visual_rows > content_rows
                     ? pass.visual_rows - content_rows
                     : 0;
    if (scroll_row > max_scroll) scroll_row = max_scroll;
    if (pass.cursor_row < scroll_row) {
        scroll_row = pass.cursor_row;
    } else if (pass.cursor_row - scroll_row >= content_rows) {
        scroll_row = pass.cursor_row - content_rows + 1;
    }
    if (!has_argument) {
        magnitude = content_rows > km_config_scroll_context_rows()
                        ? (uint64_t)(content_rows -
                                     km_config_scroll_context_rows())
                        : 1;
    } else if (argument < 0) {
        forward = !forward;
        magnitude = (uint64_t)(-(argument + 1)) + 1u;
    } else {
        magnitude = (uint64_t)argument;
    }
    if (magnitude == 0) {
        *out_scroll_row = scroll_row;
        status = KM_OK;
        goto done;
    }
    delta = magnitude > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)magnitude;
    if (forward) {
        target = delta > max_scroll - scroll_row
                     ? max_scroll
                     : scroll_row + delta;
    } else {
        target = delta > scroll_row ? 0 : scroll_row - delta;
    }
    if (target == scroll_row) {
        status = fail(error, KM_ERR_INVALID,
                      forward ? "end of buffer" : "beginning of buffer");
        goto done;
    }
    if (pass.cursor_row < target ||
        pass.cursor_row - target >= content_rows) {
        size_t point_row = pass.cursor_row < target
                               ? target
                               : target + content_rows - 1;
        pass = (KmLayoutPass){0};
        pass.text = text;
        pass.len = len;
        pass.point = point.v - start.v;
        pass.columns = columns - gutter_width;
        pass.gutter_width = gutter_width;
        pass.line_number_base = line_base;
        pass.content_rows = content_rows;
        pass.seek_active = true;
        pass.seek_row = point_row;
        status = run_layout_pass(&pass, error);
        if (status != KM_OK) goto done;
        if (!pass.seek_set || pass.seek_offset > len ||
            start.v > SIZE_MAX - pass.seek_offset) {
            status = fail(error, KM_ERR_INVALID, "scroll point");
            goto done;
        }
        if (pass.seek_cursor_row < target) {
            target = pass.seek_cursor_row;
        } else if (pass.seek_cursor_row - target >= content_rows) {
            target = pass.seek_cursor_row - content_rows + 1;
        }
        status = km_view_set_point(
            view, (KmBytePos){start.v + pass.seek_offset}, error);
        if (status != KM_OK) goto done;
    }
    *out_scroll_row = target;
    status = KM_OK;

done:
    free(text);
    return status;
}

static size_t requested_window_row(size_t content_rows, bool has_argument,
                                   int64_t argument)
{
    if (!has_argument) return content_rows / 2;
    if (argument >= 0) {
        return (uint64_t)argument >= (uint64_t)content_rows
                   ? content_rows - 1
                   : (size_t)argument;
    }
    {
        uint64_t from_bottom = (uint64_t)(-(argument + 1)) + 1u;
        return from_bottom >= (uint64_t)content_rows
                   ? 0
                   : content_rows - (size_t)from_bottom;
    }
}

KmStatus km_layout_recenter_scroll(const KmLayoutResult *layout, size_t rows,
                                   bool has_argument, int64_t argument,
                                   size_t *out_scroll_row, KmError *error)
{
    size_t content_rows;
    size_t desired_row;
    size_t cursor_row;
    size_t max_scroll;

    km_error_clear(error);
    if (layout == NULL || rows == 0 || out_scroll_row == NULL ||
        layout->scroll_row > SIZE_MAX - layout->cursor_row) {
        return fail(error, KM_ERR_INVALID, "recenter view");
    }
    content_rows = content_row_count(rows);
    desired_row = requested_window_row(content_rows, has_argument, argument);
    cursor_row = layout->scroll_row + layout->cursor_row;
    max_scroll = layout->visual_rows > content_rows
                     ? layout->visual_rows - content_rows
                     : 0;
    *out_scroll_row = cursor_row > desired_row
                          ? cursor_row - desired_row
                          : 0;
    if (*out_scroll_row > max_scroll) *out_scroll_row = max_scroll;
    return KM_OK;
}

KmStatus km_layout_move_to_window_line(const KmBuffer *buffer, KmView *view,
                                       size_t rows, size_t columns,
                                       size_t scroll_row, bool has_argument,
                                       int64_t argument, KmError *error)
{
    const KmDocument *document;
    KmBytePos start;
    KmBytePos end;
    KmBytePos point;
    uint8_t *text = NULL;
    size_t len;
    size_t content_rows;
    size_t line_base;
    size_t gutter_width;
    size_t target_row;
    size_t max_scroll;
    KmLayoutPass pass = {0};
    KmStatus status;

    km_error_clear(error);
    if (buffer == NULL || view == NULL || km_view_buffer(view) != buffer ||
        rows == 0 || columns == 0) {
        return fail(error, KM_ERR_INVALID, "move to window line");
    }
    document = km_buffer_document(buffer);
    start = km_buffer_accessible_start(buffer);
    end = km_buffer_accessible_end(buffer);
    point = km_view_point(view);
    if (document == NULL || start.v > point.v || point.v > end.v) {
        return fail(error, KM_ERR_INVALID, "move to window line");
    }
    len = end.v - start.v;
    if (len != 0) {
        text = (uint8_t *)malloc(len);
        if (text == NULL) return fail(error, KM_ERR_OOM, "layout text");
        status = km_document_copy(document, start, len, text, error);
        if (status != KM_OK) goto done;
    }
    status = line_number_base(document, start, &line_base, error);
    if (status != KM_OK) goto done;
    gutter_width = km_buffer_line_numbers_visible(buffer)
                       ? line_number_gutter(text, len, line_base, columns)
                       : 0;
    content_rows = content_row_count(rows);
    pass.text = text;
    pass.len = len;
    pass.point = point.v - start.v;
    pass.columns = columns - gutter_width;
    pass.gutter_width = gutter_width;
    pass.line_number_base = line_base;
    pass.content_rows = content_rows;
    status = run_layout_pass(&pass, error);
    if (status != KM_OK) goto done;
    if (pass.visual_rows == 0) {
        status = fail(error, KM_ERR_INVALID, "move to window line");
        goto done;
    }
    max_scroll = pass.visual_rows > content_rows
                     ? pass.visual_rows - content_rows
                     : 0;
    if (scroll_row > max_scroll) scroll_row = max_scroll;
    target_row = requested_window_row(content_rows, has_argument, argument);
    if (target_row > SIZE_MAX - scroll_row) {
        status = fail(error, KM_ERR_INVALID, "move to window line");
        goto done;
    }
    target_row += scroll_row;
    if (target_row >= pass.visual_rows) target_row = pass.visual_rows - 1;
    pass = (KmLayoutPass){0};
    pass.text = text;
    pass.len = len;
    pass.point = point.v - start.v;
    pass.columns = columns - gutter_width;
    pass.gutter_width = gutter_width;
    pass.line_number_base = line_base;
    pass.content_rows = content_rows;
    pass.seek_active = true;
    pass.seek_row = target_row;
    status = run_layout_pass(&pass, error);
    if (status != KM_OK) goto done;
    if (!pass.seek_set || pass.seek_offset > len ||
        start.v > SIZE_MAX - pass.seek_offset) {
        status = fail(error, KM_ERR_INVALID, "move to window line");
        goto done;
    }
    status = km_view_set_point(
        view, (KmBytePos){start.v + pass.seek_offset}, error);

done:
    free(text);
    return status;
}
