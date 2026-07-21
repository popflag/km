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
                        const char *message, const char *completion,
                        bool prompt_active,
                        KmCellGrid **out_grid,
                        KmLayoutResult *out_result, KmError *error);
KmStatus km_layout_scroll_view(const KmBuffer *buffer, KmView *view,
                               size_t rows, size_t columns,
                               size_t scroll_row, bool forward,
                               bool has_argument, int64_t argument,
                               size_t *out_scroll_row, KmError *error);

#endif
