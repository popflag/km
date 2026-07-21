#ifndef KM_LAYOUT_H
#define KM_LAYOUT_H

#include "editor.h"
#include "render.h"

typedef struct {
    size_t cursor_row;
    size_t cursor_column;
    size_t scroll_row;
    size_t hard_line;
    size_t hard_column;
} KmLayoutResult;

KmStatus km_layout_view(const KmBuffer *buffer, const KmView *view,
                        size_t rows, size_t columns, size_t scroll_row,
                        const char *message, bool prompt_active,
                        KmCellGrid **out_grid,
                        KmLayoutResult *out_result, KmError *error);

#endif
