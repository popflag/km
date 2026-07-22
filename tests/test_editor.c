#include "editor.h"
#include "configuration.h"

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
    if (expected_len != 0 && memcmp(actual, expected, expected_len) != 0) {
        size_t i;
        fprintf(stderr, "text mismatch (%zu bytes):", expected_len);
        for (i = 0; i < expected_len; ++i) fprintf(stderr, " %02x", actual[i]);
        fputc('\n', stderr);
    }
    CHECK(expected_len == 0 || memcmp(actual, expected, expected_len) == 0);
    free(actual);
}

static KmStatus dispatch_key_repeat(KmCommandLoop *loop, KmView *view,
                                    uint32_t codepoint, uint32_t modifiers,
                                    uint32_t repeat, KmError *error)
{
    KmEvent event = {0};

    event.kind = KM_EVENT_KEY;
    event.codepoint = codepoint;
    event.modifiers = modifiers;
    event.repeat = repeat;
    return km_command_loop_dispatch(loop, view, &event, error);
}

static KmStatus dispatch_key(KmCommandLoop *loop, KmView *view,
                             uint32_t codepoint, uint32_t modifiers,
                             KmError *error)
{
    return dispatch_key_repeat(loop, view, codepoint, modifiers, 1, error);
}

static KmStatus dispatch_text(KmCommandLoop *loop, KmView *view,
                              uint32_t codepoint, uint32_t repeat,
                              KmError *error)
{
    KmEvent event = {0};

    event.kind = KM_EVENT_TEXT;
    event.codepoint = codepoint;
    event.repeat = repeat;
    return km_command_loop_dispatch(loop, view, &event, error);
}

static KmStatus dispatch_text_block(KmCommandLoop *loop, KmView *view,
                                    const uint8_t *text, size_t len,
                                    KmError *error)
{
    KmEvent event = {0};

    event.kind = KM_EVENT_TEXT;
    event.repeat = 1;
    event.text = text;
    event.text_len = len;
    return km_command_loop_dispatch(loop, view, &event, error);
}

static KmStatus dispatch_paste(KmCommandLoop *loop, KmView *view,
                               const uint8_t *text, size_t len,
                               KmError *error)
{
    KmEvent event = {0};

    event.kind = KM_EVENT_PASTE;
    event.repeat = 1;
    event.text = text;
    event.text_len = len;
    return km_command_loop_dispatch(loop, view, &event, error);
}

