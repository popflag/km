#include "document.h"

#include "utf8proc.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *mem;
    size_t cap;
    size_t gap_lo;
    size_t gap_hi;
} KmGap;

typedef struct {
    size_t start;
    size_t end;
    uint8_t *insert;
    size_t insert_len;
    uint32_t ordinal;
} KmOwnedSplice;

typedef struct {
    KmAnchorId id;
    size_t old_pos;
    size_t new_pos;
    size_t expected_before_forward;
    size_t expected_after_forward;
    size_t expected_before_inverse;
    size_t expected_after_inverse;
    bool eligible;
} KmAnchorRestore;

typedef struct KmUndoTxn {
    struct KmUndoTxn *prev;
    struct KmUndoTxn *next;
    KmOwnedSplice *forward;
    KmOwnedSplice *inverse;
    KmAnchorRestore *restores;
    size_t splice_count;
    size_t restore_count;
    KmStateId state_before;
    KmStateId state_after;
    uint64_t command_id;
} KmUndoTxn;

struct KmAnchor {
    KmDocument *document;
    KmAnchorId id;
    size_t pos;
    KmAnchorAffinity affinity;
    KmAnchor *next;
    KmAnchor **prev_next;
};

struct KmDocument {
    KmGap gap;
    KmRevision revision;
    KmStateId history_state;
    KmStateId next_state_id;
    KmAnchorId next_anchor_id;
    KmAnchor *anchors;
    size_t anchor_count;
    KmUndoTxn *history_head;
    KmUndoTxn *history_cursor;
};

typedef struct {
    KmSplice splice;
    size_t source_index;
} KmPreparedSplice;

typedef struct {
    KmAnchorId id;
    size_t pos;
    KmAnchorAffinity affinity;
} KmAnchorSnapshot;

static KmStatus fail(KmError *error, KmStatus status,
                     const char *operation) {
    if (error != NULL) {
        error->code = status;
        error->os_code = 0;
        error->operation = operation;
    }
    return status;
}

static size_t text_len(const KmDocument *document) {
    return document->gap.cap -
           (document->gap.gap_hi - document->gap.gap_lo);
}

static uint8_t byte_at(const KmDocument *document, size_t position) {
    const KmGap *gap = &document->gap;
    size_t physical = position < gap->gap_lo
                          ? position
                          : position + (gap->gap_hi - gap->gap_lo);
    return gap->mem[physical];
}

static bool valid_utf8(const uint8_t *text, size_t len) {
    size_t offset = 0;
    utf8proc_int32_t codepoint;

    if (len > (size_t)PTRDIFF_MAX || (len != 0 && text == NULL)) {
        return false;
    }
    while (offset < len) {
        utf8proc_ssize_t consumed =
            utf8proc_iterate(text + offset,
                             (utf8proc_ssize_t)(len - offset), &codepoint);
        if (consumed <= 0) {
            return false;
        }
        offset += (size_t)consumed;
    }
    return true;
}

static bool is_boundary(const KmDocument *document, size_t position) {
    size_t len = text_len(document);
    if (position > len) {
        return false;
    }
    return position == 0 || position == len ||
           (byte_at(document, position) & 0xc0u) != 0x80u;
}

static bool checked_array_size(size_t count, size_t element_size,
                               size_t *out_size) {
    if (element_size != 0 && count > SIZE_MAX / element_size) {
        return false;
    }
    *out_size = count * element_size;
    return true;
}

static void *allocate_array(size_t count, size_t element_size) {
    size_t size;
    if (!checked_array_size(count, element_size, &size)) {
        return NULL;
    }
    return size == 0 ? NULL : calloc(1, size);
}

static bool add_delta(size_t position, ptrdiff_t delta, size_t *out) {
    if (delta >= 0) {
        size_t amount = (size_t)delta;
        if (position > (size_t)PTRDIFF_MAX - amount) {
            return false;
        }
        *out = position + amount;
        return true;
    }
    {
        size_t amount = (size_t)(-delta);
        if (position < amount) {
            return false;
        }
        *out = position - amount;
        return true;
    }
}

static bool update_delta(ptrdiff_t *delta, size_t inserted, size_t deleted) {
    size_t amount;
    if (inserted >= deleted) {
        amount = inserted - deleted;
        if (amount > (size_t)PTRDIFF_MAX ||
            *delta > PTRDIFF_MAX - (ptrdiff_t)amount) {
            return false;
        }
        *delta += (ptrdiff_t)amount;
    } else {
        amount = deleted - inserted;
        if (amount > (size_t)PTRDIFF_MAX ||
            *delta < -PTRDIFF_MAX + (ptrdiff_t)amount) {
            return false;
        }
        *delta -= (ptrdiff_t)amount;
    }
    return true;
}

