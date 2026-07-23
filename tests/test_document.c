#include "document.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static KmDocument *make_document(const uint8_t *text, size_t len) {
    KmDocument *document = NULL;
    KmError error;
    CHECK(km_document_create(text, len, &document, &error) == KM_OK);
    return document;
}

static void check_text(KmDocument *document, const uint8_t *expected,
                       size_t expected_len) {
    uint8_t *actual = expected_len == 0 ? NULL : malloc(expected_len);
    KmError error;
    if (km_document_len(document) != expected_len) {
        fprintf(stderr, "text length: got %zu, expected %zu\n",
                km_document_len(document), expected_len);
    }
    CHECK(km_document_len(document) == expected_len);
    CHECK(km_document_copy(document, (KmBytePos){0}, expected_len, actual,
                           &error) == KM_OK);
    CHECK(expected_len == 0 || memcmp(actual, expected, expected_len) == 0);
    free(actual);
}

static KmStatus apply(KmDocument *document, const KmSplice *splices,
                      size_t count) {
    KmError error;
    KmTxnMeta meta = {km_document_revision(document), 1};
    return km_document_apply(document, splices, count, meta, &error);
}

static void test_utf8_and_boundaries(void) {
    static const uint8_t invalid[][4] = {
        {0xc0, 0xaf, 0, 0},
        {0xed, 0xa0, 0x80, 0},
        {0xf4, 0x90, 0x80, 0x80},
        {0xe2, 0x82, 0, 0},
        {0x80, 0, 0, 0},
    };
    static const size_t invalid_len[] = {2, 3, 4, 2, 1};
    static const uint8_t valid[] = {'A', 0xe2, 0x82, 0xac, 0, 'B'};
    size_t i;
    KmError error;
    for (i = 0; i < sizeof(invalid_len) / sizeof(invalid_len[0]); ++i) {
        KmDocument *document = (KmDocument *)(uintptr_t)1;
        CHECK(km_document_create(invalid[i], invalid_len[i], &document,
                                 &error) == KM_ERR_INVALID);
        CHECK(document == NULL);
    }
    {
        KmDocument *document = make_document(valid, sizeof(valid));
        KmAnchor *anchor = NULL;
        static const uint8_t bad_insert[] = {0xc0, 0x80};
        KmSplice splice = {{1}, {1}, bad_insert, sizeof(bad_insert), 0};
        CHECK(km_document_is_boundary(document, (KmBytePos){1}));
        CHECK(!km_document_is_boundary(document, (KmBytePos){2}));
        CHECK(!km_document_is_boundary(document, (KmBytePos){3}));
        CHECK(km_document_is_boundary(document, (KmBytePos){4}));
        CHECK(km_anchor_create(document, (KmBytePos){2}, KM_ANCHOR_BEFORE,
                               &anchor, &error) == KM_ERR_INVALID);
        CHECK(apply(document, &splice, 1) == KM_ERR_INVALID);
        check_text(document, valid, sizeof(valid));
        km_document_destroy(document);
    }
}

static void test_gap_copy_and_insert_affinity(void) {
    static const uint8_t initial[] = "abcdef";
    static const uint8_t expected[] = "abXYcdef";
    static const uint8_t insert[] = "XY";
    KmDocument *document = make_document(initial, sizeof(initial) - 1);
    KmAnchor *before = NULL;
    KmAnchor *after = NULL;
    KmError error;
    KmSplice splice = {{2}, {2}, insert, sizeof(insert) - 1, 0};
    CHECK(km_anchor_create(document, (KmBytePos){2}, KM_ANCHOR_BEFORE,
                           &before, &error) == KM_OK);
    CHECK(km_anchor_create(document, (KmBytePos){2}, KM_ANCHOR_AFTER,
                           &after, &error) == KM_OK);
    CHECK(apply(document, &splice, 1) == KM_OK);
    check_text(document, expected, sizeof(expected) - 1);
    CHECK(km_anchor_get(before).v == 2);
    CHECK(km_anchor_get(after).v == 4);
    km_document_destroy(document);
}