static KmStatus dispatch_ignored(KmCommandLoop *loop, KmView *view,
                                 KmEventKind kind, KmError *error)
{
    KmEvent event = {0};

    event.kind = kind;
    return km_command_loop_dispatch(loop, view, &event, error);
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

static void test_view_buffer_switching(void)
{
    static const char unicode_name[] = "buffer-\xe4\xb8\xad";
    KmBuffer *first = make_base((const uint8_t *)"first", 5);
    KmBuffer *second = make_base((const uint8_t *)"second", 6);
    KmView *view = NULL;
    KmView *setup = NULL;
    KmError error;

    CHECK(km_view_create(second, &setup, &error) == KM_OK);
    CHECK(km_view_set_point(setup, (KmBytePos){2}, &error) == KM_OK);
    CHECK(km_view_destroy(setup, &error) == KM_OK);
    CHECK(km_view_create(first, &view, &error) == KM_OK);
    CHECK(km_buffer_set_name(first, unicode_name, &error) == KM_OK);
    CHECK(strcmp(km_buffer_name(first), unicode_name) == 0);
    CHECK(km_buffer_set_name(first, "bad\nname", &error) == KM_ERR_INVALID);
    CHECK(strcmp(km_buffer_name(first), unicode_name) == 0);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(km_view_buffer(view) == first);
    CHECK(km_view_set_buffer(view, second, &error) == KM_OK);
    CHECK(km_view_buffer(view) == second);
    CHECK(km_view_point(view).v == 2);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    CHECK(km_view_set_buffer(view, first, &error) == KM_OK);
    CHECK(km_view_point(view).v == 4);
    CHECK(km_view_set_buffer(view, NULL, &error) == KM_ERR_INVALID);
    CHECK(km_view_buffer(view) == first);
    CHECK(km_view_point(view).v == 4);
    CHECK(km_view_set_buffer(view, second, &error) == KM_OK);
    CHECK(km_view_point(view).v == 1);
    CHECK(km_buffer_destroy(first, &error) == KM_OK);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(second, &error) == KM_OK);
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

static void test_word_commands(void)
{
    static const uint8_t text[] = {
        ' ', ' ', 'f', 'o', 'o', '_', 'b', 'a', 'r', ' ',
        0xe4, 0xb8, 0xad, 'e', 0xcc, 0x81, '!',
    };
    KmBuffer *buffer = make_base(text, sizeof(text));
    KmView *view = NULL;
    KmError error;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_forward_word(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 5);
    CHECK(km_view_forward_word(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 9);
    CHECK(km_view_forward_word(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 16);
    CHECK(km_view_forward_word(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == sizeof(text));
    CHECK(km_view_backward_word(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 10);
    CHECK(km_view_backward_word(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 6);

    CHECK(km_buffer_narrow(buffer, (KmBytePos){6}, (KmBytePos){16}, &error) ==
          KM_OK);
    CHECK(km_view_forward_word(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 9);
    CHECK(km_view_forward_word(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 16);
    CHECK(km_view_forward_word(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 16);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_buffer_edge_commands(void)
{
    static const uint8_t text[] = "abc\ndef";
    KmBuffer *buffer = make_base(text, sizeof(text) - 1);
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){3}, &error) == KM_OK);
    CHECK(km_view_end_of_buffer(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == sizeof(text) - 1);
    CHECK(km_view_beginning_of_buffer(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 0);

    CHECK(km_buffer_narrow(buffer, (KmBytePos){1}, (KmBytePos){6}, &error) ==
          KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){3}, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '>', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 6);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_END_OF_BUFFER);
    CHECK(dispatch_key(loop, view, '<', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 1);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_BEGINNING_OF_BUFFER);
    CHECK(dispatch_key(loop, view, '>', KM_MOD_ALT | KM_MOD_SHIFT, &error) ==
          KM_OK);
    CHECK(km_view_point(view).v == 6);
    CHECK(dispatch_key(loop, view, '<', KM_MOD_ALT | KM_MOD_SHIFT, &error) ==
          KM_OK);
    CHECK(km_view_point(view).v == 1);

    CHECK(dispatch_key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '>', KM_MOD_ALT, &error) == KM_ERR_INVALID);
    CHECK(km_view_point(view).v == 1);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_paragraph_and_common_edit_commands(void)
{
    static const uint8_t paragraphs[] =
        "one\ntwo\n\nthree\nfour\n\n\nfive";
    static const uint8_t words[] = "one two three";
    static const uint8_t after_kill[] = " two three";
    static const uint8_t after_backward_kill[] = "one two ";
    static const uint8_t open_result[] = "a\n\nb";
    static const uint8_t transpose_text[] = {
        'a', 0xe4, 0xb8, 0xad, 'b',
    };
    static const uint8_t transpose_result[] = {
        0xe4, 0xb8, 0xad, 'a', 'b',
    };
    KmBuffer *buffer = make_base(paragraphs, sizeof(paragraphs) - 1);
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;
    KmRevision revision;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '}', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 8);
    CHECK(dispatch_key(loop, view, '}', KM_MOD_ALT | KM_MOD_SHIFT, &error) ==
          KM_OK);
    CHECK(km_view_point(view).v == 20);
    CHECK(dispatch_key(loop, view, '{', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 8);
    CHECK(dispatch_key(loop, view, '{', KM_MOD_ALT | KM_MOD_SHIFT, &error) ==
          KM_OK);
    CHECK(km_view_point(view).v == 0);
    CHECK(dispatch_key(loop, view, '4', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '}', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == sizeof(paragraphs) - 1);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(words, sizeof(words) - 1);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_key(loop, view, 'd', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision + 1);
    check_text(buffer, after_kill, sizeof(after_kill) - 1);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, words, sizeof(words) - 1);
    CHECK(km_view_set_point(view, (KmBytePos){sizeof(words) - 1}, &error) ==
          KM_OK);
    CHECK(dispatch_key(loop, view, 0x7f, KM_MOD_ALT, &error) == KM_OK);
    check_text(buffer, after_backward_kill, sizeof(after_backward_kill) - 1);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, words, sizeof(words) - 1);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base((const uint8_t *)"ab", 2);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'o', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision + 1);
    check_text(buffer, open_result, sizeof(open_result) - 1);
    CHECK(km_view_point(view).v == 1);
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"ab", 2);
    CHECK(km_view_point(view).v == 1);
    CHECK(km_view_redo(view, &error) == KM_OK);
    check_text(buffer, open_result, sizeof(open_result) - 1);
    CHECK(km_view_point(view).v == 1);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(transpose_text, sizeof(transpose_text));
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 't', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, transpose_result, sizeof(transpose_result));
    CHECK(km_view_point(view).v == 4);
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, transpose_text, sizeof(transpose_text));
    CHECK(km_view_point(view).v == 1);
    CHECK(km_view_redo(view, &error) == KM_OK);
    check_text(buffer, transpose_result, sizeof(transpose_result));
    CHECK(km_view_point(view).v == 4);
    CHECK(km_view_undo(view, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 't', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer,
               (const uint8_t *)"\xe4\xb8\xad" "ba", 5);
    CHECK(km_view_point(view).v == 5);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base((const uint8_t *)"abc", 3);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){3}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '-', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 't', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"bac", 3);
    CHECK(km_view_point(view).v == 1);
    CHECK(km_view_undo(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 3);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base((const uint8_t *)"a \t b", 5);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){3}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '\\', KM_MOD_ALT, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"ab", 2);
    CHECK(km_view_undo(view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){3}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '1', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '\\', KM_MOD_ALT, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"a b", 3);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_kill_ring_and_added_movement_commands(void)
{
    static const uint8_t words[] = "one two three";
    static const uint8_t transposed[] = "two three one";
    static const uint8_t sentences[] =
        "One. Two!  Three?\nFour. \xe4\xba\x94\xe3\x80\x82\xe5\x85\xad";
    KmBuffer *buffer = make_base(words, sizeof(words) - 1);
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;
    char prompt[64];
    uint8_t ring_source[61];
    uint8_t ring_expected[62];
    size_t i;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'd', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'd', KM_MOD_ALT, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)" three", 6);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, words, sizeof(words) - 1);

    CHECK(km_view_set_point(view, (KmBytePos){0}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){3}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'w', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){7}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'w', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){13}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"one two threetwo", 16);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_ALT, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"one two threeone", 16);
    CHECK(km_view_point(view).v == 16);
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_ALT, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"one two threeone two", 20);
    CHECK(km_view_point(view).v == 20);
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"one two threeone", 16);
    CHECK(km_view_point(view).v == 16);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    for (i = 0; i < sizeof(ring_source); ++i) {
        ring_source[i] = (uint8_t)('!' + i);
    }
    buffer = make_base(ring_source, sizeof(ring_source));
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    for (i = 0; i < sizeof(ring_source); ++i) {
        CHECK(km_view_set_point(view, (KmBytePos){i}, &error) == KM_OK);
        CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
        CHECK(km_view_set_point(view, (KmBytePos){i + 1}, &error) == KM_OK);
        CHECK(dispatch_key(loop, view, 'w', KM_MOD_ALT, &error) == KM_OK);
    }
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '-', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_ALT, &error) == KM_OK);
    memcpy(ring_expected, ring_source, sizeof(ring_source));
    ring_expected[sizeof(ring_source)] = ring_source[1];
    check_text(buffer, ring_expected, sizeof(ring_expected));
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(words, sizeof(words) - 1);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){13}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 0x7f, KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 0x7f, KM_MOD_ALT, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"one ", 4);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, words, sizeof(words) - 1);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(words, sizeof(words) - 1);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 't', KM_MOD_ALT, &error) == KM_OK);
    check_text(buffer, transposed, sizeof(transposed) - 1);
    CHECK(km_view_point(view).v == 13);
    CHECK(km_view_undo(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 4);
    CHECK(km_view_set_point(view, (KmBytePos){7}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '-', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 't', KM_MOD_ALT, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"two one three", 13);
    CHECK(km_view_point(view).v == 3);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(sentences, sizeof(sentences) - 1);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'e', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 9);
    CHECK(dispatch_key(loop, view, 'e', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 17);
    CHECK(dispatch_key(loop, view, 'a', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 11);
    CHECK(km_view_set_point(view, (KmBytePos){24}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'e', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 30);

    CHECK(km_buffer_narrow(buffer, (KmBytePos){6}, (KmBytePos){30}, &error) ==
          KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){17}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'h', 0, &error) == KM_OK);
    CHECK(km_view_point(view).v == 6);
    CHECK(km_buffer_mark_active(buffer));
    CHECK(km_buffer_mark(buffer).v == 30);
    CHECK(dispatch_key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'g', 0, &error) == KM_OK);
    CHECK(km_view_point(view).v == 18);
    CHECK(dispatch_key(loop, view, '1', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'g', 0, &error) == KM_ERR_INVALID);
    CHECK(km_view_point(view).v == 18);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'g', 0, &error) == KM_OK);
    CHECK(km_command_loop_prompt_active(loop));
    km_command_loop_format_prompt(loop, prompt, sizeof(prompt));
    CHECK(strcmp(prompt, "Goto line: ") == 0);
    CHECK(dispatch_text(loop, view, '2', 1, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_view_point(view).v == 18);
    CHECK(dispatch_key(loop, view, 'l', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_RECENTER);
    CHECK(!km_command_loop_request_has_argument(loop));
    km_command_loop_clear_request(loop);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_mark_ring_and_counted_kill_line(void)
{
    static const uint8_t words[] = "one two three";
    static const uint8_t lines[] = "aa\nbb\ncc\n";
    static const uint8_t transpose_text[] =
        "a\xe4\xb8\xad" "b\xe6\x96\x87";
    static const uint8_t transpose_result[] =
        "a\xe6\x96\x87" "b\xe4\xb8\xad";
    KmBuffer *buffer = make_base(words, sizeof(words) - 1);
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){8}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){12}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 12);
    CHECK(km_buffer_mark(buffer).v == 8);
    CHECK(!km_buffer_mark_active(buffer));
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 8);
    CHECK(km_buffer_mark(buffer).v == 4);
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 4);
    CHECK(km_buffer_mark(buffer).v == 12);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 12);
    CHECK(km_buffer_mark(buffer).v == 4);
    CHECK(km_buffer_mark_active(buffer));

    CHECK(km_view_set_point(view, (KmBytePos){0}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '@', KM_MOD_ALT | KM_MOD_SHIFT, &error) ==
          KM_OK);
    CHECK(km_view_point(view).v == 0);
    CHECK(km_buffer_mark(buffer).v == 3);
    CHECK(dispatch_key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '@', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_buffer_mark(buffer).v == 7);
    CHECK(km_view_set_point(view, (KmBytePos){5}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '<', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 0);
    CHECK(km_buffer_mark(buffer).v == 5);
    CHECK(!km_buffer_mark_active(buffer));
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 5);
    CHECK(km_buffer_mark(buffer).v == 7);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(words, sizeof(words) - 1);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){12}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_buffer_narrow(buffer, (KmBytePos){2}, (KmBytePos){10}, &error) ==
          KM_OK);
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 10);
    CHECK(km_buffer_mark(buffer).v == 2);
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 2);
    CHECK(km_buffer_mark(buffer).v == 10);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base((const uint8_t *)"abcdefghijklmnopqr", 18);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    {
        size_t i;
        for (i = 0; i < 18; ++i) {
            CHECK(km_view_set_point(view, (KmBytePos){i}, &error) == KM_OK);
            CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
        }
        for (i = 0; i < 17; ++i) {
            CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
            CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
        }
    }
    CHECK(km_view_point(view).v == 1);
    CHECK(km_buffer_mark(buffer).v == 17);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(transpose_text, sizeof(transpose_text) - 1);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){5}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '0', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 't', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, transpose_result, sizeof(transpose_result) - 1);
    CHECK(km_view_point(view).v == 5);
    CHECK(km_buffer_mark(buffer).v == 1);
    CHECK(km_buffer_mark_active(buffer));
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, transpose_text, sizeof(transpose_text) - 1);
    CHECK(km_view_point(view).v == 1);
    CHECK(km_buffer_mark(buffer).v == 5);
    CHECK(km_view_redo(view, &error) == KM_OK);
    CHECK(km_view_point(view).v == 5);
    CHECK(km_buffer_mark(buffer).v == 1);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(lines, sizeof(lines) - 1);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '0', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'k', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"aa\nb\ncc\n", 8);
    CHECK(km_view_point(view).v == 3);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, lines, sizeof(lines) - 1);
    CHECK(km_view_set_point(view, (KmBytePos){0}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '0', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'k', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, lines, sizeof(lines) - 1);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, lines, sizeof(lines) - 1);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(lines, sizeof(lines) - 1);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'k', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"aa\nb", 4);
    CHECK(km_view_point(view).v == 4);
    CHECK(km_view_set_point(view, (KmBytePos){0}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, lines, sizeof(lines) - 1);
    CHECK(km_buffer_mark(buffer).v == 4);
    CHECK(!km_buffer_mark_active(buffer));
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"aa\nb", 4);
    CHECK(km_buffer_mark(buffer).v == 0);
    CHECK(km_view_redo(view, &error) == KM_OK);
    check_text(buffer, lines, sizeof(lines) - 1);
    CHECK(km_buffer_mark(buffer).v == 4);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(lines, sizeof(lines) - 1);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '-', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'k', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"b\ncc\n", 5);
    CHECK(km_view_point(view).v == 0);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, lines, sizeof(lines) - 1);
    km_command_loop_destroy(loop);
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

