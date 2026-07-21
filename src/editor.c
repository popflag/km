#include "editor.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

typedef enum {
    KM_PREFIX_NONE,
    KM_PREFIX_UNIVERSAL,
    KM_PREFIX_NUMERIC
} KmPrefixKind;

struct KmCommandLoop {
    size_t key_node;
    KmPrefixKind prefix_kind;
    uint64_t prefix_magnitude;
    bool prefix_negative;
    bool prefix_has_digits;
    KmCommandId last_command;
    bool quit_requested;
    bool exit_requested;
};

typedef KmStatus (*KmCommandCallback)(KmView *view, int64_t argument,
                                      KmError *error);

typedef struct {
    KmCommandId id;
    KmCommandCallback callback;
} KmCommandSpec;

typedef struct {
    uint32_t codepoint;
    uint32_t modifiers;
    int command;
    size_t child;
    size_t sibling;
} KmKeyNode;

enum {
    KM_INTERNAL_UNIVERSAL_ARGUMENT = 100,
    KM_INTERNAL_EXIT
};

#define KM_NO_KEY_NODE SIZE_MAX

static const KmKeyNode key_trie[] = {
    {0, 0, KM_COMMAND_NONE, 1, KM_NO_KEY_NODE},
    {'b', KM_MOD_CTRL, KM_COMMAND_BACKWARD_CHAR, KM_NO_KEY_NODE, 2},
    {'d', KM_MOD_CTRL, KM_COMMAND_DELETE_CHAR, KM_NO_KEY_NODE, 3},
    {'f', KM_MOD_CTRL, KM_COMMAND_FORWARD_CHAR, KM_NO_KEY_NODE, 4},
    {0x7f, 0, KM_COMMAND_DELETE_BACKWARD_CHAR, KM_NO_KEY_NODE, 5},
    {'/', KM_MOD_CTRL, KM_COMMAND_UNDO, KM_NO_KEY_NODE, 6},
    {'x', KM_MOD_CTRL, KM_COMMAND_NONE, 8, 7},
    {'u', KM_MOD_CTRL, KM_INTERNAL_UNIVERSAL_ARGUMENT,
     KM_NO_KEY_NODE, KM_NO_KEY_NODE},
    {'u', 0, KM_COMMAND_UNDO, KM_NO_KEY_NODE, 9},
    {'r', 0, KM_COMMAND_REDO, KM_NO_KEY_NODE, 10},
    {'c', KM_MOD_CTRL, KM_INTERNAL_EXIT, KM_NO_KEY_NODE, KM_NO_KEY_NODE},
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

static uint64_t command_magnitude(int64_t argument)
{
    return argument < 0 ? (uint64_t)(-argument) : (uint64_t)argument;
}

static KmStatus move_chars(KmView *view, int64_t argument, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmBytePos next;
    uint64_t remaining = command_magnitude(argument);
    bool backward = argument < 0;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    while (remaining != 0) {
        status = adjacent_boundary(view->buffer->document, point, start, end,
                                   backward, &next);
        if (status != KM_OK) {
            return fail(error, status, "move characters");
        }
        point = next;
        --remaining;
    }
    return km_anchor_set(view->point, point, error);
}

static KmStatus delete_chars(KmView *view, int64_t argument,
                             uint64_t command_id, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmBytePos boundary;
    KmBytePos delete_start;
    KmBytePos delete_end;
    uint64_t remaining = command_magnitude(argument);
    bool backward = argument < 0;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    boundary = point;
    while (remaining != 0) {
        KmBytePos next;
        status = adjacent_boundary(view->buffer->document, boundary, start, end,
                                   backward, &next);
        if (status != KM_OK) {
            return fail(error, status, "delete characters");
        }
        boundary = next;
        --remaining;
    }
    delete_start = backward ? boundary : point;
    delete_end = backward ? point : boundary;
    return apply_view_splice(view, delete_start, delete_end, NULL, 0,
                             command_id, error);
}

static KmStatus insert_utf8_repeated(KmView *view, const uint8_t *text,
                                     size_t len, int64_t argument,
                                     uint64_t command_id, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    uint8_t *repeated = NULL;
    const uint8_t *insert = text;
    size_t insert_len = len;
    size_t offset;
    uint64_t repeats;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    if (argument < 0 || (len != 0 && text == NULL)) {
        return fail(error, KM_ERR_INVALID, "insert characters");
    }
    repeats = (uint64_t)argument;
    if (len != 0 &&
        repeats > (uint64_t)((size_t)PTRDIFF_MAX / len)) {
        return fail(error, KM_ERR_INVALID, "insert characters");
    }
    insert_len = (size_t)repeats * len;
    if (repeats > 1 && insert_len != 0) {
        repeated = (uint8_t *)malloc(insert_len);
        if (repeated == NULL) {
            return fail(error, KM_ERR_OOM, "insert characters");
        }
        for (offset = 0; offset < insert_len; offset += len) {
            memcpy(repeated + offset, text, len);
        }
        insert = repeated;
    }
    status = apply_view_splice(view, point, point, insert, insert_len,
                               command_id, error);
    free(repeated);
    return status;
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
    return insert_utf8_repeated(view, text, len, 1,
                                KM_COMMAND_INSERT_UTF8_BLOCK, error);
}

KmStatus km_view_forward_char(KmView *view, KmError *error)
{
    return move_chars(view, 1, error);
}

KmStatus km_view_backward_char(KmView *view, KmError *error)
{
    return move_chars(view, -1, error);
}

KmStatus km_view_delete_char(KmView *view, KmError *error)
{
    return delete_chars(view, 1, KM_COMMAND_DELETE_CHAR, error);
}

KmStatus km_view_delete_backward_char(KmView *view, KmError *error)
{
    return delete_chars(view, -1, KM_COMMAND_DELETE_BACKWARD_CHAR, error);
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

static KmStatus command_forward(KmView *view, int64_t argument,
                                KmError *error)
{
    return move_chars(view, argument, error);
}

static KmStatus command_backward(KmView *view, int64_t argument,
                                 KmError *error)
{
    return move_chars(view, -argument, error);
}

static KmStatus command_delete(KmView *view, int64_t argument,
                               KmError *error)
{
    return delete_chars(view, argument, KM_COMMAND_DELETE_CHAR, error);
}

static KmStatus command_delete_backward(KmView *view, int64_t argument,
                                        KmError *error)
{
    return delete_chars(view, -argument,
                        KM_COMMAND_DELETE_BACKWARD_CHAR, error);
}

static KmStatus command_undo(KmView *view, int64_t argument, KmError *error)
{
    if (argument != 1) {
        return fail(error, KM_ERR_INVALID, "undo argument");
    }
    return km_view_undo(view, error);
}

static KmStatus command_redo(KmView *view, int64_t argument, KmError *error)
{
    if (argument != 1) {
        return fail(error, KM_ERR_INVALID, "redo argument");
    }
    return km_view_redo(view, error);
}

static const KmCommandSpec command_registry[] = {
    {KM_COMMAND_FORWARD_CHAR, command_forward},
    {KM_COMMAND_BACKWARD_CHAR, command_backward},
    {KM_COMMAND_DELETE_CHAR, command_delete},
    {KM_COMMAND_DELETE_BACKWARD_CHAR, command_delete_backward},
    {KM_COMMAND_UNDO, command_undo},
    {KM_COMMAND_REDO, command_redo},
};

static void reset_command_input(KmCommandLoop *loop)
{
    loop->key_node = 0;
    loop->prefix_kind = KM_PREFIX_NONE;
    loop->prefix_magnitude = 0;
    loop->prefix_negative = false;
    loop->prefix_has_digits = false;
}

static bool valid_scalar(uint32_t codepoint)
{
    return codepoint <= 0x10ffffu &&
           !(codepoint >= 0xd800u && codepoint <= 0xdfffu);
}

static KmStatus validate_event(const KmEvent *event, KmError *error)
{
    const uint32_t known_modifiers = KM_MOD_CTRL | KM_MOD_ALT | KM_MOD_SHIFT;

    if (event == NULL || event->kind < KM_EVENT_KEY ||
        event->kind > KM_EVENT_EOF ||
        (event->modifiers & ~known_modifiers) != 0) {
        return fail(error, KM_ERR_INVALID, "command event");
    }
    if (event->kind == KM_EVENT_KEY) {
        if (event->repeat == 0 || !valid_scalar(event->codepoint) ||
            event->text != NULL || event->text_len != 0) {
            return fail(error, KM_ERR_INVALID, "key event");
        }
    } else if (event->kind == KM_EVENT_TEXT) {
        bool has_block = event->text != NULL || event->text_len != 0;
        if (event->repeat == 0 || event->modifiers != 0 ||
            (has_block && (event->codepoint != 0 || event->text == NULL)) ||
            (!has_block && !valid_scalar(event->codepoint))) {
            return fail(error, KM_ERR_INVALID, "text event");
        }
    } else if (event->kind == KM_EVENT_PASTE) {
        if (event->repeat != 1 || event->modifiers != 0 ||
            event->codepoint != 0 ||
            (event->text_len != 0 && event->text == NULL)) {
            return fail(error, KM_ERR_INVALID, "paste event");
        }
    }
    return KM_OK;
}

static size_t find_key_child(size_t parent, uint32_t codepoint,
                             uint32_t modifiers)
{
    size_t node = key_trie[parent].child;

    while (node != KM_NO_KEY_NODE) {
        if (key_trie[node].codepoint == codepoint &&
            key_trie[node].modifiers == modifiers) {
            return node;
        }
        node = key_trie[node].sibling;
    }
    return KM_NO_KEY_NODE;
}

static const KmCommandSpec *find_command(KmCommandId id)
{
    size_t i;

    for (i = 0; i < sizeof(command_registry) / sizeof(command_registry[0]); ++i) {
        if (command_registry[i].id == id) return &command_registry[i];
    }
    return NULL;
}

static bool is_quit_key(uint32_t codepoint, uint32_t modifiers)
{
    return (codepoint == 'g' || codepoint == 'G') &&
           (modifiers & KM_MOD_CTRL) != 0 &&
           (modifiers & KM_MOD_ALT) == 0;
}

static KmStatus append_prefix_digit(KmCommandLoop *loop, uint32_t digit,
                                    KmError *error)
{
    if (loop->prefix_kind != KM_PREFIX_NUMERIC) {
        loop->prefix_kind = KM_PREFIX_NUMERIC;
        loop->prefix_magnitude = 0;
        loop->prefix_negative = false;
        loop->prefix_has_digits = false;
    }
    if (loop->prefix_magnitude >
        ((uint64_t)INT64_MAX - digit) / 10u) {
        reset_command_input(loop);
        return fail(error, KM_ERR_INVALID, "prefix argument");
    }
    loop->prefix_magnitude = loop->prefix_magnitude * 10u + digit;
    loop->prefix_has_digits = true;
    return KM_OK;
}

static KmStatus begin_negative_prefix(KmCommandLoop *loop, KmError *error)
{
    if (loop->prefix_kind == KM_PREFIX_NUMERIC) {
        reset_command_input(loop);
        return fail(error, KM_ERR_INVALID, "prefix argument");
    }
    loop->prefix_kind = KM_PREFIX_NUMERIC;
    loop->prefix_magnitude = 0;
    loop->prefix_negative = true;
    loop->prefix_has_digits = false;
    return KM_OK;
}

static int64_t current_argument(const KmCommandLoop *loop)
{
    uint64_t magnitude;

    if (loop->prefix_kind == KM_PREFIX_NONE) return 1;
    magnitude = loop->prefix_kind == KM_PREFIX_UNIVERSAL
                    ? loop->prefix_magnitude
                    : (loop->prefix_has_digits ? loop->prefix_magnitude : 1);
    return loop->prefix_negative ? -(int64_t)magnitude : (int64_t)magnitude;
}

static KmStatus universal_argument(KmCommandLoop *loop, KmError *error)
{
    loop->key_node = 0;
    if (loop->prefix_kind == KM_PREFIX_NONE) {
        loop->prefix_kind = KM_PREFIX_UNIVERSAL;
        loop->prefix_magnitude = 4;
        return KM_OK;
    }
    if (loop->prefix_kind != KM_PREFIX_UNIVERSAL ||
        loop->prefix_magnitude > (uint64_t)INT64_MAX / 4u) {
        reset_command_input(loop);
        return fail(error, KM_ERR_INVALID, "prefix argument");
    }
    loop->prefix_magnitude *= 4u;
    return KM_OK;
}

static KmStatus execute_registered_command(KmCommandLoop *loop, KmView *view,
                                           KmCommandId id, KmError *error)
{
    const KmCommandSpec *command = find_command(id);
    int64_t argument = current_argument(loop);
    KmStatus status;

    reset_command_input(loop);
    if (command == NULL) {
        return fail(error, KM_ERR_INVALID, "command registry");
    }
    status = command->callback(view, argument, error);
    if (status == KM_OK) loop->last_command = id;
    return status;
}

static size_t encode_scalar(uint32_t codepoint, uint8_t bytes[4])
{
    if (codepoint <= 0x7fu) {
        bytes[0] = (uint8_t)codepoint;
        return 1;
    }
    if (codepoint <= 0x7ffu) {
        bytes[0] = (uint8_t)(0xc0u | (codepoint >> 6));
        bytes[1] = (uint8_t)(0x80u | (codepoint & 0x3fu));
        return 2;
    }
    if (codepoint <= 0xffffu) {
        bytes[0] = (uint8_t)(0xe0u | (codepoint >> 12));
        bytes[1] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[2] = (uint8_t)(0x80u | (codepoint & 0x3fu));
        return 3;
    }
    bytes[0] = (uint8_t)(0xf0u | (codepoint >> 18));
    bytes[1] = (uint8_t)(0x80u | ((codepoint >> 12) & 0x3fu));
    bytes[2] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu));
    bytes[3] = (uint8_t)(0x80u | (codepoint & 0x3fu));
    return 4;
}