static void test_anchor_delete_replace_and_restore(void) {
    static const uint8_t initial[] = "0123456789";
    KmDocument *document = make_document(initial, sizeof(initial) - 1);
    KmAnchor *anchors[6];
    size_t positions[] = {2, 2, 4, 4, 7, 7};
    KmAnchorAffinity affinity[] = {
        KM_ANCHOR_BEFORE, KM_ANCHOR_AFTER, KM_ANCHOR_BEFORE,
        KM_ANCHOR_AFTER, KM_ANCHOR_BEFORE, KM_ANCHOR_AFTER,
    };
    static const uint8_t replacement[] = "XYZ";
    KmSplice replace = {{2}, {7}, replacement, sizeof(replacement) - 1, 0};
    KmError error;
    size_t i;
    for (i = 0; i < 6; ++i) {
        CHECK(km_anchor_create(document, (KmBytePos){positions[i]}, affinity[i],
                               &anchors[i], &error) == KM_OK);
    }
    CHECK(apply(document, &replace, 1) == KM_OK);
    CHECK(km_anchor_get(anchors[0]).v == 2);
    CHECK(km_anchor_get(anchors[1]).v == 2);
    CHECK(km_anchor_get(anchors[2]).v == 2);
    CHECK(km_anchor_get(anchors[3]).v == 2);
    CHECK(km_anchor_get(anchors[4]).v == 5);
    CHECK(km_anchor_get(anchors[5]).v == 5);
    CHECK(km_document_undo(document, km_document_revision(document), &error) ==
          KM_OK);
    check_text(document, initial, sizeof(initial) - 1);
    for (i = 0; i < 6; ++i) {
        CHECK(km_anchor_get(anchors[i]).v == positions[i]);
    }
    CHECK(km_document_redo(document, km_document_revision(document), &error) ==
          KM_OK);
    CHECK(km_anchor_get(anchors[0]).v == 2);
    CHECK(km_anchor_get(anchors[1]).v == 2);
    CHECK(km_anchor_get(anchors[4]).v == 5);
    CHECK(km_anchor_get(anchors[5]).v == 5);
    km_document_destroy(document);

    document = make_document(initial, sizeof(initial) - 1);
    {
        KmAnchor *ends[4];
        size_t end_positions[] = {2, 2, 7, 7};
        KmAnchorAffinity end_affinity[] = {
            KM_ANCHOR_BEFORE, KM_ANCHOR_AFTER,
            KM_ANCHOR_BEFORE, KM_ANCHOR_AFTER,
        };
        KmSplice delete_splice = {{2}, {7}, NULL, 0, 0};
        for (i = 0; i < 4; ++i) {
            CHECK(km_anchor_create(document, (KmBytePos){end_positions[i]},
                                   end_affinity[i], &ends[i], &error) == KM_OK);
        }
        CHECK(apply(document, &delete_splice, 1) == KM_OK);
        for (i = 0; i < 4; ++i) {
            CHECK(km_anchor_get(ends[i]).v == 2);
        }
        CHECK(km_document_undo(document, km_document_revision(document),
                               &error) == KM_OK);
        for (i = 0; i < 4; ++i) {
            CHECK(km_anchor_get(ends[i]).v == end_positions[i]);
        }
    }
    km_document_destroy(document);
}

static void test_explicit_anchor_move_survives_undo(void) {
    static const uint8_t initial[] = "0123456789";
    static const uint8_t replacement[] = "XYZ";
    KmDocument *document = make_document(initial, sizeof(initial) - 1);
    KmAnchor *anchor = NULL;
    KmError error;
    KmSplice replace = {{2}, {7}, replacement, sizeof(replacement) - 1, 0};

    CHECK(km_anchor_create(document, (KmBytePos){4}, KM_ANCHOR_BEFORE,
                           &anchor, &error) == KM_OK);
    CHECK(apply(document, &replace, 1) == KM_OK);
    CHECK(km_anchor_set(anchor, (KmBytePos){3}, &error) == KM_OK);
    CHECK(km_document_undo(document, km_document_revision(document), &error) ==
          KM_OK);
    check_text(document, initial, sizeof(initial) - 1);
    CHECK(km_anchor_get(anchor).v == 2);
    {
        KmSplice no_op = {{2}, {2}, NULL, 0, 0};
        KmTxnMeta meta = {km_document_revision(document), 1};
        CHECK(km_document_apply_and_set_anchor(
                  document, &no_op, meta, anchor, (KmBytePos){5}, &error) ==
              KM_OK);
        CHECK(km_anchor_get(anchor).v == 5);
    }
    km_document_destroy(document);
}

