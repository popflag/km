#include "layout.h"

#include "utf8proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *text;
    size_t len;
    size_t point;
    size_t columns;
    size_t row;
    size_t column;
    size_t hard_line;
    size_t hard_column;
    size_t cursor_row;
    size_t cursor_column;
    size_t cursor_hard_line;
    size_t cursor_hard_column;
    size_t scroll_row;
    size_t content_rows;
    KmCellGrid *grid;
    bool cursor_set;
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
    if (offset == pass->point) {
        normalize_cursor(pass, &row, &column);
        pass->cursor_row = row;
        pass->cursor_column = column;
        pass->cursor_hard_line = pass->hard_line;
        pass->cursor_hard_column = pass->hard_column;
        pass->cursor_set = true;
    }
}

static void wrap_before(KmLayoutPass *pass, size_t width)
{
    if (pass->column == pass->columns ||
        width > pass->columns - pass->column) {
        ++pass->row;
        pass->column = 0;
    }
}

static KmStatus draw_glyph(KmLayoutPass *pass, const uint8_t *glyph,
                           size_t glyph_len, uint8_t width, KmError *error)
{
    if (pass->grid != NULL && pass->row >= pass->scroll_row &&
        pass->row - pass->scroll_row < pass->content_rows) {
        return km_cell_grid_put(pass->grid, pass->row - pass->scroll_row,
                                pass->column, glyph, glyph_len, width, 0, error);
    }
    return KM_OK;
}

