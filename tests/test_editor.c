#include "editor.h"

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

static KmBuffer *make_base(const uint8_t *text, size_t len)
{
    KmBuffer *buffer = NULL;
    KmError error;

    CHECK(km_buffer_create_base(text, len, &buffer, &error) == KM_OK);
    return buffer;
}

static void check_text(const KmBuffer *buffer, const uint8_t *expected,
                       size_t expected_len)
{
    const KmDocument *document = km_buffer_document(buffer);
    uint8_t *actual = expected_len == 0 ? NULL : malloc(expected_len);
    KmError error;

    CHECK(km_document_len(document) == expected_len);
    CHECK(km_document_copy(document, (KmBytePos){0}, expected_len, actual,
                           &error) == KM_OK);
    CHECK(expected_len == 0 || memcmp(actual, expected, expected_len) == 0);
    free(actual);
}

static void test_ownership(void)
{
    KmBuffer *base = make_base((const uint8_t *)"abc", 3);
    KmBuffer *indirect = NULL;
    KmView *view = NULL;
    KmError error;

    CHECK(km_buffer_create_indirect(base, &indirect, &error) == KM_OK);
    CHECK(km_buffer_destroy(base, &error) == KM_ERR_CONFLICT);
    CHECK(km_view_create(indirect, &view, &error) == KM_OK);
    CHECK(km_buffer_destroy(indirect, &error) == KM_ERR_CONFLICT);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(indirect, &error) == KM_OK);
    CHECK(km_buffer_destroy(base, &error) == KM_OK);
}

static void test_views_and_narrowing(void)
{
    KmBuffer *buffer = make_base((const uint8_t *)"abcdef", 6);
    KmView *first = NULL;
    KmView *second = NULL;
    KmError error;

    CHECK(km_view_create(buffer, &first, &error) == KM_OK);
    CHECK(km_view_create(buffer, &second, &error) == KM_OK);
    CHECK(km_view_set_point(first, (KmBytePos){1}, &error) == KM_OK);
    CHECK(km_view_set_point(second, (KmBytePos){5}, &error) == KM_OK);
    CHECK(km_buffer_narrow(buffer, (KmBytePos){2}, (KmBytePos){4}, &error) ==
          KM_OK);
    CHECK(km_view_point(first).v == 2);
    CHECK(km_view_point(second).v == 4);
    CHECK(km_view_set_point(first, (KmBytePos){1}, &error) == KM_ERR_INVALID);
    CHECK(km_buffer_widen(buffer, &error) == KM_OK);
    CHECK(km_buffer_accessible_start(buffer).v == 0);
    CHECK(km_buffer_accessible_end(buffer).v == 6);
    CHECK(km_view_destroy(first, &error) == KM_OK);
    CHECK(km_view_destroy(second, &error) == KM_OK);
    CHECK(km_view_create(buffer, &first, &error) == KM_OK);
    CHECK(km_view_point(first).v == 4);
    CHECK(km_view_destroy(first, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_indirect_and_utf8_narrowing(void)
{
    static const uint8_t text[] = {'A', 0xe2, 0x82, 0xac, 'B'};
    KmBuffer *base = make_base(text, sizeof(text));
    KmBuffer *indirect = NULL;
    KmView *view = NULL;
    KmError error;

    CHECK(km_buffer_create_indirect(base, &indirect, &error) == KM_OK);
    CHECK(km_view_create(indirect, &view, &error) == KM_OK);
    CHECK(km_buffer_document(base) == km_buffer_document(indirect));
    CHECK(km_buffer_narrow(base, (KmBytePos){1}, (KmBytePos){4}, &error) ==
          KM_OK);
    CHECK(km_buffer_accessible_start(indirect).v == 0);
    CHECK(km_buffer_accessible_end(indirect).v == sizeof(text));
    CHECK(km_buffer_narrow(indirect, (KmBytePos){2}, (KmBytePos){4}, &error) ==
          KM_ERR_INVALID);
    CHECK(km_buffer_accessible_start(indirect).v == 0);
    CHECK(km_buffer_accessible_end(indirect).v == sizeof(text));
    CHECK(km_view_point(view).v == 0);
    CHECK(km_buffer_narrow(indirect, (KmBytePos){1}, (KmBytePos){4}, &error) ==
          KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    CHECK(km_buffer_accessible_start(indirect).v == 1);
    CHECK(km_buffer_accessible_end(indirect).v == 4);
    CHECK(km_buffer_narrow(indirect, (KmBytePos){0}, (KmBytePos){4}, &error) ==
          KM_ERR_INVALID);
    CHECK(km_buffer_accessible_start(indirect).v == 1);
    CHECK(km_buffer_accessible_end(indirect).v == 4);
    CHECK(km_view_point(view).v == 1);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(indirect, &error) == KM_OK);
    CHECK(km_buffer_destroy(base, &error) == KM_OK);
}

static void test_saved_point(void)
{
    KmBuffer *buffer = make_base((const uint8_t *)"abcdef", 6);
    KmView *view = NULL;
    KmError error;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){5}, &error) == KM_OK);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_narrow(buffer, (KmBytePos){1}, (KmBytePos){3}, &error) ==
          KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 3);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 3);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_read_only_and_modified(void)
{
    KmBuffer *base = make_base((const uint8_t *)"abc", 3);
    KmBuffer *indirect = NULL;
    KmError error;

    CHECK(!km_buffer_is_read_only(base));
    km_buffer_set_read_only(base, true);
    CHECK(km_buffer_is_read_only(base));
    CHECK(!km_buffer_is_modified(base));
    CHECK(km_buffer_create_indirect(base, &indirect, &error) == KM_OK);
    CHECK(!km_buffer_is_modified(indirect));
    CHECK(!km_buffer_is_read_only(indirect));
    km_buffer_set_read_only(indirect, true);
    CHECK(km_buffer_is_read_only(indirect));
    CHECK(km_buffer_destroy(indirect, &error) == KM_OK);
    CHECK(km_buffer_destroy(base, &error) == KM_OK);
}