static void test_batch_anchor_moves_are_atomic(void) {
    static const uint8_t initial[] = "abcdef";
    static const uint8_t final[] = "aXdeYZf";
    static const uint8_t x[] = "X";
    static const uint8_t yz[] = "YZ";
    static const uint8_t bang[] = "!";
    KmDocument *document = make_document(initial, sizeof(initial) - 1);
    KmAnchor *first = NULL;
    KmAnchor *second = NULL;
    KmError error;
    KmSplice splices[] = {
        {{5}, {5}, yz, sizeof(yz) - 1, 0},
        {{1}, {3}, x, sizeof(x) - 1, 0},
    };
    KmAnchorMove moves[2];
    KmSplice insert = {{0}, {0}, bang, sizeof(bang) - 1, 0};
    KmRevision revision;

    CHECK(km_anchor_create(document, (KmBytePos){2}, KM_ANCHOR_BEFORE,
                           &first, &error) == KM_OK);
    CHECK(km_anchor_create(document, (KmBytePos){6}, KM_ANCHOR_AFTER,
                           &second, &error) == KM_OK);
    moves[0] = (KmAnchorMove){first, {6}};
    moves[1] = (KmAnchorMove){second, {1}};
    CHECK(km_document_apply_and_set_anchors(
              document, splices, 2,
              (KmTxnMeta){km_document_revision(document), 9}, moves, 2,
              &error) == KM_OK);
    check_text(document, final, sizeof(final) - 1);
    CHECK(km_anchor_get(first).v == 6);
    CHECK(km_anchor_get(second).v == 1);
    CHECK(km_document_undo(document, km_document_revision(document), &error) ==
          KM_OK);
    check_text(document, initial, sizeof(initial) - 1);
    CHECK(km_anchor_get(first).v == 2);
    CHECK(km_anchor_get(second).v == 6);
    CHECK(km_document_redo(document, km_document_revision(document), &error) ==
          KM_OK);
    check_text(document, final, sizeof(final) - 1);
    CHECK(km_anchor_get(first).v == 6);
    CHECK(km_anchor_get(second).v == 1);

    revision = km_document_revision(document);
    moves[0] = (KmAnchorMove){first, {0}};
    moves[1] = (KmAnchorMove){first, {1}};
    CHECK(km_document_apply_and_set_anchors(
              document, &insert, 1, (KmTxnMeta){revision, 9}, moves, 2,
              &error) == KM_ERR_INVALID);
    moves[0] = (KmAnchorMove){first, {99}};
    CHECK(km_document_apply_and_set_anchors(
              document, &insert, 1, (KmTxnMeta){revision, 9}, moves, 1,
              &error) == KM_ERR_INVALID);
    CHECK(km_document_revision(document) == revision);
    check_text(document, final, sizeof(final) - 1);
    CHECK(km_anchor_get(first).v == 6);
    CHECK(km_anchor_get(second).v == 1);
    km_document_destroy(document);
}

static void test_batch_sort_same_point_and_relations(void) {
    static const uint8_t initial[] = "abcd";
    static const uint8_t expected[] = "XbABcdZ";
    static const uint8_t x[] = "X";
    static const uint8_t a[] = "A";
    static const uint8_t b[] = "B";
    static const uint8_t z[] = "Z";
    KmDocument *document = make_document(initial, sizeof(initial) - 1);
    KmSplice splices[] = {
        {{4}, {4}, z, 1, 0},
        {{2}, {2}, b, 1, 1},
        {{0}, {1}, x, 1, 0},
        {{2}, {2}, a, 1, 0},
    };
    KmSplice bad_overlap[] = {
        {{0}, {2}, x, 1, 0},
        {{1}, {3}, x, 1, 0},
    };
    KmSplice bad_left[] = {
        {{0}, {2}, x, 1, 0},
        {{0}, {0}, a, 1, 0},
    };
    CHECK(apply(document, splices, sizeof(splices) / sizeof(splices[0])) ==
          KM_OK);
    check_text(document, expected, sizeof(expected) - 1);
    {
        KmError error;
        CHECK(km_document_undo(document, km_document_revision(document),
                               &error) == KM_OK);
        check_text(document, initial, sizeof(initial) - 1);
        CHECK(km_document_redo(document, km_document_revision(document),
                               &error) == KM_OK);
        check_text(document, expected, sizeof(expected) - 1);
    }
    CHECK(apply(document, bad_overlap,
                sizeof(bad_overlap) / sizeof(bad_overlap[0])) ==
          KM_ERR_INVALID);
    CHECK(apply(document, bad_left,
                sizeof(bad_left) / sizeof(bad_left[0])) == KM_ERR_INVALID);
    check_text(document, expected, sizeof(expected) - 1);
    km_document_destroy(document);

    document = make_document(initial, sizeof(initial) - 1);
    {
        KmSplice right[] = {
            {{0}, {2}, x, 1, 0},
            {{2}, {2}, a, 1, 0},
        };
        static const uint8_t right_expected[] = "XAcd";
        CHECK(apply(document, right, 2) == KM_OK);
        check_text(document, right_expected, sizeof(right_expected) - 1);
    }
    km_document_destroy(document);
}

