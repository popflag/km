#include "editor.h"

#include <stdlib.h>

struct KmView {
    KmBuffer *buffer;
    KmAnchor *point;
    KmView *next;
    KmView **prev_next;
};

struct KmBuffer {
    KmDocument *document;
    KmBuffer *base;
    KmAnchor *begv;
    KmAnchor *zv;
    KmAnchor *saved_point;
    KmView *views;
    size_t view_count;
    size_t indirect_count;
    KmStateId saved_state;
    bool read_only;
};

enum {
    KM_COMMAND_INSERT_UTF8_BLOCK = 1,
    KM_COMMAND_DELETE_CHAR = 2,
    KM_COMMAND_DELETE_BACKWARD_CHAR = 3,
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

static bool in_accessible_range(const KmBuffer *buffer, KmBytePos point)
{
    return point.v >= km_anchor_get(buffer->begv).v &&
           point.v <= km_anchor_get(buffer->zv).v;
}

static KmStatus validate_view(KmView *view, bool mutable, KmBytePos *point,
                              KmBytePos *start, KmBytePos *end,
                              KmError *error)
{
    KmBuffer *buffer;

    km_error_clear(error);
    if (view == NULL || view->buffer == NULL || view->point == NULL) {
        return fail(error, KM_ERR_INVALID, "view command");
    }
    buffer = view->buffer;
    if (mutable && buffer->read_only) {
        return fail(error, KM_ERR_PERMISSION, "view command");
    }
    *point = km_anchor_get(view->point);
    *start = km_anchor_get(buffer->begv);
    *end = km_anchor_get(buffer->zv);
    if (start->v > end->v || !in_accessible_range(buffer, *point) ||
        !km_document_is_boundary(buffer->document, *point) ||
        !km_document_is_boundary(buffer->document, *start) ||
        !km_document_is_boundary(buffer->document, *end)) {
        return fail(error, KM_ERR_INVALID, "view command");
    }
    return KM_OK;
}

static KmStatus apply_view_splice(KmView *view, KmBytePos start, KmBytePos end,
                                  const uint8_t *insert, size_t insert_len,
                                  uint64_t command_id, KmError *error)
{
    KmBytePos point;
    KmBytePos begv;
    KmBytePos zv;
    KmSplice splice;
    KmTxnMeta meta;
    KmStatus status;

    status = validate_view(view, true, &point, &begv, &zv, error);
    if (status != KM_OK) return status;
    if (start.v > end.v || start.v < begv.v || end.v > zv.v ||
        !km_document_is_boundary(view->buffer->document, start) ||
        !km_document_is_boundary(view->buffer->document, end)) {
        return fail(error, KM_ERR_INVALID, "view command");
    }
    splice = (KmSplice){start, end, insert, insert_len, 0};
    meta = (KmTxnMeta){km_document_revision(view->buffer->document), command_id};
    return km_document_apply(view->buffer->document, &splice, 1, meta, error);
}

static KmStatus adjacent_boundary(const KmDocument *document, KmBytePos point,
                                  KmBytePos start, KmBytePos end,
                                  bool backward, KmBytePos *out)
{
    size_t candidate;

    if (backward) {
        if (point.v == start.v) return KM_ERR_INVALID;
        candidate = point.v - 1;
        while (candidate > start.v &&
               !km_document_is_boundary(document, (KmBytePos){candidate})) {
            --candidate;
        }
    } else {
        if (point.v == end.v) return KM_ERR_INVALID;
        candidate = point.v + 1;
        while (candidate < end.v &&
               !km_document_is_boundary(document, (KmBytePos){candidate})) {
            ++candidate;
        }
    }
    if (!km_document_is_boundary(document, (KmBytePos){candidate})) {
        return KM_ERR_INVALID;
    }
    *out = (KmBytePos){candidate};
    return KM_OK;
}

static void clamp_anchor(KmAnchor *anchor, KmBytePos start, KmBytePos end)
{
    KmBytePos point = km_anchor_get(anchor);

    if (point.v < start.v) {
        (void)km_anchor_set(anchor, start, NULL);
    } else if (point.v > end.v) {
        (void)km_anchor_set(anchor, end, NULL);
    }
}

static void clamp_points(KmBuffer *buffer, KmBytePos start, KmBytePos end)
{
    KmView *view;

    clamp_anchor(buffer->saved_point, start, end);
    for (view = buffer->views; view != NULL; view = view->next) {
        clamp_anchor(view->point, start, end);
    }
}

static void destroy_anchors(KmBuffer *buffer)
{
    km_anchor_destroy(buffer->saved_point);
    km_anchor_destroy(buffer->zv);
    km_anchor_destroy(buffer->begv);
}

static KmStatus create_buffer_anchors(KmBuffer *buffer, KmError *error)
{
    KmBytePos end = {km_document_len(buffer->document)};
    KmStatus status;

    status = km_anchor_create(buffer->document, (KmBytePos){0},
                              KM_ANCHOR_BEFORE, &buffer->begv, error);
    if (status != KM_OK) return status;
    status = km_anchor_create(buffer->document, end, KM_ANCHOR_AFTER,
                              &buffer->zv, error);
    if (status != KM_OK) {
        km_anchor_destroy(buffer->begv);
        buffer->begv = NULL;
        return status;
    }
    status = km_anchor_create(buffer->document, (KmBytePos){0},
                              KM_ANCHOR_AFTER, &buffer->saved_point, error);
    if (status != KM_OK) {
        km_anchor_destroy(buffer->zv);
        km_anchor_destroy(buffer->begv);
        buffer->zv = NULL;
        buffer->begv = NULL;
    }
    return status;
}

KmStatus km_buffer_create_base(const uint8_t *text, size_t len,
                               KmBuffer **out_buffer, KmError *error)
{
    KmBuffer *buffer;
    KmDocument *document = NULL;
    KmStatus status;

    km_error_clear(error);
    if (out_buffer == NULL) return fail(error, KM_ERR_INVALID, "buffer create");
    *out_buffer = NULL;
    status = km_document_create(text, len, &document, error);
    if (status != KM_OK) return status;
    buffer = (KmBuffer *)calloc(1, sizeof(*buffer));
    if (buffer == NULL) {
        km_document_destroy(document);
        return fail(error, KM_ERR_OOM, "buffer create");
    }
    buffer->document = document;
    status = create_buffer_anchors(buffer, error);
    if (status != KM_OK) {
        km_document_destroy(buffer->document);
        free(buffer);
        return status;
    }
    buffer->saved_state = km_document_history_state(buffer->document);
    *out_buffer = buffer;
    return KM_OK;
}

KmStatus km_buffer_create_indirect(KmBuffer *base, KmBuffer **out_buffer,
                                   KmError *error)
{
    KmBuffer *buffer;
    KmStatus status;

    km_error_clear(error);
    if (out_buffer == NULL) {
        return fail(error, KM_ERR_INVALID, "indirect buffer create");
    }
    *out_buffer = NULL;
    if (base == NULL || base->base != NULL ||
        base->indirect_count == SIZE_MAX) {
        return fail(error, KM_ERR_INVALID, "indirect buffer create");
    }
    buffer = (KmBuffer *)calloc(1, sizeof(*buffer));
    if (buffer == NULL) return fail(error, KM_ERR_OOM, "indirect buffer create");
    buffer->document = base->document;
    buffer->base = base;
    status = create_buffer_anchors(buffer, error);
    if (status != KM_OK) {
        free(buffer);
        return status;
    }
    ++base->indirect_count;
    *out_buffer = buffer;
    return KM_OK;
}

KmStatus km_buffer_destroy(KmBuffer *buffer, KmError *error)
{
    km_error_clear(error);
    if (buffer == NULL) return fail(error, KM_ERR_INVALID, "buffer destroy");
    if (buffer->view_count != 0 || buffer->indirect_count != 0) {
        return fail(error, KM_ERR_CONFLICT, "buffer destroy");
    }
    destroy_anchors(buffer);
    if (buffer->base != NULL) {
        --buffer->base->indirect_count;
    } else {
        km_document_destroy(buffer->document);
    }
    free(buffer);
    return KM_OK;
}

const KmDocument *km_buffer_document(const KmBuffer *buffer)
{
    return buffer == NULL ? NULL : buffer->document;
}

KmBytePos km_buffer_accessible_start(const KmBuffer *buffer)
{
    return buffer == NULL ? (KmBytePos){0} : km_anchor_get(buffer->begv);
}

KmBytePos km_buffer_accessible_end(const KmBuffer *buffer)
{
    return buffer == NULL ? (KmBytePos){0} : km_anchor_get(buffer->zv);
}

KmStatus km_buffer_narrow(KmBuffer *buffer, KmBytePos start, KmBytePos end,
                          KmError *error)
{
    km_error_clear(error);
    if (buffer == NULL || start.v > end.v ||
        !in_accessible_range(buffer, start) ||
        !in_accessible_range(buffer, end) ||
        !km_document_is_boundary(buffer->document, start) ||
        !km_document_is_boundary(buffer->document, end)) {
        return fail(error, KM_ERR_INVALID, "buffer narrow");
    }
    (void)km_anchor_set(buffer->begv, start, NULL);
    (void)km_anchor_set(buffer->zv, end, NULL);
    clamp_points(buffer, start, end);
    return KM_OK;
}

KmStatus km_buffer_widen(KmBuffer *buffer, KmError *error)
{
    KmBytePos end;

    km_error_clear(error);
    if (buffer == NULL) return fail(error, KM_ERR_INVALID, "buffer widen");
    end.v = km_document_len(buffer->document);
    (void)km_anchor_set(buffer->begv, (KmBytePos){0}, NULL);
    (void)km_anchor_set(buffer->zv, end, NULL);
    return KM_OK;
}

bool km_buffer_is_read_only(const KmBuffer *buffer)
{
    return buffer != NULL && buffer->read_only;
}

void km_buffer_set_read_only(KmBuffer *buffer, bool read_only)
{
    if (buffer != NULL) buffer->read_only = read_only;
}

bool km_buffer_is_modified(const KmBuffer *buffer)
{
    const KmBuffer *base;

    if (buffer == NULL) return false;
    base = buffer->base == NULL ? buffer : buffer->base;
    return km_document_history_state(base->document) != base->saved_state;
}

KmStatus km_view_create(KmBuffer *buffer, KmView **out_view, KmError *error)
{
    KmView *view;
    KmStatus status;

    km_error_clear(error);
    if (out_view == NULL) return fail(error, KM_ERR_INVALID, "view create");
    *out_view = NULL;
    if (buffer == NULL || buffer->view_count == SIZE_MAX) {
        return fail(error, KM_ERR_INVALID, "view create");
    }
    view = (KmView *)calloc(1, sizeof(*view));
    if (view == NULL) return fail(error, KM_ERR_OOM, "view create");
    status = km_anchor_create(buffer->document,
                              km_anchor_get(buffer->saved_point),
                              KM_ANCHOR_AFTER, &view->point, error);
    if (status != KM_OK) {
        free(view);
        return status;
    }
    view->buffer = buffer;
    view->next = buffer->views;
    view->prev_next = &buffer->views;
    if (view->next != NULL) view->next->prev_next = &view->next;
    buffer->views = view;
    ++buffer->view_count;
    *out_view = view;
    return KM_OK;
}

KmStatus km_view_destroy(KmView *view, KmError *error)
{
    KmBuffer *buffer;

    km_error_clear(error);
    if (view == NULL) return fail(error, KM_ERR_INVALID, "view destroy");
    buffer = view->buffer;
    if (buffer->view_count == 1) {
        (void)km_anchor_set(buffer->saved_point, km_anchor_get(view->point), NULL);
    }
    *view->prev_next = view->next;
    if (view->next != NULL) view->next->prev_next = view->prev_next;
    --buffer->view_count;
    km_anchor_destroy(view->point);
    free(view);
    return KM_OK;
}

KmBytePos km_view_point(const KmView *view)
{
    return view == NULL ? (KmBytePos){0} : km_anchor_get(view->point);
}

KmStatus km_view_set_point(KmView *view, KmBytePos point, KmError *error)
{
    km_error_clear(error);
    if (view == NULL || !in_accessible_range(view->buffer, point) ||
        !km_document_is_boundary(view->buffer->document, point)) {
        return fail(error, KM_ERR_INVALID, "view point");
    }
    return km_anchor_set(view->point, point, error);
}

KmStatus km_view_insert_utf8_block(KmView *view, const uint8_t *text,
                                   size_t len, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    return apply_view_splice(view, point, point, text, len,
                             KM_COMMAND_INSERT_UTF8_BLOCK, error);
}

KmStatus km_view_forward_char(KmView *view, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmBytePos next;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    status = adjacent_boundary(view->buffer->document, point, start, end,
                               false, &next);
    if (status != KM_OK) return fail(error, status, "forward char");
    return km_anchor_set(view->point, next, error);
}

KmStatus km_view_backward_char(KmView *view, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmBytePos previous;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    status = adjacent_boundary(view->buffer->document, point, start, end,
                               true, &previous);
    if (status != KM_OK) return fail(error, status, "backward char");
    return km_anchor_set(view->point, previous, error);
}

KmStatus km_view_delete_char(KmView *view, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmBytePos next;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    status = adjacent_boundary(view->buffer->document, point, start, end,
                               false, &next);
    if (status != KM_OK) return fail(error, status, "delete char");
    return apply_view_splice(view, point, next, NULL, 0,
                             KM_COMMAND_DELETE_CHAR, error);
}

KmStatus km_view_delete_backward_char(KmView *view, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmBytePos previous;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    status = adjacent_boundary(view->buffer->document, point, start, end,
                               true, &previous);
    if (status != KM_OK) return fail(error, status, "delete backward char");
    return apply_view_splice(view, previous, point, NULL, 0,
                             KM_COMMAND_DELETE_BACKWARD_CHAR, error);
}

KmStatus km_view_undo(KmView *view, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    if (!km_document_can_undo_in_range(view->buffer->document, start, end)) {
        return fail(error, KM_ERR_INVALID, "view undo");
    }
    return km_document_undo(view->buffer->document,
                            km_document_revision(view->buffer->document),
                            error);
}

KmStatus km_view_redo(KmView *view, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    if (!km_document_can_redo_in_range(view->buffer->document, start, end)) {
        return fail(error, KM_ERR_INVALID, "view redo");
    }
    return km_document_redo(view->buffer->document,
                            km_document_revision(view->buffer->document),
                            error);
}