static void test_command_loop_events_and_trie(void)
{
    static const uint8_t initial[] = {'a', 'b'};
    static const uint8_t with_emoji[] = {
        'a', 0xf0, 0x9f, 0x98, 0x80, 'b',
    };
    static const uint8_t paste[] = {0, 'e', 0xcc, 0x81};
    static const uint8_t with_paste[] = {
        'a', 0xf0, 0x9f, 0x98, 0x80, 0, 'e', 0xcc, 0x81, 'b',
    };
    static const uint8_t invalid_utf8[] = {0xc0, 0x80};
    static const KmKeyStroke alt_h[] = {{'h', KM_MOD_ALT}};
    static const KmKeyStroke ctrl_c[] = {{'c', KM_MOD_CTRL}};
    static const KmKeyStroke ctrl_c_f[] = {
        {'c', KM_MOD_CTRL}, {'f', 0},
    };
    KmBuffer *buffer = make_base(initial, sizeof(initial));
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;
    KmRevision revision;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_NONE);
    CHECK(dispatch_key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'f', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 2);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_FORWARD_WORD);
    CHECK(dispatch_key(loop, view, 'f', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 2);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_FORWARD_WORD);
    CHECK(dispatch_key(loop, view, 'b', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 0);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_BACKWARD_WORD);
    CHECK(km_command_loop_bind_key(loop, KM_KEYMAP_GLOBAL, alt_h, 1,
                                   "forward-char", &error) == KM_OK);
    CHECK(km_command_loop_bind_key(loop, KM_KEYMAP_MINIBUFFER, alt_h, 1,
                                   "forward-char", &error) == KM_ERR_INVALID);
    CHECK(dispatch_key(loop, view, 'h', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 1);
    CHECK(km_command_loop_bind_key(loop, KM_KEYMAP_GLOBAL, alt_h, 1,
                                   "backward-char", &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'h', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 0);
    CHECK(km_command_loop_bind_key(loop, KM_KEYMAP_GLOBAL, ctrl_c_f, 2,
                                   "forward-char", &error) == KM_OK);
    CHECK(km_command_loop_bind_key(loop, KM_KEYMAP_GLOBAL, ctrl_c, 1,
                                   "forward-char", &error) ==
          KM_ERR_CONFLICT);
    CHECK(dispatch_key(loop, view, 'c', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'f', 0, &error) == KM_OK);
    CHECK(km_view_point(view).v == 1);
    CHECK(dispatch_key(loop, view, 'b', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 0);

    CHECK(dispatch_key(loop, view, 'f', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 1);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_FORWARD_CHAR);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'z', 0, &error) == KM_ERR_INVALID);
    CHECK(km_view_point(view).v == 1);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_FORWARD_CHAR);

    CHECK(dispatch_text(loop, view, 0x1f600, 1, &error) == KM_OK);
    check_text(buffer, with_emoji, sizeof(with_emoji));
    CHECK(km_view_point(view).v == 5);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_INSERT_UTF8_BLOCK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 'u', 1, &error) == KM_OK);
    check_text(buffer, initial, sizeof(initial));
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_UNDO);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'r', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, with_emoji, sizeof(with_emoji));
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_REDO);

    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_paste(loop, view, paste, sizeof(paste), &error) == KM_OK);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision + 1);
    check_text(buffer, with_paste, sizeof(with_paste));
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_PASTE);
    CHECK(dispatch_key(loop, view, '/', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, with_emoji, sizeof(with_emoji));
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_UNDO);

    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_paste(loop, view, invalid_utf8, sizeof(invalid_utf8),
                         &error) == KM_ERR_INVALID);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);
    check_text(buffer, with_emoji, sizeof(with_emoji));
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_UNDO);
    CHECK(dispatch_text(loop, view, 0xd800, 1, &error) == KM_ERR_INVALID);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_UNDO);

    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base((const uint8_t *)"ab\ncd", 5);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){2}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'w', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) == KM_OK);
    km_command_loop_clear_quit(loop);
    CHECK(dispatch_key(loop, view, 'k', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"abcd", 4);
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"ab\ncd", 5);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_command_loop_prefix_arguments(void)
{
    static const uint8_t initial[] = "abcdefghijklmnopqrst";
    static const uint8_t after_delete[] = "abcdefghijknopqrst";
    KmBuffer *buffer = make_base(initial, sizeof(initial) - 1);
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;
    KmRevision revision;
    size_t i;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);

    CHECK(dispatch_key(loop, view, 'v', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_SCROLL_UP);
    CHECK(!km_command_loop_request_has_argument(loop));
    CHECK(km_command_loop_request_argument(loop) == 1);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_SCROLL_UP);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, '3', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'v', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_SCROLL_DOWN);
    CHECK(km_command_loop_request_has_argument(loop));
    CHECK(km_command_loop_request_argument(loop) == 3);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_SCROLL_DOWN);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '-', 0, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'v', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_SCROLL_UP);
    CHECK(km_command_loop_request_page_opposite(loop));
    km_command_loop_clear_request(loop);

    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'f', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 16);
    CHECK(dispatch_key(loop, view, '9', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'f', KM_MOD_CTRL, &error) == KM_ERR_INVALID);
    CHECK(km_view_point(view).v == sizeof(initial) - 1);
    CHECK(km_view_set_point(view, (KmBytePos){16}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_ignored(loop, view, KM_EVENT_RESIZE, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '3', 0, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'b', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 13);

    CHECK(dispatch_key(loop, view, '-', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '2', 0, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'f', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 11);
    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_key(loop, view, '0', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'd', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);
    CHECK(km_view_point(view).v == 11);

    CHECK(dispatch_key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'd', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision + 1);
    check_text(buffer, after_delete, sizeof(after_delete) - 1);
    CHECK(km_view_point(view).v == 11);
    CHECK(dispatch_key(loop, view, '/', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, initial, sizeof(initial) - 1);
    CHECK(km_view_point(view).v == 11);

    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_key(loop, view, '9', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '9', 0, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'd', KM_MOD_CTRL, &error) == KM_ERR_INVALID);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);
    check_text(buffer, initial, sizeof(initial) - 1);
    CHECK(km_view_point(view).v == 11);

    CHECK(dispatch_key(loop, view, '1', KM_MOD_ALT, &error) == KM_OK);
    for (i = 0; i < 18; ++i) {
        CHECK(dispatch_key(loop, view, '0', 0, &error) == KM_OK);
    }
    CHECK(dispatch_key(loop, view, '0', 0, &error) == KM_ERR_INVALID);
    CHECK(dispatch_key(loop, view, 'f', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 12);

    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_command_loop_quit_requested(loop));
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_FORWARD_CHAR);
    km_command_loop_clear_quit(loop);
    CHECK(!km_command_loop_quit_requested(loop));
    CHECK(dispatch_key(loop, view, 'f', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 13);

    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'c', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_EXIT);
    km_command_loop_clear_request(loop);

    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'q', 0, &error) == KM_ERR_INVALID);
    CHECK(dispatch_key(loop, view, 'f', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 14);

    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_counted_movement_boundaries(void)
{
    static const uint8_t text[] = "One.  Two.\n\nThree";
    KmBuffer *buffer = make_base(text, sizeof(text) - 1);
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);

    CHECK(dispatch_key(loop, view, '9', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'f', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == sizeof(text) - 1);

    CHECK(km_view_set_point(view, (KmBytePos){0}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '9', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '}', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == sizeof(text) - 1);

    CHECK(km_view_set_point(view, (KmBytePos){0}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '9', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'e', KM_MOD_ALT, &error) == KM_ERR_INVALID);
    CHECK(km_view_point(view).v == sizeof(text) - 1);

    CHECK(dispatch_key(loop, view, '9', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'a', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 0);

    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_command_loop_atomic_edits(void)
{
    static const uint8_t three_emoji[] = {
        0xf0, 0x9f, 0x98, 0x80,
        0xf0, 0x9f, 0x98, 0x80,
        0xf0, 0x9f, 0x98, 0x80,
    };
    static const uint8_t narrowed_text[] = "abcdef";
    static const uint8_t text_block[] = {'A', 0, 'B'};
    static const uint8_t invalid_utf8[] = {0xc0, 0x80};
    KmBuffer *buffer = make_base(NULL, 0);
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;
    KmRevision revision;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '3', 0, &error) == KM_OK);
    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_text(loop, view, 0x1f600, 1, &error) == KM_OK);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision + 1);
    check_text(buffer, three_emoji, sizeof(three_emoji));
    CHECK(dispatch_key(loop, view, '/', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, NULL, 0);

    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_text_block(loop, view, text_block, sizeof(text_block),
                              &error) == KM_OK);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision + 1);
    check_text(buffer, text_block, sizeof(text_block));
    CHECK(dispatch_key(loop, view, '/', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, NULL, 0);
    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_text_block(loop, view, invalid_utf8, sizeof(invalid_utf8),
                              &error) == KM_ERR_INVALID);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);

    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_paste(loop, view, (const uint8_t *)"x", 1, &error) ==
          KM_ERR_INVALID);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);
    CHECK(dispatch_paste(loop, view, (const uint8_t *)"x", 1, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"x", 1);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(narrowed_text, sizeof(narrowed_text) - 1);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_buffer_narrow(buffer, (KmBytePos){1}, (KmBytePos){4}, &error) ==
          KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){3}, &error) == KM_OK);
    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'd', KM_MOD_CTRL, &error) == KM_ERR_INVALID);
    CHECK(km_view_point(view).v == 3);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);
    check_text(buffer, narrowed_text, sizeof(narrowed_text) - 1);
    km_buffer_set_read_only(buffer, true);
    CHECK(dispatch_text(loop, view, 'q', 1, &error) == KM_ERR_PERMISSION);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_NONE);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);

    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_lines_mark_kill_and_yank(void)
{
    static const uint8_t lines[] = {
        'a', '\t', 0xe4, 0xb8, 0xad, '\n',
        'x', 'y', '\n', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    static const uint8_t region_text[] = {
        'A', 0, 'B', '\n', 0xe4, 0xb8, 0xad, 'Z',
    };
    static const uint8_t after_kill[] = {'A', 'Z'};
    static const uint8_t indentation[] = {
        ' ', '\t', 'f', 'o', 'o', '\n',
        0xe3, 0x80, 0x80, 'b', 'a', 'r', '\n',
        ' ', ' ', ' ', '\n',
        ' ', 0xe1, 0x9a, 0x80, 'x', '\n',
        ' ', 0xe2, 0x80, 0xa8, 'x', '\n',
        ' ', 0xe2, 0x80, 0xa9, 'x', '\n',
    };
    KmBuffer *buffer = make_base(lines, sizeof(lines));
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;
    KmRevision revision;

    CHECK(strcmp(km_buffer_name(buffer), "*scratch*") == 0);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){5}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'n', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 8);
    CHECK(dispatch_key(loop, view, KM_KEY_DOWN, 0, &error) == KM_OK);
    CHECK(km_view_point(view).v == sizeof(lines));
    CHECK(dispatch_key(loop, view, KM_KEY_UP, 0, &error) == KM_OK);
    CHECK(km_view_point(view).v == 8);
    CHECK(dispatch_key(loop, view, 'a', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 6);
    CHECK(dispatch_key(loop, view, KM_KEY_END, 0, &error) == KM_OK);
    CHECK(km_view_point(view).v == 8);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(indentation, sizeof(indentation));
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'a', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 0);
    CHECK(dispatch_key(loop, view, 'm', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 2);
    CHECK(km_view_set_point(view, (KmBytePos){11}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'm', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 9);
    CHECK(km_view_set_point(view, (KmBytePos){14}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'm', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 16);
    CHECK(km_view_set_point(view, (KmBytePos){21}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'm', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 18);
    CHECK(km_view_set_point(view, (KmBytePos){27}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'm', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 24);
    CHECK(km_view_set_point(view, (KmBytePos){33}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'm', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 30);
    CHECK(km_buffer_narrow(buffer, (KmBytePos){1}, (KmBytePos){5}, &error) ==
          KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'm', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 2);
    CHECK(km_buffer_widen(buffer, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view,
                              (const uint8_t *)"back-to-indentation", 19,
                              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_view_point(view).v == 2);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base(region_text, sizeof(region_text));
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){1}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_buffer_mark_active(buffer));
    CHECK(km_buffer_mark(buffer).v == 1);
    CHECK(km_view_set_point(view, (KmBytePos){7}, &error) == KM_OK);
    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_key(loop, view, 'w', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision + 1);
    check_text(buffer, after_kill, sizeof(after_kill));
    CHECK(km_view_point(view).v == 1);
    CHECK(!km_buffer_mark_active(buffer));
    revision = km_document_revision(km_buffer_document(buffer));
    CHECK(dispatch_key(loop, view, 'y', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision + 1);
    check_text(buffer, region_text, sizeof(region_text));
    CHECK(km_view_point(view).v == 7);
    CHECK(dispatch_key(loop, view, '/', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, after_kill, sizeof(after_kill));
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(!km_buffer_mark_active(buffer));
    km_command_loop_clear_quit(loop);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 's', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_SAVE);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view,
                              (const uint8_t *)"save-buffers-kill-terminal",
                              26, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) ==
          KM_COMMAND_REQUEST_SAVE_ALL_EXIT);
    km_command_loop_clear_request(loop);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_NONE);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_incremental_search(void)
{
    static const uint8_t text[] = {
        'o', 'n', 'e', ' ', 0xe4, 0xb8, 0xad, ' ',
        't', 'w', 'o', ' ', 0xe4, 0xb8, 0xad,
    };
    static const uint8_t word[] = {'t', 'w', 'o'};
    KmBuffer *buffer = make_base(text, sizeof(text));
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;
    KmRevision revision;
    KmStateId state;
    char prompt[64];

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 's', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_command_loop_search_active(loop));
    CHECK(dispatch_text(loop, view, 0x4e2d, 1, &error) == KM_OK);
    CHECK(km_view_point(view).v == 4);
    km_command_loop_format_prompt(loop, prompt, sizeof(prompt));
    CHECK(strstr(prompt, "I-search: ") == prompt);
    CHECK(dispatch_key_repeat(loop, view, 's', KM_MOD_CTRL, 2, &error) ==
          KM_OK);
    CHECK(km_view_point(view).v == 4);
    km_command_loop_format_prompt(loop, prompt, sizeof(prompt));
    CHECK(strstr(prompt, "Wrapped I-search: ") == prompt);
    CHECK(dispatch_text(loop, view, ' ', 1, &error) == KM_OK);
    CHECK(km_view_point(view).v == 4);
    km_command_loop_format_prompt(loop, prompt, sizeof(prompt));
    CHECK(strstr(prompt, "Wrapped I-search: ") == prompt);
    CHECK(dispatch_key(loop, view, 0x7f, 0, &error) == KM_OK);
    CHECK(km_view_point(view).v == 4);
    CHECK(dispatch_key(loop, view, 'r', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 12);
    km_command_loop_format_prompt(loop, prompt, sizeof(prompt));
    CHECK(strstr(prompt, "Wrapped I-search backward: ") == prompt);
    CHECK(dispatch_key(loop, view, 0x7f, 0, &error) == KM_OK);
    CHECK(km_view_point(view).v == 0);
    CHECK(dispatch_key(loop, view, 's', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view, word, sizeof(word), &error) == KM_OK);
    CHECK(km_view_point(view).v == 8);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 0);
    CHECK(!km_command_loop_search_active(loop));
    km_command_loop_clear_quit(loop);
    CHECK(dispatch_key(loop, view, 's', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view, word, sizeof(word), &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(!km_command_loop_search_active(loop));
    CHECK(km_view_point(view).v == 8);
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_SEARCH_FORWARD);

    CHECK(km_view_set_point(view, (KmBytePos){sizeof(text)}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'r', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 0x4e2d, 1, &error) == KM_OK);
    CHECK(km_view_point(view).v == 12);
    km_command_loop_format_prompt(loop, prompt, sizeof(prompt));
    CHECK(strstr(prompt, "I-search backward: ") == prompt);
    CHECK(dispatch_key(loop, view, 'r', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 4);
    CHECK(dispatch_key(loop, view, 'r', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 12);
    km_command_loop_format_prompt(loop, prompt, sizeof(prompt));
    CHECK(strstr(prompt, "Wrapped I-search backward: ") == prompt);
    revision = km_document_revision(km_buffer_document(buffer));
    state = km_document_history_state(km_buffer_document(buffer));
    CHECK(dispatch_text(loop, view, '\n', 2, &error) == KM_OK);
    CHECK(km_document_revision(km_buffer_document(buffer)) == revision);
    CHECK(km_document_history_state(km_buffer_document(buffer)) == state);
    check_text(buffer, text, sizeof(text));
    CHECK(km_command_loop_last_command(loop) == KM_COMMAND_SEARCH_BACKWARD);

    CHECK(km_buffer_narrow(buffer, (KmBytePos){4},
                           (KmBytePos){sizeof(text)}, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){sizeof(text)}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'r', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 0x4e2d, 1, &error) == KM_OK);
    CHECK(km_view_point(view).v == 12);
    CHECK(dispatch_key(loop, view, 'r', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 4);
    CHECK(dispatch_key(loop, view, 'r', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 12);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == sizeof(text));
    km_command_loop_clear_quit(loop);

    CHECK(km_buffer_widen(buffer, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 's', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 0x4e2d, 1, &error) == KM_OK);
    CHECK(km_buffer_narrow(buffer, (KmBytePos){12},
                           (KmBytePos){sizeof(text)}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 's', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 12);
    CHECK(dispatch_key(loop, view, 0x7f, 0, &error) == KM_OK);
    CHECK(km_view_point(view).v == 12);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == 12);
    km_command_loop_clear_quit(loop);

    CHECK(km_view_set_point(view, (KmBytePos){sizeof(text)}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 's', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 'z', 1, &error) == KM_OK);
    km_command_loop_format_prompt(loop, prompt, sizeof(prompt));
    CHECK(strstr(prompt, "Failing I-search: ") == prompt);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_point(view).v == sizeof(text));
    km_command_loop_clear_quit(loop);

    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view,
                              (const uint8_t *)"isearch-backward", 16,
                              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_search_active(loop));
    km_command_loop_format_prompt(loop, prompt, sizeof(prompt));
    CHECK(strstr(prompt, "I-search backward: ") == prompt);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) == KM_OK);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_incremental_search_context(void)
{
    KmBuffer *first = make_base((const uint8_t *)"one two", 7);
    KmBuffer *second = make_base((const uint8_t *)"other", 5);
    KmView *view = NULL;
    KmView *writer = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;

    CHECK(km_view_create(first, &view, &error) == KM_OK);
    CHECK(km_view_create(first, &writer, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 's', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 't', 1, &error) == KM_OK);
    CHECK(km_view_point(view).v == 4);
    CHECK(km_view_insert_utf8_block(writer, (const uint8_t *)"x", 1,
                                    &error) == KM_OK);
    CHECK(km_view_point(view).v == 5);
    CHECK(dispatch_key(loop, view, 's', KM_MOD_CTRL, &error) ==
          KM_ERR_CONFLICT);
    CHECK(!km_command_loop_search_active(loop));
    CHECK(km_view_point(view).v == 5);

    CHECK(dispatch_key(loop, view, 's', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_view_set_buffer(view, second, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) ==
          KM_ERR_CONFLICT);
    CHECK(!km_command_loop_search_active(loop));
    CHECK(km_view_buffer(view) == second);
    CHECK(km_view_point(view).v == 0);

    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_view_destroy(writer, &error) == KM_OK);
    CHECK(km_buffer_destroy(first, &error) == KM_OK);
    CHECK(km_buffer_destroy(second, &error) == KM_OK);
}

static void test_minibuffer_requests(void)
{
    static const uint8_t cjk[] = {0xe4, 0xb8, 0xad};
    static const uint8_t invalid[] = {0xc0, 0x80};
    static const char *const buffer_matches[] = {"*scratch*"};
    static const char *const directory_match[] = {"/tmp/sub/"};
    static const KmKeyStroke alt_h[] = {{'h', KM_MOD_ALT}};
    KmBuffer *buffer = make_base(NULL, 0);
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;
    char prompt[128];

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_command_loop_bind_key(loop, KM_KEYMAP_MINIBUFFER, alt_h, 1,
                                   "minibuffer-clear", &error) == KM_OK);

    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key_repeat(loop, view, 'f', KM_MOD_CTRL, 2, &error) ==
          KM_OK);
    CHECK(km_command_loop_prompt_active(loop));
    km_command_loop_format_completions(loop, prompt, sizeof(prompt));
    CHECK(prompt[0] == '\0');
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_FIND_FILE);
    CHECK(km_command_loop_request_text(loop)[0] == '\0');
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'f', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'h', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view, (const uint8_t *)"note-", 5,
                              &error) == KM_OK);
    CHECK(dispatch_paste(loop, view, cjk, sizeof(cjk), &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 0x7f, 0, &error) == KM_OK);
    CHECK(dispatch_paste(loop, view, cjk, sizeof(cjk), &error) == KM_OK);
    CHECK(dispatch_key(loop, view, KM_KEY_TAB, 0, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_COMPLETE_FILE);
    CHECK(km_command_loop_prompt_active(loop));
    km_command_loop_clear_request(loop);
    km_command_loop_format_prompt(loop, prompt, sizeof(prompt));
    CHECK(strcmp(prompt, "Find file: note-\xe4\xb8\xad") == 0);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(!km_command_loop_prompt_active(loop));
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_FIND_FILE);
    CHECK(strcmp(km_command_loop_request_text(loop),
                 "note-\xe4\xb8\xad") == 0);
    km_command_loop_clear_request(loop);

    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'b', 0, &error) == KM_OK);
    km_command_loop_clear_request(loop);
    CHECK(km_command_loop_set_completions(loop, buffer_matches, 1,
                                          "*scratch*", &error) == KM_OK);
    km_command_loop_format_completions(loop, prompt, sizeof(prompt));
    CHECK(prompt[0] == '\0');
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_SWITCH_BUFFER);
    CHECK(km_command_loop_request_text(loop)[0] == '\0');
    km_command_loop_clear_request(loop);

    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'b', 0, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view, (const uint8_t *)"*s", 2,
                              &error) == KM_OK);
    km_command_loop_clear_request(loop);
    CHECK(km_command_loop_set_completions(loop, buffer_matches, 1,
                                          "*scratch*", &error) == KM_OK);
    km_command_loop_format_completions(loop, prompt, sizeof(prompt));
    CHECK(strcmp(prompt, " [cratch*] [Matched]") == 0);
    CHECK(dispatch_key(loop, view, KM_KEY_TAB, 0, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) ==
          KM_COMMAND_REQUEST_COMPLETE_BUFFER);
    CHECK(km_command_loop_prompt_active(loop));
    km_command_loop_clear_request(loop);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_SWITCH_BUFFER);
    CHECK(strcmp(km_command_loop_request_text(loop), "*scratch*") == 0);
    km_command_loop_clear_request(loop);

    CHECK(dispatch_text_block(loop, view, (const uint8_t *)"abc", 3,
                              &error) == KM_OK);
    CHECK(km_view_point(view).v == 3);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    km_command_loop_format_completions(loop, prompt, sizeof(prompt));
    CHECK(prompt[0] == '\0');
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_ERR_INVALID);
    CHECK(km_command_loop_prompt_active(loop));
    CHECK(dispatch_text_block(loop, view,
                              (const uint8_t *)"minibuffer", 10,
                              &error) == KM_OK);
    km_command_loop_format_completions(loop, prompt, sizeof(prompt));
    CHECK(strcmp(prompt, " [No matches]") == 0);
    CHECK(dispatch_key(loop, view, 'k', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view,
                              (const uint8_t *)"beginning-of-l", 14,
                              &error) == KM_OK);
    CHECK(dispatch_key(loop, view, KM_KEY_TAB, 0, &error) == KM_OK);
    km_command_loop_format_prompt(loop, prompt, sizeof(prompt));
    CHECK(strcmp(prompt, "M-x beginning-of-line") == 0);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_view_point(view).v == 0);
    CHECK(km_buffer_line_numbers_visible(buffer));
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(
              loop, view, (const uint8_t *)"display-line-numbers-mode", 25,
              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(!km_buffer_line_numbers_visible(buffer));
    CHECK(km_command_loop_last_command(loop) ==
          KM_COMMAND_DISPLAY_LINE_NUMBERS_MODE);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(
              loop, view,
              (const uint8_t *)"global-display-line-numbers-mode", 32,
              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) ==
          KM_COMMAND_REQUEST_GLOBAL_DISPLAY_LINE_NUMBERS_MODE);
    CHECK(km_command_loop_request_has_argument(loop));
    CHECK(km_command_loop_request_argument(loop) ==
          (km_config_global_display_line_numbers_mode() ? 0 : 1));
    CHECK(km_command_loop_last_command(loop) ==
          KM_COMMAND_GLOBAL_DISPLAY_LINE_NUMBERS_MODE);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(
              loop, view,
              (const uint8_t *)"global-display-line-numbers-mode", 32,
              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) ==
          KM_COMMAND_REQUEST_GLOBAL_DISPLAY_LINE_NUMBERS_MODE);
    CHECK(km_command_loop_request_argument(loop) == 1);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, '-', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(
              loop, view,
              (const uint8_t *)"global-display-line-numbers-mode", 32,
              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) ==
          KM_COMMAND_REQUEST_GLOBAL_DISPLAY_LINE_NUMBERS_MODE);
    CHECK(km_command_loop_request_argument(loop) == 0);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, '0', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(
              loop, view,
              (const uint8_t *)"global-display-line-numbers-mode", 32,
              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) ==
          KM_COMMAND_REQUEST_GLOBAL_DISPLAY_LINE_NUMBERS_MODE);
    CHECK(km_command_loop_request_argument(loop) == 0);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view, (const uint8_t *)"save-buffer", 11,
                              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_SAVE);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, '3', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view,
                              (const uint8_t *)"scroll-up-command", 17,
                              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_SCROLL_UP);
    CHECK(km_command_loop_request_has_argument(loop));
    CHECK(km_command_loop_request_argument(loop) == 3);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, '-', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view,
                              (const uint8_t *)"scroll-down-command", 19,
                              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_SCROLL_DOWN);
    CHECK(km_command_loop_request_page_opposite(loop));
    km_command_loop_clear_request(loop);

    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view, (const uint8_t *)"scroll-", 7,
                              &error) == KM_OK);
    km_command_loop_format_completions(loop, prompt, sizeof(prompt));
    CHECK(strcmp(prompt,
                 " {scroll-down-command | scroll-up-command}") == 0);
    CHECK(dispatch_key(loop, view, KM_KEY_RIGHT, 0, &error) == KM_OK);
    km_command_loop_format_completions(loop, prompt, sizeof(prompt));
    CHECK(strcmp(prompt,
                 " {scroll-up-command | scroll-down-command}") == 0);
    CHECK(dispatch_key(loop, view, 'r', KM_MOD_CTRL, &error) == KM_OK);
    km_command_loop_format_completions(loop, prompt, sizeof(prompt));
    CHECK(strcmp(prompt,
                 " {scroll-down-command | scroll-up-command}") == 0);
    CHECK(dispatch_key(loop, view, 's', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_SCROLL_UP);
    km_command_loop_clear_request(loop);

    CHECK(dispatch_key(loop, view, 'x', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view, (const uint8_t *)"no-such", 7,
                              &error) == KM_OK);
    km_command_loop_format_completions(loop, prompt, sizeof(prompt));
    CHECK(strcmp(prompt, " [No matches]") == 0);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_ERR_INVALID);
    CHECK(km_command_loop_prompt_active(loop));
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) == KM_OK);
    km_command_loop_clear_quit(loop);

    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'f', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 0, 1, &error) == KM_ERR_INVALID);
    CHECK(dispatch_paste(loop, view, invalid, sizeof(invalid), &error) ==
          KM_ERR_INVALID);
    CHECK(km_command_loop_prompt_active(loop));
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(!km_command_loop_prompt_active(loop));
    CHECK(km_command_loop_quit_requested(loop));
    km_command_loop_clear_quit(loop);

    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'f', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 'a', 3, &error) == KM_OK);
    CHECK(strcmp(km_command_loop_request_text(loop), "aaa") == 0);
    CHECK(km_command_loop_set_prompt_text(loop, "/tmp/foo/", &error) == KM_OK);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, 0x7f, 0, &error) == KM_OK);
    CHECK(strcmp(km_command_loop_request_text(loop), "/tmp/") == 0);
    km_command_loop_clear_request(loop);
    CHECK(km_command_loop_set_completions(loop, directory_match, 1,
                                          "/tmp/sub/", &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_COMPLETE_FILE);
    CHECK(strcmp(km_command_loop_request_text(loop), "/tmp/sub/") == 0);
    km_command_loop_clear_request(loop);
    CHECK(km_command_loop_set_prompt_text(loop, "/tmp/", &error) == KM_OK);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, 'j', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_FIND_FILE);
    CHECK(strcmp(km_command_loop_request_text(loop), "/tmp/") == 0);
    km_command_loop_clear_request(loop);

    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'k', 0, &error) == KM_OK);
    CHECK(km_command_loop_prompt_active(loop));
    CHECK(dispatch_text(loop, view, 'n', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_NONE);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'k', 0, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 'y', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_KILL_BUFFER);
    km_command_loop_clear_request(loop);

    CHECK(km_command_loop_confirm_exit(loop, &error) == KM_OK);
    km_command_loop_format_prompt(loop, prompt, sizeof(prompt));
    CHECK(strstr(prompt, "exit anyway?") != NULL);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_NONE);
    km_command_loop_clear_quit(loop);
    CHECK(km_command_loop_confirm_exit(loop, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 'y', 1, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) ==
          KM_COMMAND_REQUEST_EXIT_CONFIRMED);

    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_global_line_number_default(void)
{
    bool initial = km_config_global_display_line_numbers_mode();
    KmBuffer *buffer;
    KmError error;

    km_config_set_global_display_line_numbers_mode(false);
    buffer = make_base(NULL, 0);
    CHECK(!km_buffer_line_numbers_visible(buffer));
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    km_config_set_global_display_line_numbers_mode(true);
    buffer = make_base(NULL, 0);
    CHECK(km_buffer_line_numbers_visible(buffer));
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
    km_config_set_global_display_line_numbers_mode(initial);
}

