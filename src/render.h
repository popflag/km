#ifndef KM_RENDER_H
#define KM_RENDER_H

#include "base.h"

#include <stdbool.h>

typedef struct KmCellGrid KmCellGrid;

typedef struct {
    size_t glyph_off;
    size_t glyph_len;
    uint16_t style_id;
    uint8_t width;
    uint8_t flags;
} KmCell;

enum {
    KM_CELL_CONTINUATION = 1u << 0
};

enum {
    KM_STYLE_DEFAULT = 0,
    KM_STYLE_REGION
};

KmStatus km_cell_grid_create(size_t rows, size_t columns,
                             KmCellGrid **out_grid, KmError *error);
void km_cell_grid_destroy(KmCellGrid *grid);
size_t km_cell_grid_rows(const KmCellGrid *grid);
size_t km_cell_grid_columns(const KmCellGrid *grid);
const KmCell *km_cell_grid_cell(const KmCellGrid *grid, size_t row, size_t column);
const uint8_t *km_cell_grid_cell_glyph(const KmCellGrid *grid,
                                       size_t row, size_t column,
                                       size_t *length);
KmStatus km_cell_grid_put(KmCellGrid *grid, size_t row, size_t column,
                          const uint8_t *glyph, size_t glyph_len,
                          uint8_t width, uint16_t style_id, KmError *error);

KmStatus km_render_probe_grid_create(unsigned columns, unsigned rows,
                                     bool interactive, KmCellGrid **out_grid,
                                     KmError *error);
KmStatus km_cell_grid_encode_vt(const KmCellGrid *grid, bool clear_screen,
                                char **out_bytes, size_t *out_len,
                                KmError *error);
KmStatus km_cell_grid_encode_frame_vt(const KmCellGrid *grid,
                                      size_t cursor_row, size_t cursor_column,
                                      char **out_bytes, size_t *out_len,
                                      KmError *error);

#endif