static KmStatus draw_ascii_cell(KmLayoutPass *pass, uint8_t glyph,
                                KmError *error)
{
    KmStatus status;

    wrap_before(pass, 1);
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

static KmStatus decode_one(const uint8_t *text, size_t len, size_t offset,
                           utf8proc_int32_t *codepoint, size_t *consumed,
                           KmError *error)
{
    utf8proc_ssize_t count = utf8proc_iterate(
        text + offset, (utf8proc_ssize_t)(len - offset), codepoint);

    if (count <= 0) return fail(error, KM_ERR_INVALID, "layout UTF-8");
    *consumed = (size_t)count;
    return KM_OK;
}

static KmStatus next_grapheme(const uint8_t *text, size_t len, size_t start,
                              size_t *out_end, uint8_t *out_width,
                              bool *out_combining_only,
                              KmError *error)
{
    utf8proc_int32_t previous;
    utf8proc_int32_t state = 0;
    size_t consumed;
    size_t end;
    size_t scan;
    int width = 0;
    bool force_wide = false;
    bool force_narrow = false;
    size_t regional_indicators = 0;
    KmStatus status = decode_one(text, len, start, &previous, &consumed, error);

    if (status != KM_OK) return status;
    end = start + consumed;
    while (end < len) {
        utf8proc_int32_t next;
        status = decode_one(text, len, end, &next, &consumed, error);
        if (status != KM_OK) return status;
        if (utf8proc_grapheme_break_stateful(previous, next, &state)) break;
        previous = next;
        end += consumed;
    }
    scan = start;
    while (scan < end) {
        utf8proc_int32_t codepoint;
        int codepoint_width;
        status = decode_one(text, end, scan, &codepoint, &consumed, error);
        if (status != KM_OK) return status;
        codepoint_width = utf8proc_charwidth(codepoint);
        if (codepoint_width > width) width = codepoint_width;
        if (codepoint == 0x200d || codepoint == 0xfe0f) {
            force_wide = true;
        }
        if (codepoint >= 0x1f1e6 && codepoint <= 0x1f1ff) {
            ++regional_indicators;
        }
        if (codepoint == 0xfe0e) force_narrow = true;
        scan += consumed;
    }
    *out_combining_only = width < 1;
    if (force_narrow) {
        width = 1;
    } else if (force_wide || regional_indicators >= 2) {
        width = 2;
    } else if (width < 1) {
        width = 1;
    } else if (width > 2) {
        width = 2;
    }
    *out_end = end;
    *out_width = (uint8_t)width;
    return KM_OK;
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
        KmStatus status = decode_one(pass->text, end, scan, &codepoint,
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
            pass->cursor_column = column;
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

    record_cursor(pass, 0, 0, 0);
    while (offset < pass->len) {
        utf8proc_int32_t codepoint;
        size_t consumed;
        KmStatus status = decode_one(pass->text, pass->len, offset,
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
            record_cursor(pass, offset, pass->row, pass->column);
        } else if (codepoint == '\t') {
            size_t spaces;
            record_cursor(pass, offset, pass->row, pass->column);
            spaces = 8u - (pass->hard_column % 8u);
            while (spaces-- != 0) {
                status = draw_ascii_cell(pass, ' ', error);
                if (status != KM_OK) return status;
            }
            offset += consumed;
            record_cursor(pass, offset, pass->row, pass->column);
        } else if (is_ascii_control(codepoint)) {
            record_cursor(pass, offset, pass->row, pass->column);
            status = draw_control(pass, codepoint, error);
            if (status != KM_OK) return status;
            offset += consumed;
            record_cursor(pass, offset, pass->row, pass->column);
        } else {
            size_t end;
            uint8_t width;
            const uint8_t *glyph;
            size_t glyph_len;
            bool too_wide;
            bool combining_only;
            uint8_t *combined = NULL;

            status = next_grapheme(pass->text, pass->len, offset,
                                   &end, &width, &combining_only, error);
            if (status != KM_OK) return status;
            too_wide = width > pass->columns;
            if (too_wide) width = 1;
            wrap_before(pass, width);
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

static KmStatus put_status(KmCellGrid *grid, size_t row, size_t columns,
                           const KmLayoutResult *result, bool modified,
                           const char *message, KmError *error)
{
    char status_text[256];
    int length = snprintf(status_text, sizeof(status_text),
                          "%s *scratch*  L%llu C%llu%s%s",
                          modified ? "**" : "--",
                          (unsigned long long)result->hard_line + 1u,
                          (unsigned long long)result->hard_column + 1u,
                          message == NULL || message[0] == '\0' ? "" : "  ",
                          message == NULL ? "" : message);
    size_t count;
    size_t column;

    if (length < 0) return fail(error, KM_ERR_INVALID, "format status line");
    count = (size_t)length;
    if (count > sizeof(status_text) - 1) count = sizeof(status_text) - 1;
    if (count > columns) count = columns;
    for (column = 0; column < count; ++column) {
        KmStatus status = km_cell_grid_put(
            grid, row, column, (const uint8_t *)&status_text[column], 1, 1, 0,
            error);
        if (status != KM_OK) return status;
    }
    return KM_OK;
}

KmStatus km_layout_view(const KmBuffer *buffer, const KmView *view,
                        size_t rows, size_t columns, size_t scroll_row,
                        const char *message, KmCellGrid **out_grid,
                        KmLayoutResult *out_result, KmError *error)
{
    const KmDocument *document;
    KmBytePos start;
    KmBytePos end;
    KmBytePos point;
    uint8_t *text = NULL;
    size_t len;
    size_t content_rows;
    KmCellGrid *grid = NULL;
    KmLayoutPass pass = {0};
    KmLayoutResult result = {0};
    KmStatus status;

    km_error_clear(error);
    if (out_grid == NULL || out_result == NULL || buffer == NULL ||
        view == NULL || rows == 0 || columns == 0) {
        return fail(error, KM_ERR_INVALID, "layout view");
    }
    *out_grid = NULL;
    *out_result = (KmLayoutResult){0};
    document = km_buffer_document(buffer);
    start = km_buffer_accessible_start(buffer);
    end = km_buffer_accessible_end(buffer);
    point = km_view_point(view);
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
    content_rows = rows > 1 ? rows - 1 : 1;
    pass.text = text;
    pass.len = len;
    pass.point = point.v - start.v;
    pass.columns = columns;
    pass.content_rows = content_rows;
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
    result.hard_line = pass.cursor_hard_line;
    result.hard_column = pass.cursor_hard_column;

    status = km_cell_grid_create(rows, columns, &grid, error);
    if (status != KM_OK) goto fail;
    pass = (KmLayoutPass){0};
    pass.text = text;
    pass.len = len;
    pass.point = point.v - start.v;
    pass.columns = columns;
    pass.scroll_row = scroll_row;
    pass.content_rows = content_rows;
    pass.grid = grid;
    status = run_layout_pass(&pass, error);
    if (status != KM_OK) goto fail;
    if (rows > 1) {
        status = put_status(grid, rows - 1, columns, &result,
                            km_buffer_is_modified(buffer), message, error);
        if (status != KM_OK) goto fail;
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