static void test_char_commands_utf8_boundaries(void)
{
    static const uint8_t text[] = {
        'A', 0, 'e', 0xcc, 0x81, 0xe4, 0xb8, 0xad,
        0xf0, 0x9f, 0x98, 0x80,
    };
    static const uint8_t without_cjk[] = {
        'A', 0, 'e', 0xcc, 0x81, 0xf0, 0x9f, 0x98, 0x80,
    };
    static const uint8_t without_emoji[] = {
        'A', 0, 'e', 0xcc, 0x81, 0xe4, 0xb8, 0xad,
    };
    static const size_t forward_points[] = {1, 2, 3, 5, 8, 12};
    static const size_t backward_points[] = {8, 5, 3, 2, 1, 0};
    KmBuffer *buffer = make_base(text, sizeof(text));
    KmView *view = NULL;
    KmError error;
    KmRevision revision;
    size_t i;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    for (i = 0; i < sizeof(forward_points) / sizeof(forward_points[0]); ++i) {
        CHECK(km_view_forward_char(view, &error) == KM_OK);
        CHECK(km_view_point(view).v == forward_points[i]);
    }
    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(km_view_forward_char(view, &error) == KM_ERR_INVALID);
    CHECK(km_view_point(view).v == sizeof(text));
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);
    for (i = 0; i < sizeof(backward_points) / sizeof(backward_points[0]); ++i) {
        CHECK(km_view_backward_char(view, &error) == KM_OK);
        CHECK(km_view_point(view).v == backward_points[i]);
    }
    CHECK(km_view_backward_char(view, &error) == KM_ERR_INVALID);
    CHECK(km_view_point(view).v == 0);

    CHECK(km_view_set_point(view, (KmBytePos){5}, &error) == KM_OK);
    CHECK(km_view_delete_char(view, &error) == KM_OK);
    check_text(buffer, without_cjk, sizeof(without_cjk));
    CHECK(km_view_point(view).v == 5);
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, text, sizeof(text));

    CHECK(km_view_set_point(view, (KmBytePos){sizeof(text)}, &error) == KM_OK);
    CHECK(km_view_delete_backward_char(view, &error) == KM_OK);
    check_text(buffer, without_emoji, sizeof(without_emoji));
    CHECK(km_view_point(view).v == 8);
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, text, sizeof(text));
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_edit_commands_undo_redo_and_view_points(void)
{
    static const uint8_t initial[] = {'a', 'b'};
    static const uint8_t inserted[] = {'X', 0, 'e', 0xcc, 0x81};
    static const uint8_t after_insert[] = {
        'a', 'X', 0, 'e', 0xcc, 0x81, 'b',
    };
    static const uint8_t after_forward_delete[] = {
        'a', 'X', 0, 'e', 0xcc, 0x81,
    };
    static const uint8_t after_backward_delete[] = {'a', 'X', 0, 'e'};
    KmBuffer *buffer = make_base(initial, sizeof(initial));
    KmView *view = NULL;
    KmView *other = NULL;
    KmError error;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_create(buffer, &other, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    CHECK(km_view_set_point(other, (KmBytePos){1}, &error) == KM_OK);
    CHECK(km_view_insert_utf8_block(view, inserted, sizeof(inserted), &error) ==
          KM_OK);
    check_text(buffer, after_insert, sizeof(after_insert));
    CHECK(km_view_point(view).v == 6);
    CHECK(km_view_point(other).v == 6);
    CHECK(km_buffer_is_modified(buffer));
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, initial, sizeof(initial));
    CHECK(km_view_point(view).v == 1);
    CHECK(km_view_point(other).v == 1);
    CHECK(!km_buffer_is_modified(buffer));
    CHECK(km_view_redo(view, &error) == KM_OK);
    check_text(buffer, after_insert, sizeof(after_insert));
    CHECK(km_view_point(view).v == 6);
    CHECK(km_view_point(other).v == 6);
    CHECK(km_buffer_is_modified(buffer));

    CHECK(km_view_delete_char(view, &error) == KM_OK);
    check_text(buffer, after_forward_delete, sizeof(after_forward_delete));
    CHECK(km_view_point(view).v == 6);
    CHECK(km_view_point(other).v == 6);
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, after_insert, sizeof(after_insert));
    CHECK(km_view_point(view).v == 6);
    CHECK(km_view_point(other).v == 6);
    CHECK(km_view_redo(view, &error) == KM_OK);
    check_text(buffer, after_forward_delete, sizeof(after_forward_delete));
    CHECK(km_view_point(view).v == 6);
    CHECK(km_view_point(other).v == 6);

    CHECK(km_view_delete_backward_char(view, &error) == KM_OK);
    check_text(buffer, after_backward_delete, sizeof(after_backward_delete));
    CHECK(km_view_point(view).v == 4);
    CHECK(km_view_point(other).v == 4);
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, after_forward_delete, sizeof(after_forward_delete));
    CHECK(km_view_point(view).v == 6);
    CHECK(km_view_point(other).v == 6);
    CHECK(km_view_redo(view, &error) == KM_OK);
    check_text(buffer, after_backward_delete, sizeof(after_backward_delete));
    CHECK(km_view_point(view).v == 4);
    CHECK(km_view_point(other).v == 4);
    CHECK(km_view_destroy(other, &error) == KM_OK);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_edit_scope_and_atomic_rejection(void)
{
    static const uint8_t initial[] = "abcd";
    static const uint8_t after_begv_insert[] = "aXbcd";
    static const uint8_t after_zv_insert[] = "aXbcYd";
    static const uint8_t after_narrow_delete[] = "aXcYd";
    static const uint8_t invalid_utf8[] = {0xc0, 0x80};
    KmBuffer *buffer = make_base(initial, sizeof(initial) - 1);
    KmView *view = NULL;
    KmError error;
    KmRevision revision;
    KmStateId state;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_buffer_narrow(buffer, (KmBytePos){1}, (KmBytePos){3}, &error) ==
          KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    CHECK(km_view_insert_utf8_block(view, (const uint8_t *)"X", 1, &error) ==
          KM_OK);
    check_text(buffer, after_begv_insert, sizeof(after_begv_insert) - 1);
    CHECK(km_buffer_accessible_start(buffer).v == 1);
    CHECK(km_buffer_accessible_end(buffer).v == 4);
    CHECK(km_view_point(view).v == 2);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(km_view_insert_utf8_block(view, (const uint8_t *)"Y", 1, &error) ==
          KM_OK);
    check_text(buffer, after_zv_insert, sizeof(after_zv_insert) - 1);
    CHECK(km_buffer_accessible_end(buffer).v == 5);
    CHECK(km_view_point(view).v == 5);
    revision = km_document_revision(km_buffer_document(buffer));
    state = km_document_history_state(km_buffer_document(buffer));
    CHECK(km_view_forward_char(view, &error) == KM_ERR_INVALID);
    CHECK(km_view_delete_char(view, &error) == KM_ERR_INVALID);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);
    CHECK(km_document_history_state(km_buffer_document(buffer)) == state);
    CHECK(km_buffer_accessible_start(buffer).v == 1);
    CHECK(km_buffer_accessible_end(buffer).v == 5);
    check_text(buffer, after_zv_insert, sizeof(after_zv_insert) - 1);

    CHECK(km_buffer_narrow(buffer, (KmBytePos){2}, (KmBytePos){4}, &error) ==
          KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){2}, &error) == KM_OK);
    revision = km_document_revision(km_buffer_document(buffer));
    state = km_document_history_state(km_buffer_document(buffer));
    CHECK(km_view_delete_backward_char(view, &error) == KM_ERR_INVALID);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);
    CHECK(km_document_history_state(km_buffer_document(buffer)) == state);
    CHECK(km_view_point(view).v == 2);
    CHECK(km_buffer_accessible_start(buffer).v == 2);
    CHECK(km_buffer_accessible_end(buffer).v == 4);
    CHECK(km_view_delete_char(view, &error) == KM_OK);
    check_text(buffer, after_narrow_delete, sizeof(after_narrow_delete) - 1);
    CHECK(km_buffer_accessible_start(buffer).v == 2);
    CHECK(km_buffer_accessible_end(buffer).v == 3);

    CHECK(km_buffer_widen(buffer, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    revision = km_document_revision(km_buffer_document(buffer));
    state = km_document_history_state(km_buffer_document(buffer));
    CHECK(km_view_insert_utf8_block(view, invalid_utf8, sizeof(invalid_utf8),
                                    &error) == KM_ERR_INVALID);
    check_text(buffer, after_narrow_delete, sizeof(after_narrow_delete) - 1);
    CHECK(km_view_point(view).v == 1);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);
    CHECK(km_document_history_state(km_buffer_document(buffer)) == state);

    km_buffer_set_read_only(buffer, true);
    CHECK(km_view_insert_utf8_block(view, (const uint8_t *)"Q", 1, &error) ==
          KM_ERR_PERMISSION);
    CHECK(km_view_undo(view, &error) == KM_ERR_PERMISSION);
    check_text(buffer, after_narrow_delete, sizeof(after_narrow_delete) - 1);
    CHECK(km_view_point(view).v == 1);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);
    CHECK(km_document_history_state(km_buffer_document(buffer)) == state);
    CHECK(km_buffer_accessible_start(buffer).v == 0);
    CHECK(km_buffer_accessible_end(buffer).v == sizeof(after_narrow_delete) - 1);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_undo_redo_respect_narrowing(void)
{
    static const uint8_t initial[] = "abcdef";
    static const uint8_t edited[] = "abcdXef";
    KmBuffer *base = make_base(initial, sizeof(initial) - 1);
    KmBuffer *indirect = NULL;
    KmView *base_view = NULL;
    KmView *indirect_view = NULL;
    KmError error;
    KmRevision revision;
    KmStateId state;

    CHECK(km_buffer_create_indirect(base, &indirect, &error) == KM_OK);
    CHECK(km_view_create(base, &base_view, &error) == KM_OK);
    CHECK(km_view_create(indirect, &indirect_view, &error) == KM_OK);
    CHECK(km_buffer_narrow(base, (KmBytePos){0}, (KmBytePos){3}, &error) ==
          KM_OK);
    CHECK(km_view_set_point(indirect_view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(km_view_insert_utf8_block(indirect_view, (const uint8_t *)"X", 1,
                                    &error) == KM_OK);
    check_text(base, edited, sizeof(edited) - 1);

    revision = km_document_revision(km_buffer_document(base));
    state = km_document_history_state(km_buffer_document(base));
    CHECK(km_view_undo(base_view, &error) == KM_ERR_INVALID);
    check_text(base, edited, sizeof(edited) - 1);
    CHECK(km_document_revision(km_buffer_document(base)) == revision);
    CHECK(km_document_history_state(km_buffer_document(base)) == state);

    CHECK(km_buffer_widen(base, &error) == KM_OK);
    CHECK(km_view_undo(base_view, &error) == KM_OK);
    check_text(base, initial, sizeof(initial) - 1);
    CHECK(km_buffer_narrow(base, (KmBytePos){0}, (KmBytePos){3}, &error) ==
          KM_OK);
    revision = km_document_revision(km_buffer_document(base));
    state = km_document_history_state(km_buffer_document(base));
    CHECK(km_view_redo(base_view, &error) == KM_ERR_INVALID);
    check_text(base, initial, sizeof(initial) - 1);
    CHECK(km_document_revision(km_buffer_document(base)) == revision);
    CHECK(km_document_history_state(km_buffer_document(base)) == state);

    CHECK(km_buffer_widen(base, &error) == KM_OK);
    CHECK(km_view_redo(base_view, &error) == KM_OK);
    check_text(base, edited, sizeof(edited) - 1);
    CHECK(km_view_destroy(indirect_view, &error) == KM_OK);
    CHECK(km_view_destroy(base_view, &error) == KM_OK);
    CHECK(km_buffer_destroy(indirect, &error) == KM_OK);
    CHECK(km_buffer_destroy(base, &error) == KM_OK);
}

int main(void)
{
    test_ownership();
    test_views_and_narrowing();
    test_indirect_and_utf8_narrowing();
    test_saved_point();
    test_read_only_and_modified();
    test_char_commands_utf8_boundaries();
    test_edit_commands_undo_redo_and_view_points();
    test_edit_scope_and_atomic_rejection();
    test_undo_redo_respect_narrowing();
    return 0;
}
