#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct KmCellGrid {
    KmCell *cells;
    uint8_t *glyphs;
    size_t rows;
    size_t columns;
    size_t glyph_len;
    size_t glyph_cap;
};

static KmStatus fail(KmError *error, KmStatus status, const char *operation)
{
    if (error != NULL) {
        error->code = status;
        error->os_code = 0;
        error->operation = operation;
    }
    return status;
}

static KmStatus reserve_glyphs(KmCellGrid *grid, size_t extra, KmError *error)
{
    size_t required;
    size_t capacity;
    uint8_t *glyphs;

    if (extra > SIZE_MAX - grid->glyph_len) {
        return fail(error, KM_ERR_OOM, "grow CellGrid glyph arena");
    }
    required = grid->glyph_len + extra;
    if (required <= grid->glyph_cap) return KM_OK;
    capacity = grid->glyph_cap == 0 ? 64 : grid->glyph_cap;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    glyphs = realloc(grid->glyphs, capacity);
    if (glyphs == NULL) return fail(error, KM_ERR_OOM, "grow CellGrid glyph arena");
    grid->glyphs = glyphs;
    grid->glyph_cap = capacity;
    return KM_OK;
}

KmStatus km_cell_grid_create(size_t rows, size_t columns,
                             KmCellGrid **out_grid, KmError *error)
{
    KmCellGrid *grid;

    if (out_grid == NULL || rows == 0 || columns == 0 || rows > SIZE_MAX / columns ||
        rows * columns > SIZE_MAX / sizeof(*grid->cells)) {
        return fail(error, KM_ERR_INVALID, "create CellGrid");
    }
    *out_grid = NULL;
    grid = calloc(1, sizeof(*grid));
    if (grid == NULL) return fail(error, KM_ERR_OOM, "create CellGrid");
    grid->cells = calloc(rows * columns, sizeof(*grid->cells));
    if (grid->cells == NULL) {
        free(grid);
        return fail(error, KM_ERR_OOM, "create CellGrid cells");
    }
    grid->rows = rows;
    grid->columns = columns;
    *out_grid = grid;
    km_error_clear(error);
    return KM_OK;
}

void km_cell_grid_destroy(KmCellGrid *grid)
{
    if (grid == NULL) return;
    free(grid->glyphs);
    free(grid->cells);
    free(grid);
}

size_t km_cell_grid_rows(const KmCellGrid *grid)
{
    return grid == NULL ? 0 : grid->rows;
}

size_t km_cell_grid_columns(const KmCellGrid *grid)
{
    return grid == NULL ? 0 : grid->columns;
}

const KmCell *km_cell_grid_cell(const KmCellGrid *grid, size_t row, size_t column)
{
    if (grid == NULL || row >= grid->rows || column >= grid->columns) return NULL;
    return &grid->cells[row * grid->columns + column];
}

const uint8_t *km_cell_grid_cell_glyph(const KmCellGrid *grid,
                                       size_t row, size_t column,
                                       size_t *length)
{
    const KmCell *cell = km_cell_grid_cell(grid, row, column);

    if (length != NULL) *length = 0;
    if (cell == NULL || cell->glyph_len == 0 || cell->glyph_len > grid->glyph_len ||
        cell->glyph_off > grid->glyph_len - cell->glyph_len) {
        return NULL;
    }
    if (length != NULL) *length = cell->glyph_len;
    return grid->glyphs + cell->glyph_off;
}

static void clear_replaced_cell(KmCellGrid *grid, size_t index)
{
    KmCell *cell = &grid->cells[index];
    size_t column = index % grid->columns;

    if ((cell->flags & KM_CELL_CONTINUATION) != 0) {
        if (column != 0) memset(cell - 1, 0, sizeof(*cell));
    } else if (cell->width == 2 && column + 1 < grid->columns) {
        memset(cell + 1, 0, sizeof(*cell));
    }
    memset(cell, 0, sizeof(*cell));
}

KmStatus km_cell_grid_put(KmCellGrid *grid, size_t row, size_t column,
                          const uint8_t *glyph, size_t glyph_len,
                          uint8_t width, uint16_t style_id, KmError *error)
{
    KmCell *lead;
    size_t index;
    bool replaced_wide;
    KmStatus status;

    if (grid == NULL || glyph == NULL || glyph_len == 0 || (width != 1 && width != 2) ||
        row >= grid->rows || column >= grid->columns || width > grid->columns - column) {
        return fail(error, KM_ERR_INVALID, "write CellGrid cell");
    }
    index = row * grid->columns + column;
    replaced_wide = grid->cells[index].width == 2 && width == 1;
    if (replaced_wide && glyph_len == SIZE_MAX) {
        return fail(error, KM_ERR_OOM, "grow CellGrid glyph arena");
    }
    status = reserve_glyphs(grid, glyph_len + (replaced_wide ? 1 : 0), error);
    if (status != KM_OK) return status;
    clear_replaced_cell(grid, index);
    if (width == 2) clear_replaced_cell(grid, index + 1);
    lead = &grid->cells[index];
    lead->glyph_off = grid->glyph_len;
    lead->glyph_len = glyph_len;
    lead->style_id = style_id;
    lead->width = width;
    lead->flags = 0;
    memcpy(grid->glyphs + grid->glyph_len, glyph, glyph_len);
    grid->glyph_len += glyph_len;
    if (replaced_wide) {
        KmCell *blank = &grid->cells[index + 1];

        blank->glyph_off = grid->glyph_len;
        blank->glyph_len = 1;
        blank->style_id = style_id;
        blank->width = 1;
        grid->glyphs[grid->glyph_len++] = ' ';
    }
    if (width == 2) {
        KmCell *continuation = &grid->cells[index + 1];
        memset(continuation, 0, sizeof(*continuation));
        continuation->style_id = style_id;
        continuation->flags = KM_CELL_CONTINUATION;
    }
    km_error_clear(error);
    return KM_OK;
}