static bool choose_capacity(size_t current, size_t required, size_t *out) {
    size_t capacity = current < 4096 ? 4096 : current;
    size_t grown;

    if (capacity >= required) {
        *out = capacity;
        return true;
    }
    grown = capacity <= SIZE_MAX - capacity / 2
                ? capacity + capacity / 2
                : SIZE_MAX;
    capacity = grown > required ? grown : required;
    *out = capacity;
    return true;
}

static KmStatus reserve_gap(KmDocument *document, size_t required,
                            KmError *error) {
    KmGap *gap = &document->gap;
    size_t new_capacity;
    size_t suffix_len;
    uint8_t *new_mem;

    if (required <= gap->cap) {
        return KM_OK;
    }
    if (!choose_capacity(gap->cap, required, &new_capacity)) {
        return fail(error, KM_ERR_OOM, "document capacity");
    }
    suffix_len = gap->cap - gap->gap_hi;
    new_mem = (uint8_t *)realloc(gap->mem, new_capacity);
    if (new_mem == NULL) {
        return fail(error, KM_ERR_OOM, "document capacity");
    }
    memmove(new_mem + new_capacity - suffix_len,
            new_mem + gap->gap_hi, suffix_len);
    gap->mem = new_mem;
    gap->cap = new_capacity;
    gap->gap_hi = new_capacity - suffix_len;
    return KM_OK;
}

static void move_gap(KmGap *gap, size_t position) {
    if (position < gap->gap_lo) {
        size_t count = gap->gap_lo - position;
        memmove(gap->mem + gap->gap_hi - count,
                gap->mem + position, count);
        gap->gap_lo = position;
        gap->gap_hi -= count;
    } else if (position > gap->gap_lo) {
        size_t count = position - gap->gap_lo;
        memmove(gap->mem + gap->gap_lo, gap->mem + gap->gap_hi, count);
        gap->gap_lo += count;
        gap->gap_hi += count;
    }
}

static void replace_gap(KmGap *gap, size_t start, size_t end,
                        const uint8_t *insert, size_t insert_len) {
    move_gap(gap, start);
    gap->gap_hi += end - start;
    if (insert_len != 0) {
        memcpy(gap->mem + gap->gap_lo, insert, insert_len);
        gap->gap_lo += insert_len;
    }
}

static size_t transform_position(size_t position, KmAnchorAffinity affinity,
                                 size_t start, size_t end,
                                 size_t insert_len) {
    size_t deleted = end - start;
    if (deleted == 0) {
        if (position > start ||
            (position == start && affinity == KM_ANCHOR_AFTER)) {
            return position + insert_len;
        }
        return position;
    }
    if (position >= end) {
        if (insert_len >= deleted) {
            return position + (insert_len - deleted);
        }
        return position - (deleted - insert_len);
    }
    if (position > start) {
        return start;
    }
    return position;
}

static void transform_anchors(KmDocument *document, size_t start, size_t end,
                              size_t insert_len) {
    KmAnchor *anchor;
    for (anchor = document->anchors; anchor != NULL; anchor = anchor->next) {
        anchor->pos = transform_position(anchor->pos, anchor->affinity,
                                         start, end, insert_len);
    }
}

static void free_splices(KmOwnedSplice *splices, size_t count) {
    size_t i;
    if (splices == NULL) {
        return;
    }
    for (i = 0; i < count; ++i) {
        free(splices[i].insert);
    }
    free(splices);
}

static void free_transaction(KmUndoTxn *transaction) {
    if (transaction == NULL) {
        return;
    }
    free_splices(transaction->forward, transaction->splice_count);
    free_splices(transaction->inverse, transaction->splice_count);
    free(transaction->restores);
    free(transaction);
}

static void free_transaction_chain(KmUndoTxn *transaction) {
    while (transaction != NULL) {
        KmUndoTxn *next = transaction->next;
        free_transaction(transaction);
        transaction = next;
    }
}

static int compare_prepared(const void *left_pointer,
                            const void *right_pointer) {
    const KmPreparedSplice *left = (const KmPreparedSplice *)left_pointer;
    const KmPreparedSplice *right = (const KmPreparedSplice *)right_pointer;
#define KM_COMPARE_FIELD(field)                                                \
    do {                                                                       \
        if (left->splice.field < right->splice.field) return -1;               \
        if (left->splice.field > right->splice.field) return 1;                \
    } while (0)
    KM_COMPARE_FIELD(start.v);
    KM_COMPARE_FIELD(end.v);
    KM_COMPARE_FIELD(ordinal);
#undef KM_COMPARE_FIELD
    if (left->source_index < right->source_index) return -1;
    if (left->source_index > right->source_index) return 1;
    return 0;
}