static void test_case_space_and_line_commands(void)
{
    static const uint8_t words[] = "\xc3\xa9" "COLE foo";
    static const uint8_t capitalized[] = "\xc3\x89" "cole FOO";
    static const uint8_t lowered[] = "\xc3\xa9" "cole FOO";
    KmBuffer *buffer = make_base(words, sizeof(words) - 1);
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'c', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_view_point(view).v == 6);
    CHECK(km_view_set_point(view, (KmBytePos){7}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'u', KM_MOD_ALT, &error) == KM_OK);
    check_text(buffer, capitalized, sizeof(capitalized) - 1);
    CHECK(km_view_set_point(view, (KmBytePos){6}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '-', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'l', KM_MOD_ALT, &error) == KM_OK);
    check_text(buffer, lowered, sizeof(lowered) - 1);
    CHECK(km_view_point(view).v == 0);
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, capitalized, sizeof(capitalized) - 1);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base((const uint8_t *)"a \t b", 5);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){3}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, ' ', KM_MOD_ALT, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"a b", 3);
    CHECK(km_view_point(view).v == 2);
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"a \t b", 5);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base((const uint8_t *)"aa\n  bb", 7);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '^', KM_MOD_ALT | KM_MOD_SHIFT, &error) ==
          KM_OK);
    check_text(buffer, (const uint8_t *)"aa bb", 5);
    CHECK(km_view_point(view).v == 2);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base((const uint8_t *)"a\n\n\nb", 5);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){2}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'o', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"a\n\nb", 4);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base((const uint8_t *)"aa\nbb\ncc\ndd", 11);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){4}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 't', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"bb\ncc\naa\ndd", 11);
    CHECK(km_view_point(view).v == 9);
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"aa\nbb\ncc\ndd", 11);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base((const uint8_t *)"a\nb", 3);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){2}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 't', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"b\na\n", 4);
    CHECK(km_view_point(view).v == 4);
    CHECK(km_view_undo(view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){2}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 't', KM_MOD_CTRL, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"b\n\na\n", 5);
    CHECK(km_view_point(view).v == 5);
    CHECK(km_view_undo(view, &error) == KM_OK);
    CHECK(km_view_set_point(view, (KmBytePos){2}, &error) == KM_OK);
    {
        static const char digits[] = "9223372036854775807";
        size_t i;
        CHECK(dispatch_key(loop, view, (uint32_t)digits[0], KM_MOD_ALT,
                           &error) == KM_OK);
        for (i = 1; i < sizeof(digits) - 1; ++i) {
            CHECK(dispatch_key(loop, view, (uint32_t)digits[i], 0, &error) ==
                  KM_OK);
        }
    }
    CHECK(dispatch_key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 't', KM_MOD_CTRL, &error) == KM_ERR_OOM);
    check_text(buffer, (const uint8_t *)"a\nb", 3);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_quoted_insert_and_query_replace(void)
{
    static const uint8_t quoted[] = {
        0x01, 0xf0, 0x9f, 0x98, 0x80, 0xf0, 0x9f, 0x98, 0x80,
        0xf0, 0x9f, 0x98, 0x80,
    };
    KmBuffer *buffer = make_base(NULL, 0);
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'q', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'a', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '3', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'q', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 0x1f600, 1, &error) == KM_OK);
    check_text(buffer, quoted, sizeof(quoted));
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, quoted, 1);
    CHECK(dispatch_key(loop, view, 'q', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, KM_KEY_ESCAPE, 0, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'q', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, KM_KEY_TAB, 0, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'q', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, KM_KEY_DELETE, 0, &error) == KM_OK);
    {
        static const uint8_t named[] = {0x01, 0x1b, 0x09, 0x7f};
        check_text(buffer, named, sizeof(named));
    }
    km_buffer_set_read_only(buffer, true);
    CHECK(dispatch_key(loop, view, 'q', KM_MOD_CTRL, &error) ==
          KM_ERR_PERMISSION);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);

    buffer = make_base((const uint8_t *)"one two one two one", 19);
    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '%', KM_MOD_ALT | KM_MOD_SHIFT, &error) ==
          KM_OK);
    CHECK(dispatch_text_block(loop, view, (const uint8_t *)"one", 3,
                              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 'X', 1, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_view_point(view).v == 0);
    CHECK(dispatch_key(loop, view, 'n', 0, &error) == KM_OK);
    CHECK(km_view_point(view).v == 8);
    CHECK(dispatch_key(loop, view, 'y', 0, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"one two X two one", 17);
    CHECK(km_view_point(view).v == 14);
    CHECK(dispatch_key(loop, view, '!', 0, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"one two X two X", 15);
    CHECK(!km_command_loop_prompt_active(loop));
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"one two X two one", 17);
    CHECK(km_view_undo(view, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"one two one two one", 19);

    CHECK(km_view_set_point(view, (KmBytePos){0}, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '%', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view, (const uint8_t *)"one", 3,
                              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 'Z', 1, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'q', 0, &error) == KM_OK);
    check_text(buffer, (const uint8_t *)"one two one two one", 19);
    CHECK(dispatch_key(loop, view, '%', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_text_block(loop, view, (const uint8_t *)"one", 3,
                              &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, 'Z', 1, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_buffer_narrow(buffer, (KmBytePos){8}, (KmBytePos){19}, &error) ==
          KM_OK);
    CHECK(dispatch_key(loop, view, '!', 0, &error) == KM_ERR_CONFLICT);
    check_text(buffer, (const uint8_t *)"one two one two one", 19);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(!km_command_loop_prompt_active(loop));
    km_buffer_set_read_only(buffer, true);
    CHECK(dispatch_key(loop, view, '%', KM_MOD_ALT, &error) ==
          KM_ERR_PERMISSION);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

static void test_goto_char_and_viewport_requests(void)
{
    static const uint8_t text[] = {'A', 0xe4, 0xb8, 0xad, 'B'};
    KmBuffer *buffer = make_base(text, sizeof(text));
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;

    CHECK(km_view_create(buffer, &view, &error) == KM_OK);
    CHECK(km_command_loop_create(&loop, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, '3', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'c', 0, &error) == KM_OK);
    CHECK(km_view_point(view).v == 4);
    CHECK(dispatch_key(loop, view, 'g', KM_MOD_ALT, &error) == KM_OK);
    CHECK(dispatch_key(loop, view, 'c', 0, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '2', 1, &error) == KM_OK);
    CHECK(dispatch_text(loop, view, '\n', 1, &error) == KM_OK);
    CHECK(km_view_point(view).v == 1);

    CHECK(dispatch_key(loop, view, 'l', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) == KM_COMMAND_REQUEST_RECENTER);
    CHECK(!km_command_loop_request_has_argument(loop));
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, 'l', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_command_loop_request_has_argument(loop));
    CHECK(km_command_loop_request_argument(loop) == 0);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, 'l', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(km_command_loop_request_argument(loop) == -1);
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, 'l', KM_MOD_CTRL, &error) == KM_OK);
    CHECK(!km_command_loop_request_has_argument(loop));
    km_command_loop_clear_request(loop);
    CHECK(dispatch_key(loop, view, 'r', KM_MOD_ALT, &error) == KM_OK);
    CHECK(km_command_loop_request(loop) ==
          KM_COMMAND_REQUEST_MOVE_TO_WINDOW_LINE);
    CHECK(!km_command_loop_request_has_argument(loop));
    km_command_loop_clear_request(loop);
    km_command_loop_destroy(loop);
    CHECK(km_view_destroy(view, &error) == KM_OK);
    CHECK(km_buffer_destroy(buffer, &error) == KM_OK);
}

int main(void)
{
    test_ownership();
    test_views_and_narrowing();
    test_indirect_and_utf8_narrowing();
    test_saved_point();
    test_view_buffer_switching();
    test_read_only_and_modified();
    test_char_commands_utf8_boundaries();
    test_word_commands();
    test_buffer_edge_commands();
    test_paragraph_and_common_edit_commands();
    test_kill_ring_and_added_movement_commands();
    test_mark_ring_and_counted_kill_line();
    test_edit_commands_undo_redo_and_view_points();
    test_edit_scope_and_atomic_rejection();
    test_undo_redo_respect_narrowing();
    test_command_loop_events_and_trie();
    test_command_loop_prefix_arguments();
    test_counted_movement_boundaries();
    test_command_loop_atomic_edits();
    test_lines_mark_kill_and_yank();
    test_incremental_search();
    test_incremental_search_context();
    test_global_line_number_default();
    test_minibuffer_requests();
    test_case_space_and_line_commands();
    test_quoted_insert_and_query_replace();
    test_goto_char_and_viewport_requests();
    return 0;
}