static KmStatus put_ascii(KmCellGrid *grid, size_t row, size_t *column,
                          const char *text, KmError *error)
{
    size_t length = strlen(text);

    if (length > km_cell_grid_columns(grid) - *column) {
        return fail(error, KM_ERR_INVALID, "write CellGrid probe text");
    }
    for (size_t i = 0; i < length; ++i) {
        KmStatus status = km_cell_grid_put(grid, row, *column,
                                           (const uint8_t *)&text[i], 1, 1, 0, error);
        if (status != KM_OK) return status;
        ++*column;
    }
    return KM_OK;
}

KmStatus km_render_probe_grid_create(unsigned columns, unsigned rows,
                                     bool interactive, KmCellGrid **out_grid,
                                     KmError *error)
{
    KmCellGrid *grid;
    size_t row_count = interactive ? 6u : 4u;
    size_t column_count = columns < 80u ? 80u : columns;
    size_t column = 0;
    char size_line[64];
    int size_length;
    KmStatus status;

    (void)rows;
    if (out_grid == NULL) return fail(error, KM_ERR_INVALID, "create probe grid");
    *out_grid = NULL;
    status = km_cell_grid_create(row_count, column_count, &grid, error);
    if (status != KM_OK) return status;
    status = put_ascii(grid, 0, &column, "km terminal probe", error);
    if (status == KM_OK) {
        column = 0;
        if (interactive) {
            size_length = snprintf(size_line, sizeof(size_line), "size: %ux%u", columns, rows);
            if (size_length < 0 || (size_t)size_length >= sizeof(size_line)) {
                status = fail(error, KM_ERR_INVALID, "format probe size");
            } else {
                status = put_ascii(grid, 1, &column, size_line, error);
            }
        }
    }
    if (status == KM_OK) {
        size_t row = interactive ? 2u : 1u;
        column = 0;
        status = put_ascii(grid, row, &column, "ASCII: abc XYZ 123", error);
    }
    if (status == KM_OK) {
        size_t row = interactive ? 3u : 2u;
        column = 0;
        status = put_ascii(grid, row, &column, "Combining: ", error);
        if (status == KM_OK) {
            static const uint8_t combining[] = {'e', 0xCC, 0x81};
            status = km_cell_grid_put(grid, row, column, combining,
                                      sizeof(combining), 1, 0, error);
        }
    }
    if (status == KM_OK) {
        size_t row = interactive ? 4u : 3u;
        static const uint8_t cjk[] = {0xE6, 0x96, 0x87, 0xE6, 0x9C, 0xAC};
        column = 0;
        status = put_ascii(grid, row, &column, "CJK: ", error);
        if (status == KM_OK) {
            status = km_cell_grid_put(grid, row, column, cjk, 3, 2, 0, error);
            column += 2;
        }
        if (status == KM_OK) {
            status = km_cell_grid_put(grid, row, column, cjk + 3, 3, 2, 0, error);
        }
    }
    if (status == KM_OK && interactive) {
        column = 0;
        status = put_ascii(grid, 5, &column, "Press q or C-g to exit", error);
    }
    if (status != KM_OK) {
        km_cell_grid_destroy(grid);
        return status;
    }
    *out_grid = grid;
    return KM_OK;
}

static KmStatus append_bytes(char **bytes, size_t *length, size_t *capacity,
                             const char *source, size_t source_len, KmError *error)
{
    size_t required;
    size_t new_capacity;
    char *new_bytes;

    if (source_len > SIZE_MAX - *length - 1) {
        return fail(error, KM_ERR_OOM, "encode CellGrid VT output");
    }
    required = *length + source_len + 1;
    if (required > *capacity) {
        new_capacity = *capacity == 0 ? 128 : *capacity;
        while (new_capacity < required) {
            if (new_capacity > SIZE_MAX / 2) {
                new_capacity = required;
                break;
            }
            new_capacity *= 2;
        }
        new_bytes = realloc(*bytes, new_capacity);
        if (new_bytes == NULL) return fail(error, KM_ERR_OOM, "encode CellGrid VT output");
        *bytes = new_bytes;
        *capacity = new_capacity;
    }
    memcpy(*bytes + *length, source, source_len);
    *length += source_len;
    (*bytes)[*length] = '\0';
    return KM_OK;
}