static bool ranges_conflict(const KmSplice *left, const KmSplice *right) {
    bool left_range = left->start.v < left->end.v;
    bool right_range = right->start.v < right->end.v;
    if (left_range && right_range) {
        return left->start.v < right->end.v &&
               right->start.v < left->end.v;
    }
    if (!left_range && right_range) {
        return right->start.v <= left->start.v &&
               left->start.v < right->end.v;
    }
    if (left_range && !right_range) {
        return left->start.v <= right->start.v &&
               right->start.v < left->end.v;
    }
    return false;
}

static bool measure_splices(size_t initial_len,
                            const KmOwnedSplice *splices, size_t count,
                            size_t *out_final, size_t *out_peak) {
    size_t current_len = initial_len;
    size_t peak = initial_len;
    ptrdiff_t delta = 0;
    size_t i;
    for (i = 0; i < count; ++i) {
        size_t deleted = splices[i].end - splices[i].start;
        size_t current_start;
        size_t current_end;
        if (!add_delta(splices[i].start, delta, &current_start) ||
            !add_delta(splices[i].end, delta, &current_end) ||
            current_start > current_end || current_end > current_len ||
            current_len - deleted > (size_t)PTRDIFF_MAX -
                                        splices[i].insert_len) {
            return false;
        }
        current_len = current_len - deleted + splices[i].insert_len;
        if (current_len > peak) {
            peak = current_len;
        }
        if (!update_delta(&delta, splices[i].insert_len, deleted)) {
            return false;
        }
    }
    *out_final = current_len;
    *out_peak = peak;
    return true;
}

static size_t simulate_position(size_t position, KmAnchorAffinity affinity,
                                const KmOwnedSplice *splices, size_t count) {
    ptrdiff_t delta = 0;
    size_t i;
    for (i = 0; i < count; ++i) {
        size_t start;
        size_t end;
        size_t deleted = splices[i].end - splices[i].start;
        (void)add_delta(splices[i].start, delta, &start);
        (void)add_delta(splices[i].end, delta, &end);
        position = transform_position(position, affinity, start, end,
                                      splices[i].insert_len);
        (void)update_delta(&delta, splices[i].insert_len, deleted);
    }
    return position;
}

static void apply_owned_splices(KmDocument *document,
                                const KmOwnedSplice *splices, size_t count) {
    ptrdiff_t delta = 0;
    size_t i;
    for (i = 0; i < count; ++i) {
        size_t start;
        size_t end;
        size_t deleted = splices[i].end - splices[i].start;
        (void)add_delta(splices[i].start, delta, &start);
        (void)add_delta(splices[i].end, delta, &end);
        replace_gap(&document->gap, start, end, splices[i].insert,
                    splices[i].insert_len);
        transform_anchors(document, start, end, splices[i].insert_len);
        (void)update_delta(&delta, splices[i].insert_len, deleted);
    }
}

static KmAnchor *find_anchor(KmDocument *document, KmAnchorId id) {
    KmAnchor *anchor;
    for (anchor = document->anchors; anchor != NULL; anchor = anchor->next) {
        if (anchor->id == id) {
            return anchor;
        }
    }
    return NULL;
}

static KmStatus copy_deleted_bytes(const KmDocument *document, size_t start,
                                   size_t len, uint8_t **out,
                                   KmError *error) {
    uint8_t *copy;
    if (len == 0) {
        *out = NULL;
        return KM_OK;
    }
    copy = (uint8_t *)malloc(len);
    if (copy == NULL) {
        return fail(error, KM_ERR_OOM, "deleted text");
    }
    if (km_document_copy(document, (KmBytePos){start}, len, copy, error) !=
        KM_OK) {
        free(copy);
        return error != NULL ? error->code : KM_ERR_INVALID;
    }
    *out = copy;
    return KM_OK;
}

