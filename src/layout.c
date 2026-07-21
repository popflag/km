#include "layout.h"

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
    char digits[32];
    size_t digit_len = 0;
    size_t digit_offset = 0;
    size_t number_columns = pass->gutter_width == 0
                                ? 0
                                : pass->gutter_width - 1;
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
        uint8_t glyph = ' ';
        if (numbered && column >= start && column < start + digit_len) {
            glyph = (uint8_t)digits[digit_offset + column - start];
        }
        {
            KmStatus status = km_cell_grid_put(
                pass->grid, pass->row - pass->scroll_row, column,
                &glyph, 1, 1, KM_STYLE_DEFAULT, error);
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
        codepoint_width = utf8proc_charwidth(codepoint);
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
            spaces = 8u - (pass->hard_column % 8u);
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
    return rows > 2 ? rows - 2 : 1;
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
    gutter = digits > SIZE_MAX - 2 ? SIZE_MAX : digits + 2;
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
    gutter_width = line_number_gutter(text, len, line_base, columns);
    text_columns = columns - gutter_width;
    pass.text = text;
    pass.len = len;
    pass.point = point.v - start.v;
    pass.columns = text_columns;
    pass.gutter_width = gutter_width;
    pass.line_number_base = line_base;
    pass.content_rows = content_rows;
    pass.region_active = km_buffer_mark_active(buffer);
    pass.region_start = mark.v < point.v ? mark.v - start.v : point.v - start.v;
    pass.region_end = mark.v < point.v ? point.v - start.v : mark.v - start.v;
    status = run_layout_pass(&pass, error);
    if (status != KM_OK) goto fail;
    if (pass.cursor_row < scroll_row) {
        scroll_row = pass.cursor_row;
    } else if (pass.cursor_row - scroll_row >= content_rows) {
        scroll_row = pass.cursor_row - content_rows + 1;
    }
    result.cursor_row = pass.cursor_row - scroll_row;
    result.cursor_column = pass.cursor_column;
    result.scroll_row = scroll_row;
    result.hard_line = line_base + pass.cursor_hard_line;
    result.hard_column = pass.cursor_hard_column;

    status = km_cell_grid_create(rows, columns, &grid, error);
    if (status != KM_OK) goto fail;
    pass = (KmLayoutPass){0};
    pass.text = text;
    pass.len = len;
    pass.point = point.v - start.v;
    pass.columns = text_columns;
    pass.gutter_width = gutter_width;
    pass.line_number_base = line_base;
    pass.scroll_row = scroll_row;
    pass.content_rows = content_rows;
    pass.grid = grid;
    pass.region_active = km_buffer_mark_active(buffer);
    pass.region_start = mark.v < point.v ? mark.v - start.v : point.v - start.v;
    pass.region_end = mark.v < point.v ? point.v - start.v : mark.v - start.v;
    status = run_layout_pass(&pass, error);
    if (status != KM_OK) goto fail;
    if (rows > 2) {
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
    gutter_width = line_number_gutter(text, len, line_base, columns);
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
        magnitude = content_rows > 2 ? (uint64_t)(content_rows - 2) : 1;
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