static void test_failure_atomicity_and_revision(void) {
    static const uint8_t initial[] = "hello";
    static const uint8_t insert[] = "!";
    KmDocument *document = make_document(initial, sizeof(initial) - 1);
    KmAnchor *anchor = NULL;
    KmError error;
    KmRevision revision;
    KmStateId state;
    KmSplice splice = {{5}, {5}, insert, 1, 0};
    KmTxnMeta stale = {99, 0};
    CHECK(km_anchor_create(document, (KmBytePos){3}, KM_ANCHOR_AFTER,
                           &anchor, &error) == KM_OK);
    revision = km_document_revision(document);
    state = km_document_history_state(document);
    CHECK(km_document_apply(document, &splice, 1, stale, &error) ==
          KM_ERR_CONFLICT);
    CHECK(km_document_revision(document) == revision);
    CHECK(km_document_history_state(document) == state);
    CHECK(km_anchor_get(anchor).v == 3);
    check_text(document, initial, sizeof(initial) - 1);
    CHECK(km_document_apply(document, NULL, 0,
                            (KmTxnMeta){revision, 0}, &error) == KM_OK);
    CHECK(km_document_revision(document) == revision);
    {
        KmSplice empty = {{2}, {2}, NULL, 0, 0};
        CHECK(km_document_apply(document, &empty, 1,
                                (KmTxnMeta){revision, 0}, &error) == KM_OK);
        CHECK(km_document_revision(document) == revision);
        CHECK(km_document_history_state(document) == state);
        CHECK(!km_document_can_undo(document));
    }
    CHECK(km_anchor_set(anchor, (KmBytePos){4}, &error) == KM_OK);
    CHECK(km_document_revision(document) == revision);
    km_document_destroy(document);
}

typedef struct {
    size_t position;
    KmAnchorAffinity affinity;
} ModelAnchor;