static KmStatus prepare_transaction(KmDocument *document,
                                    const KmSplice *splices, size_t count,
                                    KmTxnMeta meta, KmUndoTxn **out,
                                    KmError *error) {
    KmPreparedSplice *prepared = NULL;
    KmAnchorSnapshot *snapshots = NULL;
    KmUndoTxn *transaction = NULL;
    KmAnchor *anchor;
    size_t document_len = text_len(document);
    size_t anchor_index = 0;
    size_t effective_count = 0;
    size_t final_len;
    size_t forward_peak;
    size_t inverse_final;
    size_t inverse_peak;
    size_t required_capacity;
    ptrdiff_t delta = 0;
    size_t i;
    size_t j;
    KmStatus status = KM_OK;

    *out = NULL;
    prepared = (KmPreparedSplice *)allocate_array(count, sizeof(*prepared));
    if (prepared == NULL) {
        return fail(error, KM_ERR_OOM, "splice descriptors");
    }
    for (i = 0; i < count; ++i) {
        const KmSplice *splice = &splices[i];
        if (splice->start.v > splice->end.v ||
            splice->end.v > document_len ||
            !is_boundary(document, splice->start.v) ||
            !is_boundary(document, splice->end.v) ||
            splice->insert_len > (size_t)PTRDIFF_MAX ||
            !valid_utf8(splice->insert, splice->insert_len)) {
            status = fail(error, KM_ERR_INVALID, "splice validation");
            goto cleanup;
        }
        if (splice->start.v != splice->end.v || splice->insert_len != 0) {
            prepared[effective_count].splice = *splice;
            prepared[effective_count].source_index = i;
            ++effective_count;
        }
    }
    if (effective_count == 0) {
        free(prepared);
        return KM_OK;
    }
    count = effective_count;
    qsort(prepared, count, sizeof(*prepared), compare_prepared);

    /* ponytail: O(K^2) is simplest; use a sweep if measured batch sizes hurt. */
    for (i = 0; i < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            if (ranges_conflict(&prepared[i].splice, &prepared[j].splice)) {
                status = fail(error, KM_ERR_INVALID, "splice overlap");
                goto cleanup;
            }
        }
    }

    transaction = (KmUndoTxn *)calloc(1, sizeof(*transaction));
    if (transaction == NULL) {
        status = fail(error, KM_ERR_OOM, "undo transaction");
        goto cleanup;
    }
    transaction->forward =
        (KmOwnedSplice *)allocate_array(count, sizeof(*transaction->forward));
    transaction->inverse =
        (KmOwnedSplice *)allocate_array(count, sizeof(*transaction->inverse));
    if (transaction->forward == NULL || transaction->inverse == NULL) {
        status = fail(error, KM_ERR_OOM, "undo splices");
        goto cleanup;
    }
    transaction->splice_count = count;
    transaction->command_id = meta.command_id;
    transaction->state_before = document->history_state;
    transaction->state_after = document->next_state_id;

    for (i = 0; i < count; ++i) {
        const KmSplice *source = &prepared[i].splice;
        KmOwnedSplice *forward = &transaction->forward[i];
        size_t deleted = source->end.v - source->start.v;
        size_t current_start;
        forward->start = source->start.v;
        forward->end = source->end.v;
        forward->insert_len = source->insert_len;
        forward->ordinal = source->ordinal;
        if (source->insert_len != 0) {
            forward->insert = (uint8_t *)malloc(source->insert_len);
            if (forward->insert == NULL) {
                status = fail(error, KM_ERR_OOM, "insert text");
                goto cleanup;
            }
            memcpy(forward->insert, source->insert, source->insert_len);
        }
        if (!add_delta(source->start.v, delta, &current_start)) {
            status = fail(error, KM_ERR_INVALID, "splice coordinate");
            goto cleanup;
        }
        transaction->inverse[i].start = current_start;
        if (current_start > (size_t)PTRDIFF_MAX - source->insert_len) {
            status = fail(error, KM_ERR_INVALID, "splice length");
            goto cleanup;
        }
        transaction->inverse[i].end = current_start + source->insert_len;
        transaction->inverse[i].insert_len = deleted;
        transaction->inverse[i].ordinal = source->ordinal;
        status = copy_deleted_bytes(document, source->start.v, deleted,
                                    &transaction->inverse[i].insert, error);
        if (status != KM_OK) {
            goto cleanup;
        }
        if (!update_delta(&delta, source->insert_len, deleted)) {
            status = fail(error, KM_ERR_INVALID, "splice delta");
            goto cleanup;
        }
    }

    if (!measure_splices(document_len, transaction->forward, count,
                         &final_len, &forward_peak) ||
        !measure_splices(final_len, transaction->inverse, count,
                         &inverse_final, &inverse_peak) ||
        inverse_final != document_len) {
        status = fail(error, KM_ERR_INVALID, "splice size");
        goto cleanup;
    }
    required_capacity = forward_peak > inverse_peak ? forward_peak
                                                    : inverse_peak;

    if (document->anchor_count != 0) {
        snapshots = (KmAnchorSnapshot *)allocate_array(
            document->anchor_count, sizeof(*snapshots));
        transaction->restores = (KmAnchorRestore *)allocate_array(
            document->anchor_count, sizeof(*transaction->restores));
        if (snapshots == NULL || transaction->restores == NULL) {
            status = fail(error, KM_ERR_OOM, "anchor restore");
            goto cleanup;
        }
        for (anchor = document->anchors; anchor != NULL;
             anchor = anchor->next) {
            snapshots[anchor_index].id = anchor->id;
            snapshots[anchor_index].pos = anchor->pos;
            snapshots[anchor_index].affinity = anchor->affinity;
            ++anchor_index;
        }
        for (i = 0; i < anchor_index; ++i) {
            size_t final_position = simulate_position(
                snapshots[i].pos, snapshots[i].affinity,
                transaction->forward, count);
            size_t inverse_position = simulate_position(
                final_position, snapshots[i].affinity,
                transaction->inverse, count);
            if (inverse_position != snapshots[i].pos) {
                KmAnchorRestore *restore =
                    &transaction->restores[transaction->restore_count++];
                restore->id = snapshots[i].id;
                restore->old_pos = snapshots[i].pos;
                restore->new_pos = final_position;
                restore->expected_before_forward = snapshots[i].pos;
                restore->expected_after_forward = final_position;
                restore->expected_before_inverse = final_position;
                restore->expected_after_inverse = inverse_position;
            }
        }
    }

    status = reserve_gap(document, required_capacity, error);
    if (status != KM_OK) {
        goto cleanup;
    }
    free(prepared);
    free(snapshots);
    *out = transaction;
    return KM_OK;