static KmStatus dispatch_text(KmCommandLoop *loop, KmView *view,
                              uint32_t codepoint, KmError *error)
{
    uint8_t bytes[4];
    size_t len = encode_scalar(codepoint, bytes);
    int64_t argument = current_argument(loop);
    KmStatus status;

    reset_command_input(loop);
    status = insert_utf8_repeated(view, bytes, len, argument,
                                  KM_COMMAND_INSERT_UTF8_BLOCK, error);
    if (status == KM_OK) loop->last_command = KM_COMMAND_INSERT_UTF8_BLOCK;
    return status;
}

static KmStatus dispatch_one(KmCommandLoop *loop, KmView *view,
                             const KmEvent *event, KmError *error)
{
    uint32_t codepoint;
    uint32_t modifiers;
    size_t node;

    if (event->kind == KM_EVENT_RESIZE || event->kind == KM_EVENT_FOCUS ||
        event->kind == KM_EVENT_MOUSE || event->kind == KM_EVENT_EOF) {
        return KM_OK;
    }
    if (event->kind == KM_EVENT_PASTE) {
        KmStatus status;
        bool has_argument = loop->prefix_kind != KM_PREFIX_NONE;
        reset_command_input(loop);
        if (has_argument) {
            return fail(error, KM_ERR_INVALID, "paste argument");
        }
        status = insert_utf8_repeated(view, event->text, event->text_len, 1,
                                      KM_COMMAND_PASTE, error);
        if (status == KM_OK) loop->last_command = KM_COMMAND_PASTE;
        return status;
    }
    if (event->kind == KM_EVENT_TEXT &&
        (event->text != NULL || event->text_len != 0)) {
        int64_t argument = current_argument(loop);
        KmStatus status;
        reset_command_input(loop);
        status = insert_utf8_repeated(view, event->text, event->text_len,
                                      argument,
                                      KM_COMMAND_INSERT_UTF8_BLOCK, error);
        if (status == KM_OK) {
            loop->last_command = KM_COMMAND_INSERT_UTF8_BLOCK;
        }
        return status;
    }

    codepoint = event->codepoint;
    modifiers = event->modifiers;
    if (is_quit_key(codepoint, modifiers)) {
        reset_command_input(loop);
        loop->quit_requested = true;
        return KM_OK;
    }
    if (loop->key_node == 0) {
        bool meta_digit = modifiers == KM_MOD_ALT &&
                          codepoint >= '0' && codepoint <= '9';
        bool plain_prefix_digit = loop->prefix_kind != KM_PREFIX_NONE &&
                                  modifiers == 0 && codepoint >= '0' &&
                                  codepoint <= '9';
        bool meta_minus = modifiers == KM_MOD_ALT && codepoint == '-';
        bool plain_prefix_minus = loop->prefix_kind != KM_PREFIX_NONE &&
                                  modifiers == 0 && codepoint == '-';

        if (meta_digit || plain_prefix_digit) {
            return append_prefix_digit(loop, codepoint - '0', error);
        }
        if (meta_minus || plain_prefix_minus) {
            return begin_negative_prefix(loop, error);
        }
    }

    node = find_key_child(loop->key_node, codepoint, modifiers);
    if (node == KM_NO_KEY_NODE) {
        if (event->kind == KM_EVENT_TEXT && loop->key_node == 0) {
            return dispatch_text(loop, view, codepoint, error);
        }
        reset_command_input(loop);
        return fail(error, KM_ERR_INVALID, "undefined key");
    }
    if (key_trie[node].child != KM_NO_KEY_NODE) {
        loop->key_node = node;
        return KM_OK;
    }
    if (key_trie[node].command == KM_INTERNAL_UNIVERSAL_ARGUMENT) {
        return universal_argument(loop, error);
    }
    if (key_trie[node].command == KM_INTERNAL_EXIT) {
        reset_command_input(loop);
        loop->exit_requested = true;
        return KM_OK;
    }
    return execute_registered_command(
        loop, view, (KmCommandId)key_trie[node].command, error);
}