static uint32_t random_u32(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static size_t model_transform(size_t position, KmAnchorAffinity affinity,
                              size_t start, size_t end, size_t insert_len) {
    size_t deleted = end - start;
    if (deleted == 0) {
        if (position > start ||
            (position == start && affinity == KM_ANCHOR_AFTER)) {
            return position + insert_len;
        }
        return position;
    }
    if (position >= end) {
        return insert_len >= deleted
                   ? position + (insert_len - deleted)
                   : position - (deleted - insert_len);
    }
    return position > start ? start : position;
}

static void model_apply_batch(uint8_t *model, size_t *model_len,
                              const KmSplice *splices, size_t count,
                              ModelAnchor *anchors, size_t anchor_count,
                              size_t capacity) {
    ptrdiff_t delta = 0;
    size_t i;
    size_t j;

    for (i = 0; i < count; ++i) {
        size_t start = delta >= 0
                           ? splices[i].start.v + (size_t)delta
                           : splices[i].start.v - (size_t)(-delta);
        size_t end = delta >= 0
                         ? splices[i].end.v + (size_t)delta
                         : splices[i].end.v - (size_t)(-delta);
        size_t deleted = end - start;
        CHECK(*model_len - deleted + splices[i].insert_len <= capacity);
        memmove(model + start + splices[i].insert_len, model + end,
                *model_len - end);
        if (splices[i].insert_len != 0) {
            memcpy(model + start, splices[i].insert, splices[i].insert_len);
        }
        *model_len = *model_len - deleted + splices[i].insert_len;
        for (j = 0; j < anchor_count; ++j) {
            anchors[j].position = model_transform(
                anchors[j].position, anchors[j].affinity,
                start, end, splices[i].insert_len);
        }
        delta += (ptrdiff_t)splices[i].insert_len - (ptrdiff_t)deleted;
    }
}

static void test_logical_iterator(void) {
    static const uint8_t initial[] = "abcdef";
    static const uint8_t inserted[] = "XY";
    static const uint8_t expected[] = "abcXYXYdef";
    KmDocument *document = make_document(initial, sizeof(initial) - 1);
    KmTextIter iterator;
    KmError error;
    KmSplice splice = {{3}, {3}, inserted, sizeof(inserted) - 1, 0};
    uint8_t bytes[2];
    size_t count;
    bool eof;

    CHECK(apply(document, &splice, 1) == KM_OK);
    CHECK(km_document_iter_init(document, (KmBytePos){2}, &iterator,
                                &error) == KM_OK);
    CHECK(km_text_iter_read(&iterator, bytes, sizeof(bytes), &count, &eof,
                            &error) == KM_OK);
    CHECK(count == 2 && !eof && memcmp(bytes, "cX", 2) == 0);
    CHECK(km_text_iter_read(&iterator, bytes, sizeof(bytes), &count, &eof,
                            &error) == KM_OK);
    CHECK(count == 2 && !eof && memcmp(bytes, "Yd", 2) == 0);
    CHECK(km_text_iter_read(&iterator, bytes, sizeof(bytes), &count, &eof,
                            &error) == KM_OK);
    CHECK(count == 2 && eof && memcmp(bytes, "ef", 2) == 0);
    CHECK(km_text_iter_read(&iterator, NULL, 0, &count, &eof, &error) ==
          KM_OK);
    CHECK(count == 0 && eof);

    CHECK(km_document_iter_init(document, (KmBytePos){0}, &iterator,
                                &error) == KM_OK);
    CHECK(km_document_apply(document, &splice, 1,
                            (KmTxnMeta){km_document_revision(document), 1},
                            &error) == KM_OK);
    CHECK(km_text_iter_read(&iterator, bytes, sizeof(bytes), &count, &eof,
                            &error) == KM_ERR_CONFLICT);
    CHECK(iterator.position.v == 0 && count == 0 && !eof);

    CHECK(km_document_iter_init(document, (KmBytePos){0}, &iterator,
                                &error) == KM_OK);
    CHECK(km_document_undo(document, km_document_revision(document), &error) ==
          KM_OK);
    CHECK(km_text_iter_read(&iterator, bytes, sizeof(bytes), &count, &eof,
                            &error) == KM_ERR_CONFLICT);
    CHECK(iterator.position.v == 0);

    CHECK(km_document_iter_init(document, (KmBytePos){0}, &iterator,
                                &error) == KM_OK);
    CHECK(km_document_redo(document, km_document_revision(document), &error) ==
          KM_OK);
    CHECK(km_text_iter_read(&iterator, bytes, sizeof(bytes), &count, &eof,
                            &error) == KM_ERR_CONFLICT);
    CHECK(iterator.position.v == 0);
    check_text(document, expected, sizeof(expected) - 1);
    km_document_destroy(document);

    {
        static const uint8_t utf8[] = {'a', 0xe4, 0xb8, 0xad};
        static const uint8_t cjk[] = {0xe4, 0xb8, 0xad};
        KmSplice prepend = {{0}, {0}, cjk, sizeof(cjk), 0};
        document = make_document(utf8, sizeof(utf8));
        CHECK(km_document_iter_init(document, (KmBytePos){2}, &iterator,
                                    &error) == KM_ERR_INVALID);
        CHECK(km_document_iter_init(document, (KmBytePos){1}, &iterator,
                                    &error) == KM_OK);
        CHECK(km_text_iter_read(&iterator, bytes, sizeof(bytes), &count, &eof,
                                &error) == KM_OK);
        CHECK(count == 2 && !eof && memcmp(bytes, "\xe4\xb8", 2) == 0);
        CHECK(km_text_iter_read(&iterator, bytes, sizeof(bytes), &count, &eof,
                                &error) == KM_OK);
        CHECK(count == 1 && eof && bytes[0] == 0xad);
        CHECK(km_document_iter_init(document, (KmBytePos){1}, &iterator,
                                    &error) == KM_OK);
        CHECK(apply(document, &prepend, 1) == KM_OK);
        CHECK(km_text_iter_read(&iterator, bytes, sizeof(bytes), &count, &eof,
                                &error) == KM_ERR_CONFLICT);
        CHECK(iterator.position.v == 1);
        km_document_destroy(document);
    }
}

static size_t model_boundaries(const uint8_t *text, size_t len,
                               size_t *positions) {
    size_t count = 1;
    size_t i;

    positions[0] = 0;
    for (i = 1; i < len; ++i) {
        if ((text[i] & 0xc0u) != 0x80u) positions[count++] = i;
    }
    positions[count++] = len;
    return count;
}

static size_t random_utf8_payload(uint32_t *state, uint8_t *payload) {
    static const uint8_t pieces[][4] = {
        {0}, {'e'}, {0xcc, 0x81}, {0xe4, 0xb8, 0xad},
        {0xf0, 0x9f, 0x98, 0x80},
    };
    static const size_t lengths[] = {1, 1, 2, 3, 4};
    size_t piece_count = 1 + random_u32(state) % 3u;
    size_t len = 0;
    size_t i;

    for (i = 0; i < piece_count; ++i) {
        size_t index = random_u32(state) % (sizeof(lengths) / sizeof(lengths[0]));
        memcpy(payload + len, pieces[index], lengths[index]);
        len += lengths[index];
    }
    return len;
}

static void test_random_utf8_reference_model(void) {
    static const uint8_t initial[] = {
        'A', 0, 'e', 0xcc, 0x81, 0xe4, 0xb8, 0xad,
        0xf0, 0x9f, 0x98, 0x80, 'B',
    };
    uint8_t model[8192];
    ModelAnchor model_anchors[6];
    KmAnchor *anchors[6];
    KmDocument *document = make_document(initial, sizeof(initial));
    KmError error;
    uint32_t random_state = 0x7554u;
    size_t model_len = sizeof(initial);
    size_t step;
    size_t i;

    memcpy(model, initial, model_len);
    for (i = 0; i < 6; ++i) {
        static const size_t initial_positions[] = {0, 2, 3, 5, 8, 13};
        model_anchors[i].position = initial_positions[i];
        model_anchors[i].affinity = (i & 1u) != 0
                                        ? KM_ANCHOR_AFTER
                                        : KM_ANCHOR_BEFORE;
        CHECK(km_anchor_create(document,
                               (KmBytePos){model_anchors[i].position},
                               model_anchors[i].affinity, &anchors[i],
                               &error) == KM_OK);
    }
    for (step = 0; step < 200; ++step) {
        uint8_t before[8192];
        uint8_t payload[12];
        size_t boundaries[8193];
        ModelAnchor anchors_before[6];
        size_t boundary_count = model_boundaries(model, model_len, boundaries);
        size_t start_index = random_u32(&random_state) % boundary_count;
        size_t end_index = start_index +
                           random_u32(&random_state) %
                               (boundary_count - start_index);
        size_t payload_len = random_utf8_payload(&random_state, payload);
        size_t before_len = model_len;
        KmSplice splice = {{boundaries[start_index]}, {boundaries[end_index]},
                            payload, payload_len, 0};

        CHECK(model_len - (splice.end.v - splice.start.v) + payload_len <=
              sizeof(model));
        memcpy(before, model, model_len);
        memcpy(anchors_before, model_anchors, sizeof(model_anchors));
        CHECK(apply(document, &splice, 1) == KM_OK);
        model_apply_batch(model, &model_len, &splice, 1, model_anchors, 6,
                          sizeof(model));
        check_text(document, model, model_len);
        for (i = 0; i <= model_len; ++i) {
            bool boundary = i == 0 || i == model_len ||
                            (model[i] & 0xc0u) != 0x80u;
            CHECK(km_document_is_boundary(document, (KmBytePos){i}) ==
                  boundary);
        }
        for (i = 0; i < 6; ++i) {
            CHECK(km_anchor_get(anchors[i]).v == model_anchors[i].position);
        }
        CHECK(km_document_undo(document, km_document_revision(document),
                               &error) == KM_OK);
        check_text(document, before, before_len);
        for (i = 0; i < 6; ++i) {
            CHECK(km_anchor_get(anchors[i]).v == anchors_before[i].position);
        }
        CHECK(km_document_redo(document, km_document_revision(document),
                               &error) == KM_OK);
        check_text(document, model, model_len);
        for (i = 0; i < 6; ++i) {
            CHECK(km_anchor_get(anchors[i]).v == model_anchors[i].position);
        }
    }
    km_document_destroy(document);
}

static void test_random_batch_reference_model(void) {
    uint8_t initial[256];
    uint8_t model[4096];
    ModelAnchor model_anchors[4];
    KmAnchor *anchors[4];
    KmDocument *document;
    KmError error;
    uint32_t random_state = 0x62617463u;
    size_t model_len = sizeof(initial);
    size_t step;
    size_t i;

    for (i = 0; i < sizeof(initial); ++i) initial[i] = (uint8_t)('a' + i % 26);
    memcpy(model, initial, sizeof(initial));
    document = make_document(initial, sizeof(initial));
    for (i = 0; i < 4; ++i) {
        model_anchors[i].position = i * (sizeof(initial) / 3);
        if (model_anchors[i].position > sizeof(initial)) {
            model_anchors[i].position = sizeof(initial);
        }
        model_anchors[i].affinity = (i & 1u) != 0
                                        ? KM_ANCHOR_AFTER
                                        : KM_ANCHOR_BEFORE;
        CHECK(km_anchor_create(document,
                               (KmBytePos){model_anchors[i].position},
                               model_anchors[i].affinity, &anchors[i],
                               &error) == KM_OK);
    }
    for (step = 0; step < 250; ++step) {
        KmSplice canonical[4];
        KmSplice submitted[4];
        uint8_t payload[4][4];
        uint8_t before[4096];
        ModelAnchor anchors_before[4];
        size_t before_len = model_len;
        size_t count = 1 + random_u32(&random_state) % 4u;
        size_t cursor = 0;
        size_t j;

        memcpy(before, model, model_len);
        memcpy(anchors_before, model_anchors, sizeof(model_anchors));
        for (i = 0; i < count; ++i) {
            size_t available = model_len - cursor;
            size_t start = cursor + random_u32(&random_state) % (available + 1);
            size_t delete_limit = model_len - start;
            size_t deleted;
            size_t insert_len;
            if (delete_limit > 3) delete_limit = 3;
            deleted = random_u32(&random_state) % (delete_limit + 1);
            insert_len = random_u32(&random_state) % 4u;
            if (deleted == 0 && insert_len == 0) insert_len = 1;
            for (j = 0; j < insert_len; ++j) {
                payload[i][j] = (uint8_t)('A' + random_u32(&random_state) % 26u);
            }
            canonical[i] = (KmSplice){{start}, {start + deleted}, payload[i],
                                      insert_len, (uint32_t)i};
            cursor = deleted == 0 && start < model_len
                         ? start + 1
                         : start + deleted;
        }
        memcpy(submitted, canonical, count * sizeof(*submitted));
        for (i = count; i > 1; --i) {
            size_t other = random_u32(&random_state) % i;
            KmSplice temporary = submitted[i - 1];
            submitted[i - 1] = submitted[other];
            submitted[other] = temporary;
        }
        CHECK(apply(document, submitted, count) == KM_OK);
        model_apply_batch(model, &model_len, canonical, count,
                          model_anchors, 4, sizeof(model));
        check_text(document, model, model_len);
        for (i = 0; i < 4; ++i) {
            CHECK(km_anchor_get(anchors[i]).v == model_anchors[i].position);
        }
        CHECK(km_document_undo(document, km_document_revision(document),
                               &error) == KM_OK);
        check_text(document, before, before_len);
        for (i = 0; i < 4; ++i) {
            CHECK(km_anchor_get(anchors[i]).v == anchors_before[i].position);
        }
        CHECK(km_document_redo(document, km_document_revision(document),
                               &error) == KM_OK);
        check_text(document, model, model_len);
        for (i = 0; i < 4; ++i) {
            CHECK(km_anchor_get(anchors[i]).v == model_anchors[i].position);
        }
    }
    km_document_destroy(document);
}

static void test_large_batch_growth(void) {
    uint8_t initial[4000];
    uint8_t insert_a[3000];
    uint8_t insert_b[3000];
    uint8_t expected[11000];
    size_t expected_len = sizeof(initial);
    KmDocument *document;
    KmError error;
    KmSplice splices[3];
    size_t i;

    memset(initial, 'i', sizeof(initial));
    memset(insert_a, 'A', sizeof(insert_a));
    memset(insert_b, 'B', sizeof(insert_b));
    memcpy(expected, initial, sizeof(initial));
    splices[0] = (KmSplice){{100}, {100}, insert_a, sizeof(insert_a), 0};
    splices[1] = (KmSplice){{2000}, {2000}, insert_b, sizeof(insert_b), 0};
    splices[2] = (KmSplice){{3000}, {3900}, NULL, 0, 0};
    document = make_document(initial, sizeof(initial));
    CHECK(apply(document, splices, 3) == KM_OK);
    model_apply_batch(expected, &expected_len, splices, 3, NULL, 0,
                      sizeof(expected));
    check_text(document, expected, expected_len);
    CHECK(km_document_undo(document, km_document_revision(document), &error) ==
          KM_OK);
    check_text(document, initial, sizeof(initial));
    CHECK(km_document_redo(document, km_document_revision(document), &error) ==
          KM_OK);
    check_text(document, expected, expected_len);
    for (i = 0; i < sizeof(insert_a); ++i) {
        CHECK(insert_a[i] == 'A' && insert_b[i] == 'B');
    }
    km_document_destroy(document);
}

static void test_random_reference_model(void) {
    static const uint8_t initial[] = "seed";
    uint8_t model[8192] = "seed";
    uint8_t final_model[8192];
    size_t model_len = sizeof(initial) - 1;
    size_t initial_positions[] = {0, 0, 2, 2, 4, 4};
    ModelAnchor model_anchors[6];
    KmAnchor *anchors[6];
    size_t final_positions[6];
    KmDocument *document = make_document(initial, sizeof(initial) - 1);
    KmError error;
    uint32_t random_state = 0x6b6d2026u;
    size_t history_count = 0;
    size_t step;
    size_t i;

    for (i = 0; i < 6; ++i) {
        KmAnchorAffinity affinity = (i & 1u) != 0
                                        ? KM_ANCHOR_AFTER
                                        : KM_ANCHOR_BEFORE;
        model_anchors[i].position = initial_positions[i];
        model_anchors[i].affinity = affinity;
        CHECK(km_anchor_create(document, (KmBytePos){initial_positions[i]},
                               affinity, &anchors[i], &error) == KM_OK);
    }
    for (step = 0; step < 1000; ++step) {
        uint8_t insert[4];
        size_t start = (size_t)(random_u32(&random_state) %
                                (uint32_t)(model_len + 1));
        size_t end = start +
                     (size_t)(random_u32(&random_state) %
                              (uint32_t)(model_len - start + 1));
        size_t insert_len = (size_t)(random_u32(&random_state) % 4u);
        size_t deleted = end - start;
        KmSplice splice;

        if (model_len - deleted + insert_len > sizeof(model)) insert_len = 0;
        for (i = 0; i < insert_len; ++i) {
            insert[i] = (uint8_t)('a' + random_u32(&random_state) % 26u);
        }
        splice = (KmSplice){{start}, {end}, insert, insert_len, 0};
        CHECK(apply(document, &splice, 1) == KM_OK);
        if (deleted == 0 && insert_len == 0) continue;
        memmove(model + start + insert_len, model + end, model_len - end);
        memcpy(model + start, insert, insert_len);
        model_len = model_len - deleted + insert_len;
        for (i = 0; i < 6; ++i) {
            model_anchors[i].position = model_transform(
                model_anchors[i].position, model_anchors[i].affinity,
                start, end, insert_len);
        }
        ++history_count;
        check_text(document, model, model_len);
        for (i = 0; i < 6; ++i) {
            CHECK(km_anchor_get(anchors[i]).v == model_anchors[i].position);
        }
    }
    memcpy(final_model, model, model_len);
    for (i = 0; i < 6; ++i) final_positions[i] = model_anchors[i].position;

    for (step = 0; step < history_count; ++step) {
        CHECK(km_document_undo(document, km_document_revision(document),
                               &error) == KM_OK);
    }
    check_text(document, initial, sizeof(initial) - 1);
    for (i = 0; i < 6; ++i) {
        CHECK(km_anchor_get(anchors[i]).v == initial_positions[i]);
    }
    for (step = 0; step < history_count; ++step) {
        CHECK(km_document_redo(document, km_document_revision(document),
                               &error) == KM_OK);
    }
    check_text(document, final_model, model_len);
    for (i = 0; i < 6; ++i) {
        CHECK(km_anchor_get(anchors[i]).v == final_positions[i]);
    }
    km_document_destroy(document);
}

static void test_undo_redo_and_branch_truncation(void) {
    static const uint8_t initial[] = "abcd";
    static const uint8_t one[] = "AB";
    static const uint8_t two[] = "!";
    static const uint8_t after_one[] = "aABd";
    static const uint8_t after_two[] = "aABd!";
    static const uint8_t branch_text[] = "?aABd";
    KmDocument *document = make_document(initial, sizeof(initial) - 1);
    KmError error;
    KmSplice first = {{1}, {3}, one, sizeof(one) - 1, 0};
    KmSplice second = {{4}, {4}, two, sizeof(two) - 1, 0};
    KmSplice branch = {{0}, {0}, (const uint8_t *)"?", 1, 0};
    CHECK(apply(document, &first, 1) == KM_OK);
    CHECK(km_document_revision(document) == 1);
    check_text(document, after_one, sizeof(after_one) - 1);
    CHECK(apply(document, &second, 1) == KM_OK);
    CHECK(km_document_revision(document) == 2);
    check_text(document, after_two, sizeof(after_two) - 1);
    CHECK(km_document_undo(document, 2, &error) == KM_OK);
    CHECK(km_document_revision(document) == 3);
    check_text(document, after_one, sizeof(after_one) - 1);
    CHECK(km_document_redo(document, 3, &error) == KM_OK);
    CHECK(km_document_revision(document) == 4);
    check_text(document, after_two, sizeof(after_two) - 1);
    CHECK(km_document_undo(document, 4, &error) == KM_OK);
    CHECK(apply(document, &branch, 1) == KM_OK);
    check_text(document, branch_text, sizeof(branch_text) - 1);
    CHECK(!km_document_can_redo(document));
    CHECK(km_document_redo(document, km_document_revision(document), &error) ==
          KM_ERR_INVALID);
    km_document_destroy(document);
}

int main(void) {
    test_utf8_and_boundaries();
    test_gap_copy_and_insert_affinity();
    test_anchor_delete_replace_and_restore();
    test_explicit_anchor_move_survives_undo();
    test_batch_anchor_moves_are_atomic();
    test_batch_sort_same_point_and_relations();
    test_failure_atomicity_and_revision();
    test_undo_redo_and_branch_truncation();
    test_logical_iterator();
    test_random_reference_model();
    test_random_batch_reference_model();
    test_random_utf8_reference_model();
    test_large_batch_growth();
    puts("document tests passed");
    return 0;
}