cleanup:
    free(prepared);
    free(snapshots);
    free_transaction(transaction);
    return status;
}

KmStatus km_document_create(const uint8_t *text, size_t len,
                            KmDocument **out_document, KmError *error) {
    KmDocument *document;
    size_t capacity;
    km_error_clear(error);
    if (out_document == NULL) {
        return fail(error, KM_ERR_INVALID, "document create");
    }
    *out_document = NULL;
    if (len > (size_t)PTRDIFF_MAX || !valid_utf8(text, len) ||
        !choose_capacity(0, len, &capacity)) {
        return fail(error, KM_ERR_INVALID, "document create");
    }
    document = (KmDocument *)calloc(1, sizeof(*document));
    if (document == NULL) {
        return fail(error, KM_ERR_OOM, "document create");
    }
    document->gap.mem = (uint8_t *)malloc(capacity);
    if (document->gap.mem == NULL) {
        free(document);
        return fail(error, KM_ERR_OOM, "document text");
    }
    if (len != 0) {
        memcpy(document->gap.mem, text, len);
    }
    document->gap.cap = capacity;
    document->gap.gap_lo = len;
    document->gap.gap_hi = capacity;
    document->next_state_id = 1;
    document->next_anchor_id = 1;
    *out_document = document;
    return KM_OK;
}

void km_document_destroy(KmDocument *document) {
    KmAnchor *anchor;
    if (document == NULL) {
        return;
    }
    anchor = document->anchors;
    while (anchor != NULL) {
        KmAnchor *next = anchor->next;
        free(anchor);
        anchor = next;
    }
    free_transaction_chain(document->history_head);
    free(document->gap.mem);
    free(document);
}

size_t km_document_len(const KmDocument *document) {
    return document == NULL ? 0 : text_len(document);
}

KmRevision km_document_revision(const KmDocument *document) {
    return document == NULL ? 0 : document->revision;
}

KmStateId km_document_history_state(const KmDocument *document) {
    return document == NULL ? 0 : document->history_state;
}

bool km_document_is_boundary(const KmDocument *document, KmBytePos position) {
    return document != NULL && is_boundary(document, position.v);
}

KmStatus km_document_copy(const KmDocument *document, KmBytePos start,
                          size_t len, uint8_t *destination, KmError *error) {
    size_t available;
    size_t first = 0;
    size_t logical = start.v;
    km_error_clear(error);
    if (document == NULL || (len != 0 && destination == NULL) ||
        start.v > text_len(document)) {
        return fail(error, KM_ERR_INVALID, "document copy");
    }
    available = text_len(document) - start.v;
    if (len > available) {
        return fail(error, KM_ERR_INVALID, "document copy");
    }
    if (len == 0) {
        return KM_OK;
    }
    if (logical < document->gap.gap_lo) {
        first = document->gap.gap_lo - logical;
        if (first > len) {
            first = len;
        }
        memcpy(destination, document->gap.mem + logical, first);
        logical += first;
    }
    if (first < len) {
        size_t physical = logical +
                          (document->gap.gap_hi - document->gap.gap_lo);
        memcpy(destination + first, document->gap.mem + physical, len - first);
    }
    return KM_OK;
}