KmStatus km_cell_grid_encode_vt(const KmCellGrid *grid, bool clear_screen,
                                char **out_bytes, size_t *out_len,
                                KmError *error)
{
    char *bytes = NULL;
    size_t length = 0;
    size_t capacity = 0;
    size_t last_row = 0;
    KmStatus status;

    if (grid == NULL || out_bytes == NULL || out_len == NULL) {
        return fail(error, KM_ERR_INVALID, "encode CellGrid VT output");
    }
    *out_bytes = NULL;
    *out_len = 0;
    for (size_t row = 0; row < grid->rows; ++row) {
        for (size_t column = 0; column < grid->columns; ++column) {
            if (grid->cells[row * grid->columns + column].glyph_len != 0) {
                last_row = row;
            }
        }
    }
    if (clear_screen) {
        status = append_bytes(&bytes, &length, &capacity, "\x1b[2J\x1b[H", 7, error);
        if (status != KM_OK) goto fail;
    }
    for (size_t row = 0; row <= last_row; ++row) {
        size_t last_column = 0;
        for (size_t column = 0; column < grid->columns; ++column) {
            if (grid->cells[row * grid->columns + column].glyph_len != 0) {
                last_column = column;
            }
        }
        for (size_t column = 0; column <= last_column; ++column) {
            const KmCell *cell = &grid->cells[row * grid->columns + column];
            if ((cell->flags & KM_CELL_CONTINUATION) != 0) continue;
            if (cell->glyph_len == 0) {
                status = append_bytes(&bytes, &length, &capacity, " ", 1, error);
            } else {
                status = append_bytes(&bytes, &length, &capacity,
                                      (const char *)grid->glyphs + cell->glyph_off,
                                      cell->glyph_len, error);
            }
            if (status != KM_OK) goto fail;
        }
        status = append_bytes(&bytes, &length, &capacity,
                              clear_screen ? "\r\n" : "\n",
                              clear_screen ? 2 : 1, error);
        if (status != KM_OK) goto fail;
    }
    *out_bytes = bytes;
    *out_len = length;
    km_error_clear(error);
    return KM_OK;

fail:
    free(bytes);
    return status;
}

KmStatus km_cell_grid_encode_frame_vt(const KmCellGrid *grid,
                                      size_t cursor_row, size_t cursor_column,
                                      char **out_bytes, size_t *out_len,
                                      KmError *error)
{
    char *bytes = NULL;
    size_t length = 0;
    size_t capacity = 0;
    char position[64];
    int position_len;
    KmStatus status;
    size_t row;

    if (grid == NULL || out_bytes == NULL || out_len == NULL ||
        cursor_row >= grid->rows || cursor_column >= grid->columns) {
        return fail(error, KM_ERR_INVALID, "encode CellGrid frame");
    }
    *out_bytes = NULL;
    *out_len = 0;
    status = append_bytes(&bytes, &length, &capacity,
                          "\x1b[?25l\x1b[2J", 10, error);
    if (status != KM_OK) goto fail;
    for (row = 0; row < grid->rows; ++row) {
        size_t column;
        size_t last_column = 0;
        bool populated = false;

        for (column = 0; column < grid->columns; ++column) {
            if (grid->cells[row * grid->columns + column].glyph_len != 0) {
                last_column = column;
                populated = true;
            }
        }
        if (!populated) continue;
        position_len = snprintf(position, sizeof(position), "\x1b[%llu;1H",
                                (unsigned long long)row + 1u);
        if (position_len < 0 || (size_t)position_len >= sizeof(position)) {
            status = fail(error, KM_ERR_INVALID, "format CellGrid row");
            goto fail;
        }
        status = append_bytes(&bytes, &length, &capacity, position,
                              (size_t)position_len, error);
        if (status != KM_OK) goto fail;
        for (column = 0; column <= last_column; ++column) {
            const KmCell *cell = &grid->cells[row * grid->columns + column];
            if ((cell->flags & KM_CELL_CONTINUATION) != 0) continue;
            status = cell->glyph_len == 0
                         ? append_bytes(&bytes, &length, &capacity, " ", 1, error)
                         : append_bytes(
                               &bytes, &length, &capacity,
                               (const char *)grid->glyphs + cell->glyph_off,
                               cell->glyph_len, error);
            if (status != KM_OK) goto fail;
        }
    }
    position_len = snprintf(position, sizeof(position), "\x1b[%llu;%lluH\x1b[?25h",
                            (unsigned long long)cursor_row + 1u,
                            (unsigned long long)cursor_column + 1u);
    if (position_len < 0 || (size_t)position_len >= sizeof(position)) {
        status = fail(error, KM_ERR_INVALID, "format CellGrid cursor");
        goto fail;
    }
    status = append_bytes(&bytes, &length, &capacity, position,
                          (size_t)position_len, error);
    if (status != KM_OK) goto fail;
    *out_bytes = bytes;
    *out_len = length;
    km_error_clear(error);
    return KM_OK;

fail:
    free(bytes);
    return status;
}