KmStatus km_command_loop_create(KmCommandLoop **out_loop, KmError *error)
{
    KmCommandLoop *loop;

    km_error_clear(error);
    if (out_loop == NULL) {
        return fail(error, KM_ERR_INVALID, "command loop create");
    }
    *out_loop = NULL;
    loop = (KmCommandLoop *)calloc(1, sizeof(*loop));
    if (loop == NULL) {
        return fail(error, KM_ERR_OOM, "command loop create");
    }
    *out_loop = loop;
    return KM_OK;
}

void km_command_loop_destroy(KmCommandLoop *loop)
{
    free(loop);
}

KmStatus km_command_loop_dispatch(KmCommandLoop *loop, KmView *view,
                                  const KmEvent *event, KmError *error)
{
    uint32_t repeat;
    uint32_t i;
    KmStatus status;

    km_error_clear(error);
    if (loop == NULL || view == NULL) {
        return fail(error, KM_ERR_INVALID, "command dispatch");
    }
    status = validate_event(event, error);
    if (status != KM_OK) {
        reset_command_input(loop);
        return status;
    }
    repeat = event->kind == KM_EVENT_KEY || event->kind == KM_EVENT_TEXT
                 ? event->repeat
                 : 1;
    for (i = 0; i < repeat; ++i) {
        status = dispatch_one(loop, view, event, error);
        if (status != KM_OK) return status;
    }
    return KM_OK;
}

KmCommandId km_command_loop_last_command(const KmCommandLoop *loop)
{
    return loop == NULL ? KM_COMMAND_NONE : loop->last_command;
}

bool km_command_loop_quit_requested(const KmCommandLoop *loop)
{
    return loop != NULL && loop->quit_requested;
}

void km_command_loop_clear_quit(KmCommandLoop *loop)
{
    if (loop != NULL) loop->quit_requested = false;
}

bool km_command_loop_exit_requested(const KmCommandLoop *loop)
{
    return loop != NULL && loop->exit_requested;
}