KmStatus km_document_iter_init(const KmDocument *document, KmBytePos start,
                               KmTextIter *out_iterator, KmError *error) {
    km_error_clear(error);
    if (out_iterator == NULL) {
        return fail(error, KM_ERR_INVALID, "document iterator");
    }
    *out_iterator = (KmTextIter){0};
    if (document == NULL || !is_boundary(document, start.v)) {
        return fail(error, KM_ERR_INVALID, "document iterator");
    }
    out_iterator->document = document;
    out_iterator->revision = document->revision;
    out_iterator->position = start;
    return KM_OK;
}

KmStatus km_text_iter_read(KmTextIter *iterator, uint8_t *destination,
                           size_t capacity, size_t *out_len, bool *out_eof,
                           KmError *error) {
    const KmDocument *document;
    size_t available;
    size_t count;
    KmStatus status;

    km_error_clear(error);
    if (out_len != NULL) *out_len = 0;
    if (out_eof != NULL) *out_eof = false;
    if (iterator == NULL || out_len == NULL || out_eof == NULL ||
        (capacity != 0 && destination == NULL)) {
        return fail(error, KM_ERR_INVALID, "document iterator read");
    }
    document = iterator->document;
    if (document == NULL) {
        return fail(error, KM_ERR_INVALID, "document iterator read");
    }
    if (iterator->revision != document->revision) {
        return fail(error, KM_ERR_CONFLICT, "document iterator read");
    }
    if (iterator->position.v > text_len(document)) {
        return fail(error, KM_ERR_INVALID, "document iterator read");
    }
    available = text_len(document) - iterator->position.v;
    count = capacity < available ? capacity : available;
    status = km_document_copy(document, iterator->position, count,
                              destination, error);
    if (status != KM_OK) {
        return status;
    }
    iterator->position.v += count;
    *out_len = count;
    *out_eof = iterator->position.v == text_len(document);
    return KM_OK;
}

KmStatus km_anchor_create(KmDocument *document, KmBytePos position,
                          KmAnchorAffinity affinity, KmAnchor **out_anchor,
                          KmError *error) {
    KmAnchor *anchor;
    km_error_clear(error);
    if (out_anchor == NULL || document == NULL ||
        (affinity != KM_ANCHOR_BEFORE && affinity != KM_ANCHOR_AFTER) ||
        !is_boundary(document, position.v) ||
        document->next_anchor_id == UINT64_MAX ||
        document->anchor_count == SIZE_MAX) {
        return fail(error, KM_ERR_INVALID, "anchor create");
    }
    *out_anchor = NULL;
    anchor = (KmAnchor *)calloc(1, sizeof(*anchor));
    if (anchor == NULL) {
        return fail(error, KM_ERR_OOM, "anchor create");
    }
    anchor->document = document;
    anchor->id = document->next_anchor_id++;
    anchor->pos = position.v;
    anchor->affinity = affinity;
    anchor->next = document->anchors;
    anchor->prev_next = &document->anchors;
    if (anchor->next != NULL) {
        anchor->next->prev_next = &anchor->next;
    }
    document->anchors = anchor;
    ++document->anchor_count;
    *out_anchor = anchor;
    return KM_OK;
}

void km_anchor_destroy(KmAnchor *anchor) {
    if (anchor == NULL) {
        return;
    }
    *anchor->prev_next = anchor->next;
    if (anchor->next != NULL) {
        anchor->next->prev_next = anchor->prev_next;
    }
    --anchor->document->anchor_count;
    free(anchor);
}

KmBytePos km_anchor_get(const KmAnchor *anchor) {
    return (KmBytePos){anchor == NULL ? 0 : anchor->pos};
}

KmAnchorAffinity km_anchor_affinity(const KmAnchor *anchor) {
    return anchor == NULL ? KM_ANCHOR_BEFORE : anchor->affinity;
}

KmAnchorId km_anchor_id(const KmAnchor *anchor) {
    return anchor == NULL ? 0 : anchor->id;
}

KmStatus km_anchor_set(KmAnchor *anchor, KmBytePos position, KmError *error) {
    km_error_clear(error);
    if (anchor == NULL || !is_boundary(anchor->document, position.v)) {
        return fail(error, KM_ERR_INVALID, "anchor set");
    }
    anchor->pos = position.v;
    return KM_OK;
}

static KmStatus document_apply(KmDocument *document, const KmSplice *splices,
                               size_t count, KmTxnMeta meta,
                               KmAnchor *moved_anchor, size_t moved_position,
                               KmError *error) {
    KmUndoTxn *transaction;
    KmUndoTxn *discard;
    KmStatus status;
    km_error_clear(error);
    if (document == NULL || (count != 0 && splices == NULL)) {
        return fail(error, KM_ERR_INVALID, "document apply");
    }
    if (meta.expected_revision != document->revision) {
        return fail(error, KM_ERR_CONFLICT, "document apply");
    }
    if (count == 0) {
        return KM_OK;
    }
    if (document->revision == UINT64_MAX ||
        document->next_state_id == UINT64_MAX) {
        return fail(error, KM_ERR_INVALID, "document revision");
    }
    status = prepare_transaction(document, splices, count, meta,
                                 &transaction, error);
    if (status != KM_OK) {
        return status;
    }
    if (transaction == NULL) {
        if (moved_anchor != NULL) moved_anchor->pos = moved_position;
        return KM_OK;
    }

    if (moved_anchor != NULL) {
        KmAnchorRestore *restore = NULL;
        size_t i;

        for (i = 0; i < transaction->restore_count; ++i) {
            if (transaction->restores[i].id == moved_anchor->id) {
                restore = &transaction->restores[i];
                break;
            }
        }
        if (restore == NULL) {
            restore = &transaction->restores[transaction->restore_count++];
            restore->id = moved_anchor->id;
            restore->old_pos = moved_anchor->pos;
            restore->expected_before_forward = moved_anchor->pos;
        }
        restore->new_pos = moved_position;
        restore->expected_after_forward = simulate_position(
            moved_anchor->pos, moved_anchor->affinity,
            transaction->forward, transaction->splice_count);
        restore->expected_before_inverse = moved_position;
        restore->expected_after_inverse = simulate_position(
            moved_position, moved_anchor->affinity,
            transaction->inverse, transaction->splice_count);
    }

    apply_owned_splices(document, transaction->forward,
                        transaction->splice_count);
    if (moved_anchor != NULL) moved_anchor->pos = moved_position;
    discard = document->history_cursor != NULL
                  ? document->history_cursor->next
                  : document->history_head;
    if (document->history_cursor != NULL) {
        document->history_cursor->next = NULL;
    } else {
        document->history_head = NULL;
    }
    free_transaction_chain(discard);
    transaction->prev = document->history_cursor;
    if (document->history_cursor != NULL) {
        document->history_cursor->next = transaction;
    } else {
        document->history_head = transaction;
    }
    document->history_cursor = transaction;
    document->history_state = transaction->state_after;
    ++document->next_state_id;
    ++document->revision;
    return KM_OK;
}

KmStatus km_document_apply(KmDocument *document, const KmSplice *splices,
                           size_t count, KmTxnMeta meta, KmError *error) {
    return document_apply(document, splices, count, meta, NULL, 0, error);
}

static bool boundary_after_splice(const KmDocument *document,
                                  const KmSplice *splice, size_t position) {
    size_t deleted;
    size_t final_len;
    size_t inserted_end;

    if (splice->start.v > splice->end.v ||
        splice->end.v > text_len(document) ||
        splice->insert_len > SIZE_MAX - splice->start.v ||
        text_len(document) - (splice->end.v - splice->start.v) >
            SIZE_MAX - splice->insert_len) {
        return false;
    }
    deleted = splice->end.v - splice->start.v;
    final_len = text_len(document) - deleted + splice->insert_len;
    inserted_end = splice->start.v + splice->insert_len;
    if (position > final_len) return false;
    if (position < splice->start.v) return is_boundary(document, position);
    if (position <= inserted_end) {
        size_t offset = position - splice->start.v;
        return valid_utf8(splice->insert, offset);
    }
    return is_boundary(document,
                       position - splice->insert_len + deleted);
}

KmStatus km_document_apply_and_set_anchor(KmDocument *document,
                                          const KmSplice *splice,
                                          KmTxnMeta meta, KmAnchor *anchor,
                                          KmBytePos position,
                                          KmError *error) {
    km_error_clear(error);
    if (document == NULL || splice == NULL || anchor == NULL ||
        anchor->document != document ||
        !boundary_after_splice(document, splice, position.v)) {
        return fail(error, KM_ERR_INVALID, "document apply anchor");
    }
    return document_apply(document, splice, 1, meta, anchor, position.v,
                          error);
}

bool km_document_can_undo(const KmDocument *document) {
    return document != NULL && document->history_cursor != NULL;
}

bool km_document_can_redo(const KmDocument *document) {
    return document != NULL &&
           (document->history_cursor != NULL
                ? document->history_cursor->next != NULL
                : document->history_head != NULL);
}

static bool splices_within_range(const KmOwnedSplice *splices, size_t count,
                                 size_t start, size_t end) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (splices[i].start < start || splices[i].end > end) {
            return false;
        }
    }
    return true;
}

bool km_document_can_undo_in_range(const KmDocument *document,
                                   KmBytePos start, KmBytePos end) {
    const KmUndoTxn *transaction;

    if (document == NULL || start.v > end.v || end.v > text_len(document) ||
        !is_boundary(document, start.v) || !is_boundary(document, end.v) ||
        document->history_cursor == NULL) {
        return false;
    }
    transaction = document->history_cursor;
    return splices_within_range(transaction->inverse,
                                transaction->splice_count, start.v, end.v);
}

bool km_document_can_redo_in_range(const KmDocument *document,
                                   KmBytePos start, KmBytePos end) {
    const KmUndoTxn *transaction;

    if (document == NULL || start.v > end.v || end.v > text_len(document) ||
        !is_boundary(document, start.v) || !is_boundary(document, end.v)) {
        return false;
    }
    transaction = document->history_cursor != NULL
                      ? document->history_cursor->next
                      : document->history_head;
    return transaction != NULL &&
           splices_within_range(transaction->forward,
                                transaction->splice_count, start.v, end.v);
}

KmStatus km_document_undo(KmDocument *document,
                          KmRevision expected_revision, KmError *error) {
    KmUndoTxn *transaction;
    size_t i;
    km_error_clear(error);
    if (document == NULL) {
        return fail(error, KM_ERR_INVALID, "document undo");
    }
    if (expected_revision != document->revision) {
        return fail(error, KM_ERR_CONFLICT, "document undo");
    }
    if (document->revision == UINT64_MAX) {
        return fail(error, KM_ERR_INVALID, "document revision");
    }
    transaction = document->history_cursor;
    if (transaction == NULL) {
        return fail(error, KM_ERR_INVALID, "document undo");
    }
    for (i = 0; i < transaction->restore_count; ++i) {
        KmAnchorRestore *restore = &transaction->restores[i];
        KmAnchor *anchor = find_anchor(document, restore->id);
        restore->eligible = anchor != NULL &&
                            anchor->pos == restore->expected_before_inverse;
    }
    apply_owned_splices(document, transaction->inverse,
                        transaction->splice_count);
    for (i = 0; i < transaction->restore_count; ++i) {
        KmAnchorRestore *restore = &transaction->restores[i];
        KmAnchor *anchor = find_anchor(document, restore->id);
        if (anchor != NULL &&
            restore->eligible &&
            anchor->pos == restore->expected_after_inverse) {
            anchor->pos = restore->old_pos;
        }
    }
    document->history_state = transaction->state_before;
    document->history_cursor = transaction->prev;
    ++document->revision;
    return KM_OK;
}

KmStatus km_document_redo(KmDocument *document,
                          KmRevision expected_revision, KmError *error) {
    KmUndoTxn *transaction;
    size_t i;
    km_error_clear(error);
    if (document == NULL) {
        return fail(error, KM_ERR_INVALID, "document redo");
    }
    if (expected_revision != document->revision) {
        return fail(error, KM_ERR_CONFLICT, "document redo");
    }
    if (document->revision == UINT64_MAX) {
        return fail(error, KM_ERR_INVALID, "document revision");
    }
    transaction = document->history_cursor != NULL
                      ? document->history_cursor->next
                      : document->history_head;
    if (transaction == NULL) {
        return fail(error, KM_ERR_INVALID, "document redo");
    }
    for (i = 0; i < transaction->restore_count; ++i) {
        KmAnchorRestore *restore = &transaction->restores[i];
        KmAnchor *anchor = find_anchor(document, restore->id);
        restore->eligible = anchor != NULL &&
                            anchor->pos == restore->expected_before_forward;
    }
    apply_owned_splices(document, transaction->forward,
                        transaction->splice_count);
    for (i = 0; i < transaction->restore_count; ++i) {
        KmAnchorRestore *restore = &transaction->restores[i];
        KmAnchor *anchor = find_anchor(document, restore->id);
        if (anchor != NULL && restore->eligible &&
            anchor->pos == restore->expected_after_forward) {
            anchor->pos = restore->new_pos;
        }
    }
    document->history_state = transaction->state_after;
    document->history_cursor = transaction;
    ++document->revision;
    return KM_OK;
}
