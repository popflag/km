#include "editor.h"

#include "file.h"
#include "unicode.h"

#include "utf8proc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct KmView {
    KmBuffer *buffer;
    KmAnchor *point;
    KmView *next;
    KmView **prev_next;
    size_t preferred_column;
    bool preferred_column_set;
};

struct KmBuffer {
    KmDocument *document;
    KmBuffer *base;
    KmAnchor *begv;
    KmAnchor *zv;
    KmAnchor *mark;
    KmAnchor *saved_point;
    KmView *views;
    size_t view_count;
    size_t indirect_count;
    KmStateId saved_state;
    KmFile *file;
    char *name;
    bool read_only;
    bool mark_active;
    bool line_numbers_visible;
};

typedef enum {
    KM_PREFIX_NONE,
    KM_PREFIX_UNIVERSAL,
    KM_PREFIX_NUMERIC
} KmPrefixKind;

typedef enum {
    KM_PROMPT_NONE,
    KM_PROMPT_FIND_FILE,
    KM_PROMPT_SWITCH_BUFFER,
    KM_PROMPT_COMMAND,
    KM_PROMPT_GOTO_LINE,
    KM_PROMPT_CONFIRM_KILL,
    KM_PROMPT_CONFIRM_EXIT
} KmPromptKind;

typedef struct {
    uint8_t *text;
    size_t len;
} KmKillEntry;

#define KM_KILL_RING_MAX 60

typedef struct {
    uint32_t codepoint;
    uint32_t modifiers;
    int command;
    size_t child;
    size_t sibling;
} KmKeyNode;

typedef struct {
    KmKeyNode *nodes;
    size_t count;
    size_t capacity;
} KmKeymap;

struct KmCommandLoop {
    size_t key_node;
    KmKeymapId keymap_id;
    KmKeymap keymaps[KM_KEYMAP_COUNT];
    KmPrefixKind prefix_kind;
    uint64_t prefix_magnitude;
    bool prefix_negative;
    bool prefix_has_digits;
    KmCommandId last_command;
    bool quit_requested;
    KmCommandRequest request;
    int64_t request_argument;
    bool request_has_argument;
    bool request_page_opposite;
    bool command_has_argument;
    bool command_page_opposite;
    KmPromptKind prompt_kind;
    int64_t prompt_argument;
    bool prompt_has_argument;
    bool prompt_page_opposite;
    uint8_t *prompt_text;
    size_t prompt_len;
    size_t prompt_cap;
    char **completions;
    size_t completion_count;
    size_t completion_index;
    char *completion_common;
    KmKillEntry kill_ring[KM_KILL_RING_MAX];
    size_t kill_count;
    KmBuffer *yank_buffer;
    KmRevision yank_revision;
    size_t yank_start;
    size_t yank_end;
    size_t yank_index;
    uint8_t *search_query;
    size_t search_len;
    size_t search_cap;
    KmBytePos search_origin;
    bool search_active;
    bool search_forward;
    bool search_failed;
    bool search_wrapped;
};

typedef KmStatus (*KmCommandCallback)(KmCommandLoop *loop, KmView *view,
                                      int64_t argument,
                                      KmError *error);

static KmStatus refresh_prompt_completions(KmCommandLoop *loop,
                                           KmError *error);
static KmStatus begin_prompt(KmCommandLoop *loop, KmPromptKind kind,
                             KmError *error);

typedef struct {
    int id;
    const char *name;
    KmCommandCallback callback;
    uint32_t contexts;
    uint32_t flags;
} KmCommandSpec;

enum {
    KM_INTERNAL_UNIVERSAL_ARGUMENT = 100,
    KM_INTERNAL_EXIT,
    KM_INTERNAL_SAVE,
    KM_INTERNAL_SAVE_ALL_EXIT,
    KM_INTERNAL_SEARCH_FORWARD,
    KM_INTERNAL_SEARCH_BACKWARD,
    KM_INTERNAL_FIND_FILE,
    KM_INTERNAL_SWITCH_BUFFER,
    KM_INTERNAL_KILL_BUFFER,
    KM_INTERNAL_EXTENDED_COMMAND,
    KM_INTERNAL_KEYBOARD_QUIT,
    KM_INTERNAL_MINIBUFFER_BACKSPACE,
    KM_INTERNAL_MINIBUFFER_CLEAR,
    KM_INTERNAL_MINIBUFFER_NEXT,
    KM_INTERNAL_MINIBUFFER_PREVIOUS,
    KM_INTERNAL_MINIBUFFER_COMPLETE,
    KM_INTERNAL_MINIBUFFER_ACCEPT_ORIGINAL,
    KM_INTERNAL_MINIBUFFER_ACCEPT,
    KM_INTERNAL_ISEARCH_REPEAT_FORWARD,
    KM_INTERNAL_ISEARCH_REPEAT_BACKWARD,
    KM_INTERNAL_ISEARCH_BACKSPACE,
    KM_INTERNAL_ISEARCH_ACCEPT,
    KM_INTERNAL_CONFIRM_YES,
    KM_INTERNAL_CONFIRM_NO,
    KM_INTERNAL_UNDEFINED
};

#define KM_NO_KEY_NODE SIZE_MAX

enum {
    KM_COMMAND_MX = 1u << 0,
    KM_COMMAND_RECORD_LAST = 1u << 1,
    KM_COMMAND_KEEP_PREFIX = 1u << 2
};

#define KM_CONTEXT(id) (1u << (unsigned)(id))

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

static void reset_preferred_columns(KmBuffer *buffer)
{
    KmView *view;

    for (view = buffer->views; view != NULL; view = view->next) {
        view->preferred_column_set = false;
    }
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
    status = km_document_apply(view->buffer->document, &splice, 1, meta, error);
    if (status == KM_OK) reset_preferred_columns(view->buffer);
    return status;
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
    status = km_anchor_set(view->point, point, error);
    if (status == KM_OK) view->preferred_column_set = false;
    return status;
}

static KmStatus copy_accessible_text(KmBuffer *buffer, uint8_t **out_text,
                                     size_t *out_len, size_t *out_point,
                                     KmView *view, KmError *error)
{
    KmBytePos start = km_anchor_get(buffer->begv);
    KmBytePos end = km_anchor_get(buffer->zv);
    KmBytePos point = km_anchor_get(view->point);
    uint8_t *text = NULL;
    size_t len = end.v - start.v;
    KmStatus status;

    if (len != 0) {
        text = (uint8_t *)malloc(len);
        if (text == NULL) return fail(error, KM_ERR_OOM, "line movement");
        status = km_document_copy(buffer->document, start, len, text, error);
        if (status != KM_OK) {
            free(text);
            return status;
        }
    }
    *out_text = text;
    *out_len = len;
    *out_point = point.v - start.v;
    return KM_OK;
}

static bool word_constituent(int32_t codepoint)
{
    utf8proc_category_t category = utf8proc_category(codepoint);

    /* ponytail: Replace this approximation when mode syntax tables exist. */
    return (category >= UTF8PROC_CATEGORY_LU &&
            category <= UTF8PROC_CATEGORY_ME) ||
           (category >= UTF8PROC_CATEGORY_ND &&
            category <= UTF8PROC_CATEGORY_NO) ||
           category == UTF8PROC_CATEGORY_SO;
}

static size_t previous_codepoint_start(const uint8_t *text, size_t position)
{
    --position;
    while (position != 0 && (text[position] & 0xc0u) == 0x80u) --position;
    return position;
}

static KmStatus scan_words(const uint8_t *text, size_t len, size_t position,
                           int64_t argument, size_t *out_position,
                           KmError *error)
{
    uint64_t remaining = command_magnitude(argument);
    bool backward = argument < 0;
    KmStatus status = KM_OK;

    while (remaining != 0) {
        bool found = false;

        if (backward) {
            while (position != 0) {
                int32_t codepoint;
                size_t consumed;
                size_t previous = previous_codepoint_start(text, position);
                status = km_unicode_decode(text, len, previous, &codepoint,
                                           &consumed, error);
                if (status != KM_OK) goto done;
                if (word_constituent(codepoint)) {
                    found = true;
                    break;
                }
                position = previous;
            }
            if (!found) {
                return fail(error, KM_ERR_INVALID, "scan words");
            }
            while (position != 0) {
                int32_t codepoint;
                size_t consumed;
                size_t previous = previous_codepoint_start(text, position);
                status = km_unicode_decode(text, len, previous, &codepoint,
                                           &consumed, error);
                if (status != KM_OK) goto done;
                if (!word_constituent(codepoint)) break;
                position = previous;
            }
        } else {
            while (position < len) {
                int32_t codepoint;
                size_t consumed;
                status = km_unicode_decode(text, len, position, &codepoint,
                                           &consumed, error);
                if (status != KM_OK) goto done;
                if (word_constituent(codepoint)) {
                    found = true;
                    break;
                }
                position += consumed;
            }
            if (!found) {
                return fail(error, KM_ERR_INVALID, "scan words");
            }
            while (position < len) {
                int32_t codepoint;
                size_t consumed;
                status = km_unicode_decode(text, len, position, &codepoint,
                                           &consumed, error);
                if (status != KM_OK) goto done;
                if (!word_constituent(codepoint)) break;
                position += consumed;
            }
        }
        --remaining;
    }
    *out_position = position;
    return KM_OK;

done:
    return status;
}

static KmStatus move_words(KmView *view, int64_t argument, KmError *error)
{
    uint8_t *text = NULL;
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    size_t len;
    size_t position;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    status = copy_accessible_text(view->buffer, &text, &len, &position, view,
                                  error);
    if (status != KM_OK) return status;
    status = scan_words(text, len, position, argument, &position, error);
    if (status == KM_OK) {
        status = km_anchor_set(view->point,
                               (KmBytePos){start.v + position}, error);
    }
    if (status == KM_OK) view->preferred_column_set = false;
    free(text);
    return status;
}

static bool sentence_terminal(int32_t codepoint)
{
    return codepoint == '.' || codepoint == '?' || codepoint == '!' ||
           codepoint == 0x3002 || codepoint == 0xff01 ||
           codepoint == 0xff1f;
}

static bool sentence_closer(int32_t codepoint)
{
    return codepoint == '"' || codepoint == '\'' || codepoint == ')' ||
           codepoint == ']' || codepoint == '}' || codepoint == 0x2019 ||
           codepoint == 0x201d || codepoint == 0x3009 ||
           codepoint == 0x300b || codepoint == 0x300d ||
           codepoint == 0x300f;
}

static size_t skip_sentence_space(const uint8_t *text, size_t len,
                                  size_t position)
{
    while (position < len &&
           (text[position] == ' ' || text[position] == '\t' ||
            text[position] == '\f' || text[position] == '\n')) {
        ++position;
    }
    return position;
}

static KmStatus scan_sentence_forward(const uint8_t *text, size_t len,
                                      size_t position, size_t *out_position,
                                      KmError *error)
{
    size_t offset = position;

    if (position == len) {
        return fail(error, KM_ERR_INVALID, "scan sentences");
    }
    while (offset < len) {
        int32_t codepoint;
        size_t consumed;
        size_t after;
        size_t spaces;
        KmStatus status = km_unicode_decode(text, len, offset, &codepoint,
                                            &consumed, error);
        if (status != KM_OK) return status;
        offset += consumed;
        if (!sentence_terminal(codepoint)) continue;
        after = offset;
        while (after < len) {
            int32_t closer;
            status = km_unicode_decode(text, len, after, &closer, &consumed,
                                       error);
            if (status != KM_OK) return status;
            if (!sentence_closer(closer)) break;
            after += consumed;
        }
        if (codepoint == 0x3002 || codepoint == 0xff01 ||
            codepoint == 0xff1f || after == len || text[after] == '\n') {
            *out_position = after;
            return KM_OK;
        }
        spaces = after;
        while (spaces < len &&
               (text[spaces] == ' ' || text[spaces] == '\t' ||
                text[spaces] == '\f')) {
            ++spaces;
        }
        if (spaces - after >= 2) {
            *out_position = after;
            return KM_OK;
        }
    }
    *out_position = len;
    return KM_OK;
}

static KmStatus scan_sentence_backward(const uint8_t *text, size_t len,
                                       size_t position,
                                       size_t *out_position,
                                       KmError *error)
{
    size_t sentence_start = 0;

    if (position == 0) {
        return fail(error, KM_ERR_INVALID, "scan sentences");
    }
    while (sentence_start < position) {
        size_t sentence_end;
        size_t next_start;
        KmStatus status = scan_sentence_forward(
            text, len, sentence_start, &sentence_end, error);
        if (status != KM_OK) return status;
        next_start = skip_sentence_space(text, len, sentence_end);
        if (position <= next_start || next_start <= sentence_start) {
            *out_position = sentence_start;
            return KM_OK;
        }
        sentence_start = next_start;
    }
    return fail(error, KM_ERR_INVALID, "scan sentences");
}

static KmStatus move_sentences(KmView *view, int64_t argument,
                               KmError *error)
{
    uint8_t *text = NULL;
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    size_t len;
    size_t position;
    uint64_t remaining = command_magnitude(argument);
    bool backward = argument < 0;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    status = copy_accessible_text(view->buffer, &text, &len, &position, view,
                                  error);
    if (status != KM_OK) return status;
    while (remaining-- != 0) {
        status = backward
                     ? scan_sentence_backward(text, len, position, &position,
                                              error)
                     : scan_sentence_forward(text, len, position, &position,
                                             error);
        if (status != KM_OK) goto done;
    }
    status = km_anchor_set(view->point, (KmBytePos){start.v + position}, error);
    if (status == KM_OK) view->preferred_column_set = false;

done:
    free(text);
    return status;
}

static KmStatus move_to_line(KmView *view, int64_t line, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    const KmDocument *document;
    uint8_t *text = NULL;
    size_t len;
    size_t position = 0;
    int64_t current = 1;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    if (line < 1) return fail(error, KM_ERR_INVALID, "line number");
    document = view->buffer->document;
    len = km_document_len(document);
    if (len != 0) {
        text = (uint8_t *)malloc(len);
        if (text == NULL) return fail(error, KM_ERR_OOM, "goto line");
        status = km_document_copy(document, (KmBytePos){0}, len, text, error);
        if (status != KM_OK) goto done;
    }
    while (current < line && position < len) {
        if (text[position++] == '\n') ++current;
    }
    if (current != line || position < start.v || position > end.v) {
        status = fail(error, KM_ERR_INVALID, "line outside accessible range");
        goto done;
    }
    status = km_anchor_set(view->point, (KmBytePos){position}, error);
    if (status == KM_OK) view->preferred_column_set = false;

done:
    free(text);
    return status;
}

static size_t line_start_at(const uint8_t *text, size_t point)
{
    while (point != 0 && text[point - 1] != '\n') --point;
    return point;
}

static size_t line_end_at(const uint8_t *text, size_t len, size_t point)
{
    while (point < len && text[point] != '\n') ++point;
    return point;
}

static bool blank_line_at(const uint8_t *text, size_t len, size_t start)
{
    size_t end = line_end_at(text, len, start);

    while (start < end) {
        uint8_t byte = text[start++];
        if (byte != ' ' && byte != '\t' && byte != '\f') return false;
    }
    return true;
}

static size_t next_line_at(const uint8_t *text, size_t len, size_t start)
{
    size_t end = line_end_at(text, len, start);
    return end < len ? end + 1 : len;
}

static size_t previous_line_at(const uint8_t *text, size_t start)
{
    return start == 0 ? 0 : line_start_at(text, start - 1);
}

static size_t forward_paragraph_once(const uint8_t *text, size_t len,
                                     size_t position)
{
    size_t line = line_start_at(text, position);

    while (line < len && blank_line_at(text, len, line)) {
        line = next_line_at(text, len, line);
    }
    while (line < len && !blank_line_at(text, len, line)) {
        line = next_line_at(text, len, line);
    }
    return line;
}

static size_t backward_paragraph_once(const uint8_t *text, size_t len,
                                      size_t position)
{
    size_t line = line_start_at(text, position);

    while (line != 0 && blank_line_at(text, len, line)) {
        line = previous_line_at(text, line);
    }
    while (line != 0) {
        size_t previous = previous_line_at(text, line);
        if (blank_line_at(text, len, previous)) return previous;
        line = previous;
    }
    return 0;
}

static KmStatus move_paragraphs(KmView *view, int64_t argument,
                                KmError *error)
{
    uint8_t *text = NULL;
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    size_t len;
    size_t position;
    uint64_t remaining = command_magnitude(argument);
    bool backward = argument < 0;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    status = copy_accessible_text(view->buffer, &text, &len, &position, view,
                                  error);
    if (status != KM_OK) return status;
    while (remaining-- != 0) {
        size_t next = backward
                          ? backward_paragraph_once(text, len, position)
                          : forward_paragraph_once(text, len, position);
        if (next == position) {
            status = fail(error, KM_ERR_INVALID,
                          backward ? "beginning of buffer" : "end of buffer");
            goto done;
        }
        position = next;
    }
    status = km_anchor_set(view->point, (KmBytePos){start.v + position}, error);
    if (status == KM_OK) view->preferred_column_set = false;

done:
    free(text);
    return status;
}

static KmStatus add_column(size_t *column, size_t amount, KmError *error)
{
    if (*column > SIZE_MAX - amount) {
        return fail(error, KM_ERR_INVALID, "line column");
    }
    *column += amount;
    return KM_OK;
}

static KmStatus grapheme_prefix_width(const uint8_t *text, size_t end,
                                      size_t point, size_t *out_width,
                                      KmError *error)
{
    size_t scan;
    size_t width = 0;

    for (scan = 0; scan < point;) {
        int32_t codepoint;
        size_t consumed;
        int codepoint_width;
        KmStatus status = km_unicode_decode(text, end, scan, &codepoint,
                                            &consumed, error);
        if (status != KM_OK) return status;
        codepoint_width = utf8proc_charwidth(codepoint);
        if (codepoint_width > 0 && (size_t)codepoint_width > width) {
            width = (size_t)codepoint_width;
        }
        scan += consumed;
    }
    *out_width = width;
    return KM_OK;
}

static KmStatus line_column_at(const uint8_t *text, size_t len,
                               size_t start, size_t point, size_t *out_column,
                               KmError *error)
{
    size_t offset = start;
    size_t column = 0;

    while (offset < point) {
        int32_t codepoint;
        size_t consumed;
        KmStatus status = km_unicode_decode(text, len, offset, &codepoint,
                                            &consumed, error);
        if (status != KM_OK) return status;
        if (codepoint == '\t') {
            status = add_column(&column, 8u - column % 8u, error);
            offset += consumed;
        } else if ((codepoint >= 0 && codepoint < 0x20) || codepoint == 0x7f) {
            status = add_column(&column, 2, error);
            offset += consumed;
        } else {
            KmGrapheme grapheme;
            status = km_unicode_next_grapheme(text, len, offset, &grapheme,
                                              error);
            if (status != KM_OK) return status;
            if (point < grapheme.end) {
                size_t prefix;
                status = grapheme_prefix_width(text + offset,
                                                grapheme.end - offset,
                                                point - offset, &prefix,
                                                error);
                if (status != KM_OK) return status;
                if (prefix > grapheme.width) prefix = grapheme.width;
                status = add_column(&column, prefix, error);
                offset = point;
            } else {
                status = add_column(&column, grapheme.width, error);
                offset = grapheme.end;
            }
        }
        if (status != KM_OK) return status;
    }
    *out_column = column;
    return KM_OK;
}

static KmStatus line_position_at_column(const uint8_t *text, size_t len,
                                        size_t start, size_t end,
                                        size_t target, size_t *out_position,
                                        KmError *error)
{
    size_t offset = start;
    size_t column = 0;

    while (offset < end) {
        int32_t codepoint;
        size_t consumed;
        size_t next_column;
        size_t next_offset;
        KmStatus status = km_unicode_decode(text, len, offset, &codepoint,
                                            &consumed, error);
        if (status != KM_OK) return status;
        next_offset = offset + consumed;
        if (codepoint == '\t') {
            if (column > SIZE_MAX - (8u - column % 8u)) {
                return fail(error, KM_ERR_INVALID, "line column");
            }
            next_column = column + (8u - column % 8u);
        } else if ((codepoint >= 0 && codepoint < 0x20) || codepoint == 0x7f) {
            if (column > SIZE_MAX - 2) {
                return fail(error, KM_ERR_INVALID, "line column");
            }
            next_column = column + 2;
        } else {
            KmGrapheme grapheme;
            status = km_unicode_next_grapheme(text, len, offset, &grapheme,
                                              error);
            if (status != KM_OK) return status;
            next_offset = grapheme.end;
            if (column > SIZE_MAX - grapheme.width) {
                return fail(error, KM_ERR_INVALID, "line column");
            }
            next_column = column + grapheme.width;
        }
        if (next_column > target) break;
        column = next_column;
        offset = next_offset;
    }
    *out_position = offset;
    return KM_OK;
}

static KmStatus move_vertical(KmView *view, int64_t argument, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    uint8_t *text = NULL;
    size_t len;
    size_t position;
    size_t preferred;
    uint64_t remaining = command_magnitude(argument);
    bool backward = argument < 0;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    status = copy_accessible_text(view->buffer, &text, &len, &position, view,
                                  error);
    if (status != KM_OK) return status;
    if (view->preferred_column_set) {
        preferred = view->preferred_column;
    } else {
        status = line_column_at(text, len, line_start_at(text, position),
                                position, &preferred, error);
        if (status != KM_OK) goto done;
    }
    while (remaining != 0) {
        size_t current_start = line_start_at(text, position);
        size_t target_start;
        size_t target_end;

        if (backward) {
            if (current_start == 0) {
                status = fail(error, KM_ERR_INVALID, "move lines");
                goto done;
            }
            target_end = current_start - 1;
            target_start = line_start_at(text, target_end);
        } else {
            size_t current_end = line_end_at(text, len, position);
            if (current_end == len) {
                status = fail(error, KM_ERR_INVALID, "move lines");
                goto done;
            }
            target_start = current_end + 1;
            target_end = line_end_at(text, len, target_start);
        }
        status = line_position_at_column(text, len, target_start, target_end,
                                         preferred, &position, error);
        if (status != KM_OK) goto done;
        --remaining;
    }
    status = km_anchor_set(view->point, (KmBytePos){start.v + position}, error);
    if (status == KM_OK) {
        view->preferred_column = preferred;
        view->preferred_column_set = true;
    }

done:
    free(text);
    return status;
}

static KmStatus move_line_edge(KmView *view, bool to_end, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    uint8_t *text = NULL;
    size_t len;
    size_t position;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    status = copy_accessible_text(view->buffer, &text, &len, &position, view,
                                  error);
    if (status == KM_OK) {
        position = to_end ? line_end_at(text, len, position)
                          : line_start_at(text, position);
        status = km_anchor_set(view->point,
                               (KmBytePos){start.v + position}, error);
        if (status == KM_OK) view->preferred_column_set = false;
    }
    free(text);
    return status;
}

static bool indentation_whitespace(int32_t codepoint)
{
    /* GNU Emacs default whitespace syntax is not Unicode White_Space. */
    return codepoint == ' ' || codepoint == '\t' || codepoint == '\f' ||
           codepoint == 0x00a0 || codepoint == 0x200b ||
           (codepoint >= 0x2000 && codepoint <= 0x200a) ||
           codepoint == 0x202f || codepoint == 0x205f ||
           codepoint == 0x3000;
}

static KmStatus move_back_to_indentation(KmView *view, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    uint8_t *text = NULL;
    size_t len;
    size_t position;
    size_t line_end;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    status = copy_accessible_text(view->buffer, &text, &len, &position, view,
                                  error);
    if (status != KM_OK) return status;
    position = line_start_at(text, position);
    line_end = line_end_at(text, len, position);
    while (position < line_end) {
        int32_t codepoint;
        size_t consumed;

        status = km_unicode_decode(text, len, position, &codepoint, &consumed,
                                   error);
        if (status != KM_OK || !indentation_whitespace(codepoint)) break;
        position += consumed;
    }
    if (status == KM_OK) {
        status = km_anchor_set(view->point,
                               (KmBytePos){start.v + position}, error);
        if (status == KM_OK) view->preferred_column_set = false;
    }
    free(text);
    return status;
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
    clamp_anchor(buffer->mark, start, end);
    for (view = buffer->views; view != NULL; view = view->next) {
        clamp_anchor(view->point, start, end);
    }
}

static void destroy_anchors(KmBuffer *buffer)
{
    km_anchor_destroy(buffer->saved_point);
    km_anchor_destroy(buffer->mark);
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
                              KM_ANCHOR_BEFORE, &buffer->mark, error);
    if (status != KM_OK) {
        km_anchor_destroy(buffer->zv);
        km_anchor_destroy(buffer->begv);
        buffer->zv = NULL;
        buffer->begv = NULL;
        return status;
    }
    status = km_anchor_create(buffer->document, (KmBytePos){0},
                              KM_ANCHOR_AFTER, &buffer->saved_point, error);
    if (status != KM_OK) {
        km_anchor_destroy(buffer->mark);
        km_anchor_destroy(buffer->zv);
        km_anchor_destroy(buffer->begv);
        buffer->mark = NULL;
        buffer->zv = NULL;
        buffer->begv = NULL;
    }
    return status;
}

static char *copy_name(const char *name)
{
    size_t len = strlen(name);
    char *copy;

    if (len == SIZE_MAX) return NULL;
    copy = (char *)malloc(len + 1);
    if (copy != NULL) memcpy(copy, name, len + 1);
    return copy;
}

static bool valid_buffer_name(const char *name)
{
    const uint8_t *bytes = (const uint8_t *)name;
    size_t len = strlen(name);
    size_t offset = 0;

    while (offset < len) {
        int32_t codepoint;
        if (len - offset > (size_t)PTRDIFF_MAX) return false;
        utf8proc_ssize_t consumed = utf8proc_iterate(bytes + offset,
                                                     (utf8proc_ssize_t)(len - offset),
                                                     &codepoint);
        if (consumed <= 0 || codepoint < 0x20 || codepoint == 0x7f) {
            return false;
        }
        offset += (size_t)consumed;
    }
    return len != 0;
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
    buffer->line_numbers_visible = true;
    buffer->name = copy_name("*scratch*");
    if (buffer->name == NULL) {
        km_document_destroy(document);
        free(buffer);
        return fail(error, KM_ERR_OOM, "buffer name");
    }
    status = create_buffer_anchors(buffer, error);
    if (status != KM_OK) {
        free(buffer->name);
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
    buffer->line_numbers_visible = base->line_numbers_visible;
    status = create_buffer_anchors(buffer, error);
    if (status != KM_OK) {
        free(buffer);
        return status;
    }
    ++base->indirect_count;
    *out_buffer = buffer;
    return KM_OK;
}

KmStatus km_buffer_create_file(KmFile *file, const uint8_t *text, size_t len,
                               KmBuffer **out_buffer, KmError *error)
{
    KmBuffer *buffer = NULL;
    char *name;
    KmStatus status;

    if (file == NULL) return fail(error, KM_ERR_INVALID, "file buffer create");
    name = copy_name(km_file_display_name(file));
    if (name == NULL) return fail(error, KM_ERR_OOM, "buffer name");
    status = km_buffer_create_base(text, len, &buffer, error);
    if (status != KM_OK) {
        free(name);
        return status;
    }
    free(buffer->name);
    buffer->name = name;
    buffer->file = file;
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
        km_file_destroy(buffer->file);
        km_document_destroy(buffer->document);
    }
    free(buffer->name);
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

bool km_buffer_is_visited(const KmBuffer *buffer)
{
    return buffer != NULL && buffer->base == NULL && buffer->file != NULL;
}

KmStatus km_buffer_save(KmBuffer *buffer, KmError *error)
{
    KmStateId state;
    KmStatus status;

    km_error_clear(error);
    if (buffer == NULL || buffer->base != NULL || buffer->file == NULL) {
        return fail(error, KM_ERR_UNSUPPORTED, "save buffer");
    }
    state = km_document_history_state(buffer->document);
    status = km_file_save(buffer->file, buffer->document, error);
    if (status == KM_OK && km_document_history_state(buffer->document) == state) {
        buffer->saved_state = state;
    }
    return status;
}

const char *km_buffer_name(const KmBuffer *buffer)
{
    if (buffer == NULL) return "";
    if (buffer->base != NULL) buffer = buffer->base;
    return buffer->name;
}

KmStatus km_buffer_set_name(KmBuffer *buffer, const char *name,
                            KmError *error)
{
    char *copy;

    km_error_clear(error);
    if (buffer == NULL || buffer->base != NULL || name == NULL ||
        !valid_buffer_name(name)) {
        return fail(error, KM_ERR_INVALID, "buffer name");
    }
    copy = copy_name(name);
    if (copy == NULL) return fail(error, KM_ERR_OOM, "buffer name");
    free(buffer->name);
    buffer->name = copy;
    return KM_OK;
}

bool km_buffer_visits_same_file(const KmBuffer *left, const KmBuffer *right)
{
    if (left == NULL || right == NULL) return false;
    if (left->base != NULL) left = left->base;
    if (right->base != NULL) right = right->base;
    return left->file != NULL && right->file != NULL &&
           km_file_same_target(left->file, right->file);
}

bool km_buffer_mark_active(const KmBuffer *buffer)
{
    return buffer != NULL && buffer->mark_active;
}

KmBytePos km_buffer_mark(const KmBuffer *buffer)
{
    return buffer == NULL ? (KmBytePos){0} : km_anchor_get(buffer->mark);
}

bool km_buffer_line_numbers_visible(const KmBuffer *buffer)
{
    return buffer != NULL && buffer->line_numbers_visible;
}

void km_buffer_set_line_numbers_visible(KmBuffer *buffer, bool visible)
{
    if (buffer != NULL) buffer->line_numbers_visible = visible;
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

KmBuffer *km_view_buffer(const KmView *view)
{
    return view == NULL ? NULL : view->buffer;
}

KmStatus km_view_set_buffer(KmView *view, KmBuffer *buffer, KmError *error)
{
    KmBuffer *old_buffer;
    KmAnchor *new_point = NULL;
    KmStatus status;

    km_error_clear(error);
    if (view == NULL || buffer == NULL) {
        return fail(error, KM_ERR_INVALID, "view buffer");
    }
    old_buffer = view->buffer;
    if (old_buffer == buffer) return KM_OK;
    if (old_buffer == NULL || buffer->view_count == SIZE_MAX) {
        return fail(error, KM_ERR_INVALID, "view buffer");
    }
    status = km_anchor_create(buffer->document,
                              km_anchor_get(buffer->saved_point),
                              KM_ANCHOR_AFTER, &new_point, error);
    if (status != KM_OK) return status;

    if (old_buffer->view_count == 1) {
        (void)km_anchor_set(old_buffer->saved_point,
                            km_anchor_get(view->point), NULL);
    }
    *view->prev_next = view->next;
    if (view->next != NULL) view->next->prev_next = view->prev_next;
    --old_buffer->view_count;
    km_anchor_destroy(view->point);

    view->buffer = buffer;
    view->point = new_point;
    view->next = buffer->views;
    view->prev_next = &buffer->views;
    if (view->next != NULL) view->next->prev_next = &view->next;
    buffer->views = view;
    ++buffer->view_count;
    view->preferred_column_set = false;
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

KmStatus km_view_forward_word(KmView *view, KmError *error)
{
    return move_words(view, 1, error);
}

KmStatus km_view_backward_word(KmView *view, KmError *error)
{
    return move_words(view, -1, error);
}

KmStatus km_view_delete_char(KmView *view, KmError *error)
{
    return delete_chars(view, 1, KM_COMMAND_DELETE_CHAR, error);
}

KmStatus km_view_delete_backward_char(KmView *view, KmError *error)
{
    return delete_chars(view, -1, KM_COMMAND_DELETE_BACKWARD_CHAR, error);
}

KmStatus km_view_beginning_of_line(KmView *view, KmError *error)
{
    return move_line_edge(view, false, error);
}

KmStatus km_view_end_of_line(KmView *view, KmError *error)
{
    return move_line_edge(view, true, error);
}

static KmStatus move_buffer_edge(KmView *view, bool to_end, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    status = km_anchor_set(view->point, to_end ? end : start, error);
    if (status == KM_OK) view->preferred_column_set = false;
    return status;
}

KmStatus km_view_beginning_of_buffer(KmView *view, KmError *error)
{
    return move_buffer_edge(view, false, error);
}

KmStatus km_view_end_of_buffer(KmView *view, KmError *error)
{
    return move_buffer_edge(view, true, error);
}

KmStatus km_view_forward_paragraph(KmView *view, KmError *error)
{
    return move_paragraphs(view, 1, error);
}

KmStatus km_view_backward_paragraph(KmView *view, KmError *error)
{
    return move_paragraphs(view, -1, error);
}

KmStatus km_view_forward_sentence(KmView *view, KmError *error)
{
    return move_sentences(view, 1, error);
}

KmStatus km_view_backward_sentence(KmView *view, KmError *error)
{
    return move_sentences(view, -1, error);
}

KmStatus km_view_next_line(KmView *view, KmError *error)
{
    return move_vertical(view, 1, error);
}

KmStatus km_view_previous_line(KmView *view, KmError *error)
{
    return move_vertical(view, -1, error);
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
    status = km_document_undo(view->buffer->document,
                              km_document_revision(view->buffer->document),
                              error);
    if (status == KM_OK) reset_preferred_columns(view->buffer);
    return status;
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
    status = km_document_redo(view->buffer->document,
                              km_document_revision(view->buffer->document),
                              error);
    if (status == KM_OK) reset_preferred_columns(view->buffer);
    return status;
}

static bool kill_command(KmCommandId command)
{
    return command == KM_COMMAND_KILL_REGION ||
           command == KM_COMMAND_KILL_LINE ||
           command == KM_COMMAND_KILL_WORD ||
           command == KM_COMMAND_BACKWARD_KILL_WORD;
}

static KmStatus prepare_kill(KmCommandLoop *loop, const uint8_t *text,
                             size_t len, bool prepend, bool force_new,
                             KmKillEntry *out_entry, bool *out_replace,
                             KmError *error)
{
    bool replace = !force_new && loop->kill_count != 0 &&
                   kill_command(loop->last_command);
    size_t old_len = replace ? loop->kill_ring[0].len : 0;
    uint8_t *copy;

    if (len > SIZE_MAX - old_len) {
        return fail(error, KM_ERR_OOM, "kill ring");
    }
    copy = (uint8_t *)malloc(old_len + len);
    if (copy == NULL) return fail(error, KM_ERR_OOM, "kill ring");
    if (replace && prepend) {
        memcpy(copy, text, len);
        memcpy(copy + len, loop->kill_ring[0].text, old_len);
    } else {
        if (replace) memcpy(copy, loop->kill_ring[0].text, old_len);
        memcpy(copy + old_len, text, len);
    }
    *out_entry = (KmKillEntry){copy, old_len + len};
    *out_replace = replace;
    return KM_OK;
}

static void commit_kill(KmCommandLoop *loop, KmKillEntry entry, bool replace)
{
    if (replace) {
        free(loop->kill_ring[0].text);
    } else {
        if (loop->kill_count == KM_KILL_RING_MAX) {
            free(loop->kill_ring[KM_KILL_RING_MAX - 1].text);
        } else {
            ++loop->kill_count;
        }
        memmove(&loop->kill_ring[1], &loop->kill_ring[0],
                (loop->kill_count - 1) * sizeof(loop->kill_ring[0]));
    }
    loop->kill_ring[0] = entry;
}

static KmStatus kill_words(KmCommandLoop *loop, KmView *view,
                           int64_t argument, uint64_t command_id,
                           KmError *error)
{
    uint8_t *text = NULL;
    KmKillEntry entry = {0};
    bool replace = false;
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    size_t len;
    size_t position;
    size_t target;
    size_t kill_start;
    size_t kill_end;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    if (argument == 0) return KM_OK;
    status = copy_accessible_text(view->buffer, &text, &len, &position, view,
                                  error);
    if (status != KM_OK) return status;
    status = scan_words(text, len, position, argument, &target, error);
    if (status != KM_OK) goto done;
    kill_start = target < position ? target : position;
    kill_end = target < position ? position : target;
    status = prepare_kill(loop, text + kill_start, kill_end - kill_start,
                          target < position, false, &entry, &replace, error);
    if (status != KM_OK) goto done;
    status = apply_view_splice(
        view, (KmBytePos){start.v + kill_start},
        (KmBytePos){start.v + kill_end}, NULL, 0, command_id, error);
    if (status == KM_OK) {
        commit_kill(loop, entry, replace);
        entry.text = NULL;
    }

done:
    free(entry.text);
    free(text);
    return status;
}

static KmStatus open_lines(KmView *view, int64_t argument, KmError *error)
{
    uint8_t *newlines = NULL;
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmSplice splice;
    KmTxnMeta meta;
    size_t count;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    if (argument < 0 || (uint64_t)argument > (uint64_t)PTRDIFF_MAX) {
        return fail(error, KM_ERR_INVALID, "open line argument");
    }
    count = (size_t)argument;
    if (count == 0) return KM_OK;
    newlines = (uint8_t *)malloc(count);
    if (newlines == NULL) return fail(error, KM_ERR_OOM, "open line");
    memset(newlines, '\n', count);
    splice = (KmSplice){point, point, newlines, count, 0};
    meta = (KmTxnMeta){km_document_revision(view->buffer->document),
                       KM_COMMAND_OPEN_LINE};
    status = km_document_apply_and_set_anchor(
        view->buffer->document, &splice, meta, view->point, point, error);
    if (status == KM_OK) reset_preferred_columns(view->buffer);
    free(newlines);
    return status;
}

static KmStatus scan_codepoints(const uint8_t *text, size_t len,
                                size_t position, uint64_t count,
                                bool backward, size_t *out_position,
                                KmError *error)
{
    while (count-- != 0) {
        if (backward) {
            if (position == 0) {
                return fail(error, KM_ERR_INVALID, "transpose characters");
            }
            position = previous_codepoint_start(text, position);
        } else {
            if (position == len) {
                return fail(error, KM_ERR_INVALID, "transpose characters");
            }
            ++position;
            while (position < len && (text[position] & 0xc0u) == 0x80u) {
                ++position;
            }
        }
    }
    *out_position = position;
    return KM_OK;
}

static KmStatus transpose_chars(KmView *view, int64_t argument,
                                KmError *error)
{
    uint8_t *text = NULL;
    uint8_t *replacement = NULL;
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    size_t len;
    size_t position;
    size_t first_start;
    size_t first_end;
    size_t second_start;
    size_t second_end;
    size_t range_start;
    size_t range_end;
    size_t final_position;
    size_t first_len;
    size_t second_len;
    KmSplice splice;
    KmTxnMeta meta;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    if (argument == 0) {
        return fail(error, KM_ERR_INVALID, "transpose mark unavailable");
    }
    status = copy_accessible_text(view->buffer, &text, &len, &position, view,
                                  error);
    if (status != KM_OK) return status;
    if (position == 0) {
        status = fail(error, KM_ERR_INVALID, "transpose characters");
        goto done;
    }
    if (position == len || text[position] == '\n') {
        position = previous_codepoint_start(text, position);
    }
    if (position == 0) {
        status = fail(error, KM_ERR_INVALID, "transpose characters");
        goto done;
    }
    first_start = previous_codepoint_start(text, position);
    first_end = position;
    if (argument > 0) {
        second_start = position;
        status = scan_codepoints(text, len, second_start,
                                 (uint64_t)argument, false, &second_end,
                                 error);
        if (status != KM_OK) goto done;
        range_start = first_start;
        range_end = second_end;
        final_position = range_end;
    } else {
        second_end = first_start;
        status = scan_codepoints(text, len, second_end,
                                 command_magnitude(argument), true,
                                 &second_start, error);
        if (status != KM_OK) goto done;
        range_start = second_start;
        range_end = first_end;
        final_position = range_start + first_end - first_start;
    }
    first_len = first_end - first_start;
    second_len = second_end - second_start;
    replacement = (uint8_t *)malloc(first_len + second_len);
    if (replacement == NULL) {
        status = fail(error, KM_ERR_OOM, "transpose characters");
        goto done;
    }
    if (argument > 0) {
        memcpy(replacement, text + second_start, second_len);
        memcpy(replacement + second_len, text + first_start, first_len);
    } else {
        memcpy(replacement, text + first_start, first_len);
        memcpy(replacement + first_len, text + second_start, second_len);
    }
    splice = (KmSplice){
        {start.v + range_start}, {start.v + range_end}, replacement,
        first_len + second_len, 0,
    };
    meta = (KmTxnMeta){km_document_revision(view->buffer->document),
                       KM_COMMAND_TRANSPOSE_CHARS};
    status = km_document_apply_and_set_anchor(
        view->buffer->document, &splice, meta, view->point,
        (KmBytePos){start.v + final_position}, error);
    if (status == KM_OK) {
        reset_preferred_columns(view->buffer);
    }

done:
    free(replacement);
    free(text);
    return status;
}

static KmStatus transpose_words(KmView *view, int64_t argument,
                                KmError *error)
{
    uint8_t *text = NULL;
    uint8_t *replacement = NULL;
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    size_t len;
    size_t position;
    size_t first_start;
    size_t first_end;
    size_t second_start;
    size_t second_end;
    size_t middle_len;
    size_t first_len;
    size_t second_len;
    size_t final_position;
    KmSplice splice;
    KmTxnMeta meta;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    if (argument == 0) {
        return fail(error, KM_ERR_INVALID, "transpose mark unavailable");
    }
    status = copy_accessible_text(view->buffer, &text, &len, &position, view,
                                  error);
    if (status != KM_OK) return status;
    status = scan_words(text, len, position, -1, &first_start, error);
    if (status != KM_OK && argument > 0) {
        status = scan_words(text, len, position, 1, &first_end, error);
        if (status == KM_OK) {
            status = scan_words(text, len, first_end, -1, &first_start,
                                error);
        }
    }
    if (status != KM_OK) goto done;
    status = scan_words(text, len, first_start, 1, &first_end, error);
    if (status != KM_OK) goto done;
    if (argument > 0) {
        status = scan_words(text, len, first_end, 1, &second_end, error);
        if (status != KM_OK) goto done;
        status = scan_words(text, len, second_end, -1, &second_start, error);
        if (status != KM_OK) goto done;
        if (argument > 1) {
            status = scan_words(text, len, first_end, argument, &second_end,
                                error);
            if (status != KM_OK) goto done;
        }
        middle_len = second_start - first_end;
        first_len = first_end - first_start;
        second_len = second_end - second_start;
        replacement = (uint8_t *)malloc(first_len + middle_len + second_len);
        if (replacement == NULL) {
            status = fail(error, KM_ERR_OOM, "transpose words");
            goto done;
        }
        memcpy(replacement, text + second_start, second_len);
        memcpy(replacement + second_len, text + first_end, middle_len);
        memcpy(replacement + second_len + middle_len,
               text + first_start, first_len);
        final_position = second_end;
    } else {
        status = scan_words(text, len, first_start, argument, &second_start,
                            error);
        if (status != KM_OK) goto done;
        status = scan_words(text, len, second_start, -argument, &second_end,
                            error);
        if (status != KM_OK) goto done;
        middle_len = first_start - second_end;
        first_len = first_end - first_start;
        second_len = second_end - second_start;
        replacement = (uint8_t *)malloc(first_len + middle_len + second_len);
        if (replacement == NULL) {
            status = fail(error, KM_ERR_OOM, "transpose words");
            goto done;
        }
        memcpy(replacement, text + first_start, first_len);
        memcpy(replacement + first_len, text + second_end, middle_len);
        memcpy(replacement + first_len + middle_len,
               text + second_start, second_len);
        final_position = second_start + first_len;
        first_start = second_start;
        second_end = first_end;
    }
    splice = (KmSplice){
        {start.v + first_start}, {start.v + second_end}, replacement,
        first_len + middle_len + second_len, 0,
    };
    meta = (KmTxnMeta){km_document_revision(view->buffer->document),
                       KM_COMMAND_TRANSPOSE_WORDS};
    status = km_document_apply_and_set_anchor(
        view->buffer->document, &splice, meta, view->point,
        (KmBytePos){start.v + final_position}, error);
    if (status == KM_OK) reset_preferred_columns(view->buffer);

done:
    free(replacement);
    free(text);
    return status;
}

static KmStatus delete_horizontal_space(KmView *view, bool backward_only,
                                        KmError *error)
{
    uint8_t *text = NULL;
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    size_t len;
    size_t position;
    size_t delete_start;
    size_t delete_end;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    status = copy_accessible_text(view->buffer, &text, &len, &position, view,
                                  error);
    if (status != KM_OK) return status;
    delete_start = position;
    delete_end = position;
    while (delete_start != 0 &&
           (text[delete_start - 1] == ' ' || text[delete_start - 1] == '\t')) {
        --delete_start;
    }
    if (!backward_only) {
        while (delete_end < len &&
               (text[delete_end] == ' ' || text[delete_end] == '\t')) {
            ++delete_end;
        }
    }
    if (delete_start != delete_end) {
        status = apply_view_splice(
            view, (KmBytePos){start.v + delete_start},
            (KmBytePos){start.v + delete_end}, NULL, 0,
            KM_COMMAND_DELETE_HORIZONTAL_SPACE, error);
    }
    free(text);
    return status;
}

static KmStatus command_forward(KmCommandLoop *loop, KmView *view,
                                int64_t argument,
                                KmError *error)
{
    (void)loop;
    return move_chars(view, argument, error);
}

static KmStatus command_backward(KmCommandLoop *loop, KmView *view,
                                 int64_t argument,
                                 KmError *error)
{
    (void)loop;
    return move_chars(view, -argument, error);
}

static KmStatus command_forward_word(KmCommandLoop *loop, KmView *view,
                                     int64_t argument, KmError *error)
{
    (void)loop;
    return move_words(view, argument, error);
}

static KmStatus command_backward_word(KmCommandLoop *loop, KmView *view,
                                      int64_t argument, KmError *error)
{
    (void)loop;
    return move_words(view, -argument, error);
}

static KmStatus command_forward_paragraph(KmCommandLoop *loop, KmView *view,
                                          int64_t argument, KmError *error)
{
    (void)loop;
    return move_paragraphs(view, argument, error);
}

static KmStatus command_backward_paragraph(KmCommandLoop *loop, KmView *view,
                                           int64_t argument, KmError *error)
{
    (void)loop;
    return move_paragraphs(view, -argument, error);
}

static KmStatus command_forward_sentence(KmCommandLoop *loop, KmView *view,
                                         int64_t argument, KmError *error)
{
    (void)loop;
    return move_sentences(view, argument, error);
}

static KmStatus command_backward_sentence(KmCommandLoop *loop, KmView *view,
                                          int64_t argument, KmError *error)
{
    (void)loop;
    return move_sentences(view, -argument, error);
}

static KmStatus command_delete(KmCommandLoop *loop, KmView *view,
                               int64_t argument,
                               KmError *error)
{
    (void)loop;
    return delete_chars(view, argument, KM_COMMAND_DELETE_CHAR, error);
}

static KmStatus command_delete_backward(KmCommandLoop *loop, KmView *view,
                                        int64_t argument,
                                        KmError *error)
{
    (void)loop;
    return delete_chars(view, -argument,
                        KM_COMMAND_DELETE_BACKWARD_CHAR, error);
}

static KmStatus command_kill_word(KmCommandLoop *loop, KmView *view,
                                  int64_t argument, KmError *error)
{
    return kill_words(loop, view, argument, KM_COMMAND_KILL_WORD, error);
}

static KmStatus command_backward_kill_word(KmCommandLoop *loop, KmView *view,
                                           int64_t argument, KmError *error)
{
    return kill_words(loop, view, -argument, KM_COMMAND_BACKWARD_KILL_WORD,
                      error);
}

static KmStatus command_open_line(KmCommandLoop *loop, KmView *view,
                                  int64_t argument, KmError *error)
{
    (void)loop;
    return open_lines(view, argument, error);
}

static KmStatus command_transpose_chars(KmCommandLoop *loop, KmView *view,
                                        int64_t argument, KmError *error)
{
    (void)loop;
    return transpose_chars(view, argument, error);
}

static KmStatus command_transpose_words(KmCommandLoop *loop, KmView *view,
                                        int64_t argument, KmError *error)
{
    (void)loop;
    return transpose_words(view, argument, error);
}

static KmStatus command_delete_horizontal_space(
    KmCommandLoop *loop, KmView *view, int64_t argument, KmError *error)
{
    (void)argument;
    return delete_horizontal_space(view, loop->command_has_argument, error);
}

static KmStatus command_line_edge(KmView *view, int64_t argument, bool to_end,
                                  KmError *error)
{
    if (argument != 1) return fail(error, KM_ERR_INVALID, "line argument");
    return move_line_edge(view, to_end, error);
}

static KmStatus command_beginning_of_line(KmCommandLoop *loop, KmView *view,
                                          int64_t argument, KmError *error)
{
    (void)loop;
    return command_line_edge(view, argument, false, error);
}

static KmStatus command_back_to_indentation(KmCommandLoop *loop, KmView *view,
                                            int64_t argument, KmError *error)
{
    (void)loop;
    (void)argument;
    return move_back_to_indentation(view, error);
}

static KmStatus command_end_of_line(KmCommandLoop *loop, KmView *view,
                                    int64_t argument, KmError *error)
{
    (void)loop;
    return command_line_edge(view, argument, true, error);
}

static KmStatus command_buffer_edge(KmCommandLoop *loop, KmView *view,
                                    bool to_end, KmError *error)
{
    /* ponytail: Numeric tenths need raw prefix and inactive-mark support. */
    if (loop->command_has_argument) {
        return fail(error, KM_ERR_INVALID, "buffer edge argument");
    }
    return move_buffer_edge(view, to_end, error);
}

static KmStatus command_beginning_of_buffer(KmCommandLoop *loop, KmView *view,
                                            int64_t argument, KmError *error)
{
    (void)argument;
    return command_buffer_edge(loop, view, false, error);
}

static KmStatus command_end_of_buffer(KmCommandLoop *loop, KmView *view,
                                      int64_t argument, KmError *error)
{
    (void)argument;
    return command_buffer_edge(loop, view, true, error);
}

static KmStatus command_next_line(KmCommandLoop *loop, KmView *view,
                                  int64_t argument, KmError *error)
{
    (void)loop;
    return move_vertical(view, argument, error);
}

static KmStatus command_previous_line(KmCommandLoop *loop, KmView *view,
                                      int64_t argument, KmError *error)
{
    (void)loop;
    return move_vertical(view, -argument, error);
}

static KmStatus command_set_mark(KmCommandLoop *loop, KmView *view,
                                 int64_t argument, KmError *error)
{
    KmStatus status;

    (void)loop;
    if (argument != 1) return fail(error, KM_ERR_INVALID, "mark argument");
    status = km_anchor_set(view->buffer->mark, km_anchor_get(view->point), error);
    if (status == KM_OK) view->buffer->mark_active = true;
    return status;
}

static KmStatus command_mark_whole_buffer(KmCommandLoop *loop, KmView *view,
                                          int64_t argument, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmStatus status;

    (void)loop;
    if (argument != 1) return fail(error, KM_ERR_INVALID, "mark argument");
    status = validate_view(view, false, &point, &start, &end, error);
    if (status != KM_OK) return status;
    status = km_anchor_set(view->buffer->mark, end, error);
    if (status != KM_OK) return status;
    status = km_anchor_set(view->point, start, error);
    if (status == KM_OK) {
        view->buffer->mark_active = true;
        view->preferred_column_set = false;
    }
    return status;
}

static KmStatus command_exchange_mark(KmCommandLoop *loop, KmView *view,
                                      int64_t argument, KmError *error)
{
    KmBytePos point;
    KmBytePos mark;
    KmStatus status;

    (void)loop;
    if (argument != 1 || !view->buffer->mark_active) {
        return fail(error, KM_ERR_INVALID, "exchange point and mark");
    }
    point = km_anchor_get(view->point);
    mark = km_anchor_get(view->buffer->mark);
    status = km_anchor_set(view->point, mark, error);
    if (status != KM_OK) return status;
    status = km_anchor_set(view->buffer->mark, point, error);
    if (status != KM_OK) {
        (void)km_anchor_set(view->point, point, NULL);
        return status;
    }
    view->preferred_column_set = false;
    return KM_OK;
}

static KmStatus region_bounds(KmView *view, KmBytePos *out_start,
                              KmBytePos *out_end, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmBytePos mark;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    if (!view->buffer->mark_active) {
        return fail(error, KM_ERR_INVALID, "region inactive");
    }
    mark = km_anchor_get(view->buffer->mark);
    if (mark.v < start.v || mark.v > end.v || mark.v == point.v) {
        return fail(error, KM_ERR_INVALID, "region inactive");
    }
    *out_start = mark.v < point.v ? mark : point;
    *out_end = mark.v < point.v ? point : mark;
    return KM_OK;
}

static KmStatus command_kill_region(KmCommandLoop *loop, KmView *view,
                                    int64_t argument, KmError *error)
{
    KmBytePos start;
    KmBytePos end;
    KmBytePos point;
    KmKillEntry entry = {0};
    bool replace = false;
    size_t len;
    KmStatus status;

    if (argument != 1) return fail(error, KM_ERR_INVALID, "kill argument");
    status = region_bounds(view, &start, &end, error);
    if (status != KM_OK) return status;
    if (view->buffer->read_only) {
        return fail(error, KM_ERR_PERMISSION, "kill region");
    }
    point = km_anchor_get(view->point);
    len = end.v - start.v;
    entry.text = (uint8_t *)malloc(len);
    if (entry.text == NULL) return fail(error, KM_ERR_OOM, "kill region");
    status = km_document_copy(view->buffer->document, start, len, entry.text,
                              error);
    if (status == KM_OK) {
        uint8_t *copy = entry.text;
        entry.text = NULL;
        status = prepare_kill(loop, copy, len, point.v == end.v, false,
                              &entry, &replace, error);
        free(copy);
    }
    if (status == KM_OK) {
        status = apply_view_splice(view, start, end, NULL, 0,
                                   KM_COMMAND_KILL_REGION, error);
    }
    if (status != KM_OK) {
        free(entry.text);
        return status;
    }
    commit_kill(loop, entry, replace);
    view->buffer->mark_active = false;
    return KM_OK;
}

static KmStatus command_copy_region(KmCommandLoop *loop, KmView *view,
                                    int64_t argument, KmError *error)
{
    KmBytePos start;
    KmBytePos end;
    KmKillEntry entry = {0};
    bool replace = false;
    size_t len;
    KmStatus status;

    if (argument != 1) return fail(error, KM_ERR_INVALID, "copy argument");
    status = region_bounds(view, &start, &end, error);
    if (status != KM_OK) return status;
    len = end.v - start.v;
    entry.text = (uint8_t *)malloc(len);
    if (entry.text == NULL) return fail(error, KM_ERR_OOM, "copy region");
    status = km_document_copy(view->buffer->document, start, len, entry.text,
                              error);
    if (status != KM_OK) {
        free(entry.text);
        return status;
    }
    {
        uint8_t *copy = entry.text;
        entry.text = NULL;
        status = prepare_kill(loop, copy, len, false, true, &entry, &replace,
                              error);
        free(copy);
    }
    if (status != KM_OK) return status;
    commit_kill(loop, entry, replace);
    return KM_OK;
}

static KmStatus command_kill_line(KmCommandLoop *loop, KmView *view,
                                  int64_t argument, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    uint8_t *text = NULL;
    size_t len;
    size_t position;
    size_t line_end;
    KmKillEntry entry = {0};
    bool replace = false;
    size_t kill_len;
    KmStatus status;

    if (argument != 1) return fail(error, KM_ERR_INVALID, "kill line argument");
    status = validate_view(view, true, &point, &start, &end, error);
    if (status != KM_OK) return status;
    status = copy_accessible_text(view->buffer, &text, &len, &position, view,
                                  error);
    if (status != KM_OK) return status;
    line_end = line_end_at(text, len, position);
    if (line_end == position && line_end < len) ++line_end;
    if (line_end == position) {
        free(text);
        return fail(error, KM_ERR_INVALID, "kill line");
    }
    kill_len = line_end - position;
    status = prepare_kill(loop, text + position, kill_len, false, false,
                          &entry, &replace, error);
    free(text);
    if (status != KM_OK) return status;
    status = apply_view_splice(view, point,
                               (KmBytePos){point.v + kill_len}, NULL, 0,
                               KM_COMMAND_KILL_LINE, error);
    if (status != KM_OK) {
        free(entry.text);
        return status;
    }
    commit_kill(loop, entry, replace);
    return KM_OK;
}

static KmStatus command_yank(KmCommandLoop *loop, KmView *view,
                             int64_t argument, KmError *error)
{
    KmBytePos start;
    KmStatus status;

    if (loop->kill_count == 0) {
        return fail(error, KM_ERR_INVALID, "kill ring empty");
    }
    start = km_view_point(view);
    status = insert_utf8_repeated(view, loop->kill_ring[0].text,
                                  loop->kill_ring[0].len, argument,
                                  KM_COMMAND_YANK, error);
    if (status == KM_OK) {
        loop->yank_buffer = view->buffer;
        loop->yank_revision =
            km_document_revision(view->buffer->document);
        loop->yank_start = start.v;
        loop->yank_end = km_view_point(view).v;
        loop->yank_index = 0;
    }
    return status;
}

static KmStatus command_yank_pop(KmCommandLoop *loop, KmView *view,
                                 int64_t argument, KmError *error)
{
    KmBytePos point;
    KmBytePos start;
    KmBytePos end;
    KmSplice splice;
    KmTxnMeta meta;
    size_t step;
    size_t index;
    size_t target;
    KmStatus status = validate_view(view, true, &point, &start, &end, error);

    if (status != KM_OK) return status;
    if (loop->kill_count == 0 || loop->yank_buffer != view->buffer ||
        loop->yank_revision != km_document_revision(view->buffer->document) ||
        loop->yank_start < start.v || loop->yank_end > end.v ||
        loop->yank_start > loop->yank_end || point.v != loop->yank_end) {
        return fail(error, KM_ERR_INVALID, "previous command was not a yank");
    }
    step = (size_t)(command_magnitude(argument) % loop->kill_count);
    if (argument < 0) {
        index = (loop->yank_index + loop->kill_count - step) %
                loop->kill_count;
    } else {
        index = (loop->yank_index + step) % loop->kill_count;
    }
    if (loop->yank_start > SIZE_MAX - loop->kill_ring[index].len) {
        return fail(error, KM_ERR_INVALID, "yank pop");
    }
    target = loop->yank_start + loop->kill_ring[index].len;
    splice = (KmSplice){
        {loop->yank_start}, {loop->yank_end}, loop->kill_ring[index].text,
        loop->kill_ring[index].len, 0,
    };
    meta = (KmTxnMeta){km_document_revision(view->buffer->document),
                       KM_COMMAND_YANK_POP};
    status = km_document_apply_and_set_anchor(
        view->buffer->document, &splice, meta, view->point,
        (KmBytePos){target}, error);
    if (status == KM_OK) {
        loop->yank_revision =
            km_document_revision(view->buffer->document);
        loop->yank_end = target;
        loop->yank_index = index;
        reset_preferred_columns(view->buffer);
    }
    return status;
}

static KmStatus command_undo(KmCommandLoop *loop, KmView *view,
                             int64_t argument, KmError *error)
{
    (void)loop;
    if (argument != 1) {
        return fail(error, KM_ERR_INVALID, "undo argument");
    }
    return km_view_undo(view, error);
}

static KmStatus command_redo(KmCommandLoop *loop, KmView *view,
                             int64_t argument, KmError *error)
{
    (void)loop;
    if (argument != 1) {
        return fail(error, KM_ERR_INVALID, "redo argument");
    }
    return km_view_redo(view, error);
}

static KmStatus request_scroll(KmCommandLoop *loop, KmView *view,
                               int64_t argument, KmCommandRequest request,
                               KmError *error)
{
    (void)view;
    (void)error;
    loop->request = request;
    loop->request_argument = argument;
    loop->request_has_argument = loop->command_has_argument;
    loop->request_page_opposite = loop->command_page_opposite;
    return KM_OK;
}

static KmStatus command_scroll_up(KmCommandLoop *loop, KmView *view,
                                  int64_t argument, KmError *error)
{
    return request_scroll(loop, view, argument, KM_COMMAND_REQUEST_SCROLL_UP,
                          error);
}

static KmStatus command_scroll_down(KmCommandLoop *loop, KmView *view,
                                    int64_t argument, KmError *error)
{
    return request_scroll(loop, view, argument, KM_COMMAND_REQUEST_SCROLL_DOWN,
                          error);
}

static KmStatus command_recenter(KmCommandLoop *loop, KmView *view,
                                 int64_t argument, KmError *error)
{
    return request_scroll(loop, view, argument, KM_COMMAND_REQUEST_RECENTER,
                          error);
}

static KmStatus command_goto_line(KmCommandLoop *loop, KmView *view,
                                  int64_t argument, KmError *error)
{
    KmStatus status;

    if (loop->command_has_argument) return move_to_line(view, argument, error);
    status = begin_prompt(loop, KM_PROMPT_GOTO_LINE, error);
    return status == KM_OK ? refresh_prompt_completions(loop, error) : status;
}

static KmStatus command_display_line_numbers_mode(
    KmCommandLoop *loop, KmView *view, int64_t argument, KmError *error)
{
    KmBuffer *buffer = km_view_buffer(view);

    if (loop == NULL || buffer == NULL) {
        return fail(error, KM_ERR_INVALID, "display line numbers");
    }
    km_buffer_set_line_numbers_visible(
        buffer, loop->command_has_argument
                    ? argument > 0
                    : !km_buffer_line_numbers_visible(buffer));
    km_error_clear(error);
    return KM_OK;
}

static KmStatus command_universal_argument(KmCommandLoop *loop, KmView *view,
                                           int64_t argument, KmError *error);
static KmStatus command_request_exit(KmCommandLoop *loop, KmView *view,
                                     int64_t argument, KmError *error);
static KmStatus command_request_save(KmCommandLoop *loop, KmView *view,
                                     int64_t argument, KmError *error);
static KmStatus command_request_save_all_exit(KmCommandLoop *loop,
                                              KmView *view, int64_t argument,
                                              KmError *error);
static KmStatus command_start_search_forward(KmCommandLoop *loop, KmView *view,
                                             int64_t argument,
                                             KmError *error);
static KmStatus command_start_search_backward(KmCommandLoop *loop,
                                              KmView *view, int64_t argument,
                                              KmError *error);
static KmStatus command_find_file(KmCommandLoop *loop, KmView *view,
                                  int64_t argument, KmError *error);
static KmStatus command_switch_buffer(KmCommandLoop *loop, KmView *view,
                                      int64_t argument, KmError *error);
static KmStatus command_kill_buffer(KmCommandLoop *loop, KmView *view,
                                    int64_t argument, KmError *error);
static KmStatus command_extended(KmCommandLoop *loop, KmView *view,
                                 int64_t argument, KmError *error);
static KmStatus command_keyboard_quit(KmCommandLoop *loop, KmView *view,
                                      int64_t argument, KmError *error);
static KmStatus command_minibuffer_backspace(KmCommandLoop *loop, KmView *view,
                                             int64_t argument,
                                             KmError *error);
static KmStatus command_minibuffer_clear(KmCommandLoop *loop, KmView *view,
                                         int64_t argument, KmError *error);
static KmStatus command_minibuffer_next(KmCommandLoop *loop, KmView *view,
                                        int64_t argument, KmError *error);
static KmStatus command_minibuffer_previous(KmCommandLoop *loop, KmView *view,
                                            int64_t argument,
                                            KmError *error);
static KmStatus command_minibuffer_complete(KmCommandLoop *loop, KmView *view,
                                            int64_t argument,
                                            KmError *error);
static KmStatus command_minibuffer_accept_original(
    KmCommandLoop *loop, KmView *view, int64_t argument, KmError *error);
static KmStatus command_minibuffer_accept(KmCommandLoop *loop, KmView *view,
                                          int64_t argument, KmError *error);
static KmStatus command_isearch_repeat_forward(KmCommandLoop *loop,
                                               KmView *view, int64_t argument,
                                               KmError *error);
static KmStatus command_isearch_repeat_backward(KmCommandLoop *loop,
                                                KmView *view,
                                                int64_t argument,
                                                KmError *error);
static KmStatus command_isearch_backspace(KmCommandLoop *loop, KmView *view,
                                          int64_t argument, KmError *error);
static KmStatus command_isearch_accept(KmCommandLoop *loop, KmView *view,
                                       int64_t argument, KmError *error);
static KmStatus command_confirm_yes(KmCommandLoop *loop, KmView *view,
                                    int64_t argument, KmError *error);
static KmStatus command_confirm_no(KmCommandLoop *loop, KmView *view,
                                   int64_t argument, KmError *error);
static KmStatus command_undefined(KmCommandLoop *loop, KmView *view,
                                  int64_t argument, KmError *error);

static const KmCommandSpec command_registry[] = {
    {KM_COMMAND_FORWARD_CHAR, "forward-char", command_forward,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_BACKWARD_CHAR, "backward-char", command_backward,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_FORWARD_WORD, "forward-word", command_forward_word,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_BACKWARD_WORD, "backward-word", command_backward_word,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_FORWARD_PARAGRAPH, "forward-paragraph",
     command_forward_paragraph, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_BACKWARD_PARAGRAPH, "backward-paragraph",
     command_backward_paragraph, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_FORWARD_SENTENCE, "forward-sentence",
     command_forward_sentence, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_BACKWARD_SENTENCE, "backward-sentence",
     command_backward_sentence, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_DELETE_CHAR, "delete-char", command_delete,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_DELETE_BACKWARD_CHAR, "delete-backward-char",
     command_delete_backward, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_KILL_WORD, "kill-word", command_kill_word,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_BACKWARD_KILL_WORD, "backward-kill-word",
     command_backward_kill_word, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_OPEN_LINE, "open-line", command_open_line,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_TRANSPOSE_CHARS, "transpose-chars", command_transpose_chars,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_TRANSPOSE_WORDS, "transpose-words", command_transpose_words,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_DELETE_HORIZONTAL_SPACE, "delete-horizontal-space",
     command_delete_horizontal_space, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_BEGINNING_OF_LINE, "beginning-of-line",
     command_beginning_of_line, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_BACK_TO_INDENTATION, "back-to-indentation",
     command_back_to_indentation, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_END_OF_LINE, "end-of-line", command_end_of_line,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_BEGINNING_OF_BUFFER, "beginning-of-buffer",
     command_beginning_of_buffer, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_END_OF_BUFFER, "end-of-buffer", command_end_of_buffer,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_NEXT_LINE, "next-line", command_next_line,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_PREVIOUS_LINE, "previous-line", command_previous_line,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_SET_MARK, "set-mark-command", command_set_mark,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_MARK_WHOLE_BUFFER, "mark-whole-buffer",
     command_mark_whole_buffer, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_EXCHANGE_POINT_AND_MARK, "exchange-point-and-mark",
     command_exchange_mark, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_KILL_REGION, "kill-region", command_kill_region,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_COPY_REGION, "copy-region-as-kill", command_copy_region,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_KILL_LINE, "kill-line", command_kill_line,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_YANK, "yank", command_yank, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_YANK_POP, "yank-pop", command_yank_pop,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_UNDO, "undo", command_undo, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_REDO, "undo-redo", command_redo,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_SCROLL_UP, "scroll-up-command", command_scroll_up,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_SCROLL_DOWN, "scroll-down-command", command_scroll_down,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_RECENTER, "recenter-top-bottom", command_recenter,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_GOTO_LINE, "goto-line", command_goto_line,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_COMMAND_DISPLAY_LINE_NUMBERS_MODE, "display-line-numbers-mode",
     command_display_line_numbers_mode, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_MX | KM_COMMAND_RECORD_LAST},
    {KM_INTERNAL_UNIVERSAL_ARGUMENT, "universal-argument",
     command_universal_argument, KM_CONTEXT(KM_KEYMAP_GLOBAL),
     KM_COMMAND_KEEP_PREFIX},
    {KM_INTERNAL_EXIT, "exit-editor", command_request_exit,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), 0},
    {KM_INTERNAL_SAVE, "save-buffer", command_request_save,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX},
    {KM_INTERNAL_SAVE_ALL_EXIT, "save-buffers-kill-terminal",
     command_request_save_all_exit, KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX},
    {KM_INTERNAL_SEARCH_FORWARD, "isearch-forward",
     command_start_search_forward, KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX},
    {KM_INTERNAL_SEARCH_BACKWARD, "isearch-backward",
     command_start_search_backward, KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX},
    {KM_INTERNAL_FIND_FILE, "find-file", command_find_file,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX},
    {KM_INTERNAL_SWITCH_BUFFER, "switch-to-buffer", command_switch_buffer,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX},
    {KM_INTERNAL_KILL_BUFFER, "kill-buffer", command_kill_buffer,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), KM_COMMAND_MX},
    {KM_INTERNAL_EXTENDED_COMMAND, "execute-extended-command", command_extended,
     KM_CONTEXT(KM_KEYMAP_GLOBAL), 0},
    {KM_INTERNAL_KEYBOARD_QUIT, "keyboard-quit", command_keyboard_quit,
     KM_CONTEXT(KM_KEYMAP_GLOBAL) | KM_CONTEXT(KM_KEYMAP_MINIBUFFER) |
         KM_CONTEXT(KM_KEYMAP_ISEARCH) |
         KM_CONTEXT(KM_KEYMAP_CONFIRMATION),
     0},
    {KM_INTERNAL_MINIBUFFER_BACKSPACE, "minibuffer-backspace",
     command_minibuffer_backspace, KM_CONTEXT(KM_KEYMAP_MINIBUFFER), 0},
    {KM_INTERNAL_MINIBUFFER_CLEAR, "minibuffer-clear",
     command_minibuffer_clear, KM_CONTEXT(KM_KEYMAP_MINIBUFFER), 0},
    {KM_INTERNAL_MINIBUFFER_NEXT, "minibuffer-next-completion",
     command_minibuffer_next, KM_CONTEXT(KM_KEYMAP_MINIBUFFER), 0},
    {KM_INTERNAL_MINIBUFFER_PREVIOUS, "minibuffer-previous-completion",
     command_minibuffer_previous, KM_CONTEXT(KM_KEYMAP_MINIBUFFER), 0},
    {KM_INTERNAL_MINIBUFFER_COMPLETE, "minibuffer-complete",
     command_minibuffer_complete, KM_CONTEXT(KM_KEYMAP_MINIBUFFER), 0},
    {KM_INTERNAL_MINIBUFFER_ACCEPT_ORIGINAL, "minibuffer-accept-original",
     command_minibuffer_accept_original, KM_CONTEXT(KM_KEYMAP_MINIBUFFER), 0},
    {KM_INTERNAL_MINIBUFFER_ACCEPT, "minibuffer-accept",
     command_minibuffer_accept, KM_CONTEXT(KM_KEYMAP_MINIBUFFER), 0},
    {KM_INTERNAL_ISEARCH_REPEAT_FORWARD, "isearch-repeat-forward",
     command_isearch_repeat_forward, KM_CONTEXT(KM_KEYMAP_ISEARCH), 0},
    {KM_INTERNAL_ISEARCH_REPEAT_BACKWARD, "isearch-repeat-backward",
     command_isearch_repeat_backward, KM_CONTEXT(KM_KEYMAP_ISEARCH), 0},
    {KM_INTERNAL_ISEARCH_BACKSPACE, "isearch-backspace",
     command_isearch_backspace, KM_CONTEXT(KM_KEYMAP_ISEARCH), 0},
    {KM_INTERNAL_ISEARCH_ACCEPT, "isearch-accept", command_isearch_accept,
     KM_CONTEXT(KM_KEYMAP_ISEARCH), 0},
    {KM_INTERNAL_CONFIRM_YES, "confirmation-yes", command_confirm_yes,
     KM_CONTEXT(KM_KEYMAP_CONFIRMATION), 0},
    {KM_INTERNAL_CONFIRM_NO, "confirmation-no", command_confirm_no,
     KM_CONTEXT(KM_KEYMAP_CONFIRMATION), 0},
    {KM_INTERNAL_UNDEFINED, "undefined", command_undefined,
     KM_CONTEXT(KM_KEYMAP_GLOBAL) | KM_CONTEXT(KM_KEYMAP_MINIBUFFER) |
         KM_CONTEXT(KM_KEYMAP_ISEARCH) |
         KM_CONTEXT(KM_KEYMAP_CONFIRMATION),
     0},
};

static void reset_command_input(KmCommandLoop *loop)
{
    loop->key_node = 0;
    loop->keymap_id = KM_KEYMAP_GLOBAL;
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

static bool valid_key_code(uint32_t codepoint)
{
    return valid_scalar(codepoint) ||
           (codepoint >= KM_KEY_ESCAPE && codepoint <= KM_KEY_DELETE);
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
        if (event->repeat == 0 || !valid_key_code(event->codepoint) ||
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

static size_t find_key_child(const KmKeymap *keymap, size_t parent,
                             uint32_t codepoint, uint32_t modifiers)
{
    size_t node = keymap->nodes[parent].child;

    while (node != KM_NO_KEY_NODE) {
        if (keymap->nodes[node].codepoint == codepoint &&
            keymap->nodes[node].modifiers == modifiers) {
            return node;
        }
        node = keymap->nodes[node].sibling;
    }
    return KM_NO_KEY_NODE;
}

static const KmCommandSpec *find_command(int id)
{
    size_t i;

    for (i = 0; i < sizeof(command_registry) / sizeof(command_registry[0]); ++i) {
        if (command_registry[i].id == id) return &command_registry[i];
    }
    return NULL;
}

static const KmCommandSpec *find_command_name(const char *name)
{
    size_t i;

    if (name == NULL) return NULL;
    for (i = 0; i < sizeof(command_registry) / sizeof(command_registry[0]); ++i) {
        if (strcmp(command_registry[i].name, name) == 0) {
            return &command_registry[i];
        }
    }
    return NULL;
}

static KmStatus reserve_keymap(KmKeymap *keymap, size_t required,
                               KmError *error)
{
    size_t capacity;
    KmKeyNode *nodes;

    if (required <= keymap->capacity) return KM_OK;
    capacity = keymap->capacity == 0 ? 16 : keymap->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*nodes)) {
        return fail(error, KM_ERR_OOM, "keymap binding");
    }
    nodes = (KmKeyNode *)realloc(keymap->nodes, capacity * sizeof(*nodes));
    if (nodes == NULL) return fail(error, KM_ERR_OOM, "keymap binding");
    keymap->nodes = nodes;
    keymap->capacity = capacity;
    return KM_OK;
}

static KmStatus bind_key(KmCommandLoop *loop, KmKeymapId keymap_id,
                         const KmKeyStroke *sequence, size_t count,
                         const KmCommandSpec *command, KmError *error)
{
    KmKeymap *keymap;
    size_t node = 0;
    size_t existing = 0;
    size_t i;
    KmStatus status;

    if (loop == NULL || keymap_id < KM_KEYMAP_GLOBAL ||
        keymap_id >= KM_KEYMAP_COUNT || sequence == NULL || count == 0 ||
        command == NULL ||
        (command->contexts & KM_CONTEXT(keymap_id)) == 0) {
        return fail(error, KM_ERR_INVALID, "keymap binding");
    }
    for (i = 0; i < count; ++i) {
        if (!valid_key_code(sequence[i].codepoint) ||
            (sequence[i].modifiers &
             ~(uint32_t)(KM_MOD_CTRL | KM_MOD_ALT | KM_MOD_SHIFT)) != 0) {
            return fail(error, KM_ERR_INVALID, "keymap binding");
        }
    }
    keymap = &loop->keymaps[keymap_id];
    while (existing < count) {
        size_t child = find_key_child(keymap, node,
                                      sequence[existing].codepoint,
                                      sequence[existing].modifiers);
        if (child == KM_NO_KEY_NODE) break;
        node = child;
        ++existing;
        if (existing < count && keymap->nodes[node].command != KM_COMMAND_NONE) {
            return fail(error, KM_ERR_CONFLICT, "keymap binding");
        }
    }
    if (existing == count) {
        if (keymap->nodes[node].child != KM_NO_KEY_NODE) {
            return fail(error, KM_ERR_CONFLICT, "keymap binding");
        }
        keymap->nodes[node].command = command->id;
        return KM_OK;
    }
    if (count - existing > SIZE_MAX - keymap->count) {
        return fail(error, KM_ERR_OOM, "keymap binding");
    }
    status = reserve_keymap(keymap, keymap->count + count - existing, error);
    if (status != KM_OK) return status;
    for (i = existing; i < count; ++i) {
        size_t next = keymap->count++;
        KmKeyNode *parent = &keymap->nodes[node];

        keymap->nodes[next] = (KmKeyNode){
            sequence[i].codepoint,
            sequence[i].modifiers,
            KM_COMMAND_NONE,
            KM_NO_KEY_NODE,
            parent->child,
        };
        parent->child = next;
        node = next;
    }
    keymap->nodes[node].command = command->id;
    return KM_OK;
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

static KmStatus execute_registered_with_argument(
    KmCommandLoop *loop, KmView *view, int id, KmKeymapId context,
    int64_t argument, bool has_argument, bool page_opposite, KmError *error)
{
    const KmCommandSpec *command = find_command(id);
    KmStatus status;

    if (command == NULL ||
        (command->contexts & KM_CONTEXT(context)) == 0) {
        reset_command_input(loop);
        return fail(error, KM_ERR_INVALID, "command registry");
    }
    if ((command->flags & KM_COMMAND_KEEP_PREFIX) == 0) {
        reset_command_input(loop);
    }
    if (id != KM_COMMAND_YANK && id != KM_COMMAND_YANK_POP &&
        id != KM_INTERNAL_UNIVERSAL_ARGUMENT) {
        loop->yank_buffer = NULL;
    }
    loop->command_has_argument = has_argument;
    loop->command_page_opposite = page_opposite;
    status = command->callback(loop, view, argument, error);
    loop->command_has_argument = false;
    loop->command_page_opposite = false;
    if (status == KM_OK && (command->flags & KM_COMMAND_RECORD_LAST) != 0) {
        loop->last_command = (KmCommandId)id;
    }
    return status;
}

static KmStatus execute_registered_command(KmCommandLoop *loop, KmView *view,
                                           int id, KmKeymapId context,
                                           KmError *error)
{
    int64_t argument = current_argument(loop);
    bool has_argument = loop->prefix_kind != KM_PREFIX_NONE;
    bool page_opposite = loop->prefix_kind == KM_PREFIX_NUMERIC &&
                         loop->prefix_negative &&
                         !loop->prefix_has_digits;

    return execute_registered_with_argument(loop, view, id, context, argument,
                                            has_argument, page_opposite, error);
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
    loop->yank_buffer = NULL;
    status = insert_utf8_repeated(view, bytes, len, argument,
                                  KM_COMMAND_INSERT_UTF8_BLOCK, error);
    if (status == KM_OK) loop->last_command = KM_COMMAND_INSERT_UTF8_BLOCK;
    return status;
}

static bool valid_utf8_block(const uint8_t *text, size_t len)
{
    size_t offset = 0;

    if (len != 0 && text == NULL) return false;
    while (offset < len) {
        int32_t codepoint;
        size_t consumed;
        if (km_unicode_decode(text, len, offset, &codepoint, &consumed, NULL) !=
            KM_OK) {
            return false;
        }
        offset += consumed;
    }
    return true;
}

static KmStatus begin_prompt(KmCommandLoop *loop, KmPromptKind kind,
                             KmError *error)
{
    reset_command_input(loop);
    if (loop->request != KM_COMMAND_REQUEST_NONE ||
        loop->prompt_kind != KM_PROMPT_NONE || loop->search_active) {
        return fail(error, KM_ERR_CONFLICT, "minibuffer");
    }
    loop->prompt_kind = kind;
    loop->prompt_argument = 1;
    loop->prompt_has_argument = false;
    loop->prompt_page_opposite = false;
    loop->prompt_len = 0;
    if (loop->prompt_text != NULL) loop->prompt_text[0] = '\0';
    return KM_OK;
}

static bool valid_prompt_text(const uint8_t *text, size_t len)
{
    size_t offset = 0;

    if (!valid_utf8_block(text, len)) return false;
    while (offset < len) {
        int32_t codepoint;
        size_t consumed;
        if (km_unicode_decode(text, len, offset, &codepoint, &consumed, NULL) !=
                KM_OK ||
            codepoint < 0x20 || codepoint == 0x7f) {
            return false;
        }
        offset += consumed;
    }
    return true;
}

static KmStatus replace_prompt_text(KmCommandLoop *loop, const char *text,
                                    KmError *error)
{
    size_t len;
    uint8_t *replacement;

    if (text == NULL) return fail(error, KM_ERR_INVALID, "minibuffer text");
    len = strlen(text);
    if (!valid_prompt_text((const uint8_t *)text, len) || len == SIZE_MAX) {
        return fail(error, KM_ERR_INVALID, "minibuffer text");
    }
    replacement = (uint8_t *)malloc(len + 1);
    if (replacement == NULL) return fail(error, KM_ERR_OOM, "minibuffer text");
    memcpy(replacement, text, len + 1);
    free(loop->prompt_text);
    loop->prompt_text = replacement;
    loop->prompt_len = len;
    loop->prompt_cap = len + 1;
    return KM_OK;
}

static KmStatus append_prompt_text(KmCommandLoop *loop, const uint8_t *text,
                                   size_t len, KmError *error)
{
    size_t required;
    size_t capacity;
    uint8_t *input;

    if (!valid_prompt_text(text, len) ||
        len > SIZE_MAX - loop->prompt_len - 1) {
        return fail(error, KM_ERR_INVALID, "minibuffer text");
    }
    required = loop->prompt_len + len + 1;
    if (required > loop->prompt_cap) {
        capacity = loop->prompt_cap == 0 ? 64 : loop->prompt_cap;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        input = (uint8_t *)realloc(loop->prompt_text, capacity);
        if (input == NULL) return fail(error, KM_ERR_OOM, "minibuffer text");
        loop->prompt_text = input;
        loop->prompt_cap = capacity;
    }
    if (len != 0) memcpy(loop->prompt_text + loop->prompt_len, text, len);
    loop->prompt_len += len;
    loop->prompt_text[loop->prompt_len] = '\0';
    return KM_OK;
}

static void prompt_backspace(KmCommandLoop *loop)
{
    if (loop->prompt_len == 0) return;
    --loop->prompt_len;
    while (loop->prompt_len != 0 &&
           (loop->prompt_text[loop->prompt_len] & 0xc0u) == 0x80u) {
        --loop->prompt_len;
    }
    loop->prompt_text[loop->prompt_len] = '\0';
}

static void clear_completions(KmCommandLoop *loop)
{
    size_t i;

    for (i = 0; i < loop->completion_count; ++i) {
        free(loop->completions[i]);
    }
    free(loop->completions);
    free(loop->completion_common);
    loop->completions = NULL;
    loop->completion_count = 0;
    loop->completion_index = 0;
    loop->completion_common = NULL;
}

static size_t utf8_common_prefix(const char *left, const char *right)
{
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    size_t common = left_len < right_len ? left_len : right_len;
    size_t offset = 0;

    while (offset < common && left[offset] == right[offset]) ++offset;
    common = offset;
    while (common != 0 && common < left_len &&
           ((unsigned char)left[common] & 0xc0u) == 0x80u) {
        --common;
    }
    return common;
}

static int compare_completions(const void *left, const void *right)
{
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return strcmp(*a, *b);
}

static KmStatus replace_completions(KmCommandLoop *loop,
                                    const char *const *items, size_t count,
                                    const char *common, KmError *error)
{
    char **copies = NULL;
    char *common_copy = NULL;
    size_t common_len = 0;
    size_t i;

    if (count > SIZE_MAX / sizeof(*copies)) {
        return fail(error, KM_ERR_OOM, "minibuffer completions");
    }
    if (count != 0) {
        copies = (char **)calloc(count, sizeof(*copies));
        if (copies == NULL) {
            return fail(error, KM_ERR_OOM, "minibuffer completions");
        }
    }
    for (i = 0; i < count; ++i) {
        size_t len;
        if (items == NULL || items[i] == NULL ||
            !valid_prompt_text((const uint8_t *)items[i], strlen(items[i]))) {
            goto invalid;
        }
        len = strlen(items[i]);
        copies[i] = (char *)malloc(len + 1);
        if (copies[i] == NULL) goto oom;
        memcpy(copies[i], items[i], len + 1);
    }
    if (count != 0) {
        qsort(copies, count, sizeof(*copies), compare_completions);
        common_len = strlen(copies[0]);
        for (i = 1; i < count; ++i) {
            size_t candidate_common = utf8_common_prefix(copies[0], copies[i]);
            if (candidate_common < common_len) common_len = candidate_common;
        }
    }
    if (common != NULL) {
        if (!valid_prompt_text((const uint8_t *)common, strlen(common))) {
            goto invalid;
        }
        common_len = strlen(common);
        common_copy = (char *)malloc(common_len + 1);
        if (common_copy == NULL) goto oom;
        memcpy(common_copy, common, common_len + 1);
    } else if (count != 0) {
        common_copy = (char *)malloc(common_len + 1);
        if (common_copy == NULL) goto oom;
        memcpy(common_copy, copies[0], common_len);
        common_copy[common_len] = '\0';
    }
    clear_completions(loop);
    loop->completions = copies;
    loop->completion_count = count;
    loop->completion_common = common_copy;
    return KM_OK;

invalid:
    for (i = 0; i < count; ++i) free(copies[i]);
    free(copies);
    free(common_copy);
    return fail(error, KM_ERR_INVALID, "minibuffer completions");

oom:
    for (i = 0; i < count; ++i) free(copies[i]);
    free(copies);
    free(common_copy);
    return fail(error, KM_ERR_OOM, "minibuffer completions");
}

static KmStatus refresh_prompt_completions(KmCommandLoop *loop,
                                           KmError *error)
{
    const char *matches[sizeof(command_registry) / sizeof(command_registry[0])];
    size_t count = 0;
    size_t i;

    clear_completions(loop);
    if (loop->prompt_kind == KM_PROMPT_FIND_FILE ||
        loop->prompt_kind == KM_PROMPT_SWITCH_BUFFER) {
        loop->request = loop->prompt_kind == KM_PROMPT_FIND_FILE
                            ? KM_COMMAND_REQUEST_COMPLETE_FILE
                            : KM_COMMAND_REQUEST_COMPLETE_BUFFER;
        return KM_OK;
    }
    if (loop->prompt_kind != KM_PROMPT_COMMAND) return KM_OK;
    for (i = 0; i < sizeof(command_registry) / sizeof(command_registry[0]); ++i) {
        const char *candidate = command_registry[i].name;
        size_t len = strlen(candidate);
        if ((command_registry[i].flags & KM_COMMAND_MX) != 0 &&
            len >= loop->prompt_len &&
            (loop->prompt_len == 0 ||
             memcmp(candidate, loop->prompt_text, loop->prompt_len) == 0)) {
            matches[count++] = candidate;
        }
    }
    return replace_completions(loop, matches, count, NULL, error);
}

static KmStatus complete_prompt(KmCommandLoop *loop, KmError *error)
{
    KmStatus status;

    if (loop->completion_common == NULL ||
        strlen(loop->completion_common) <= loop->prompt_len) {
        return KM_OK;
    }
    status = replace_prompt_text(loop, loop->completion_common, error);
    if (status != KM_OK) return status;
    return refresh_prompt_completions(loop, error);
}

static KmStatus execute_named_command(KmCommandLoop *loop, KmView *view,
                                      KmError *error)
{
    const KmCommandSpec *command;
    int64_t argument;
    bool has_argument;
    bool page_opposite;

    if (loop->prompt_len == 0) {
        return fail(error, KM_ERR_INVALID, "command name empty");
    }
    command = find_command_name((const char *)loop->prompt_text);
    if (command == NULL || (command->flags & KM_COMMAND_MX) == 0) {
        return fail(error, KM_ERR_INVALID, "unknown command");
    }
    argument = loop->prompt_argument;
    has_argument = loop->prompt_has_argument;
    page_opposite = loop->prompt_page_opposite;
    loop->prompt_kind = KM_PROMPT_NONE;
    return execute_registered_with_argument(
        loop, view, command->id, KM_KEYMAP_GLOBAL, argument,
        has_argument, page_opposite, error);
}

static bool prompt_ends_in_separator(const KmCommandLoop *loop)
{
    if (loop->prompt_len == 0) return false;
    return loop->prompt_text[loop->prompt_len - 1] == '/' ||
           loop->prompt_text[loop->prompt_len - 1] == '\\';
}

static void cycle_completion(KmCommandLoop *loop, bool forward)
{
    if (loop->completion_count < 2) return;
    if (forward) {
        loop->completion_index =
            (loop->completion_index + 1) % loop->completion_count;
    } else {
        loop->completion_index = loop->completion_index == 0
                                     ? loop->completion_count - 1
                                     : loop->completion_index - 1;
    }
}

static KmStatus fido_backspace(KmCommandLoop *loop, KmError *error)
{
    if (loop->prompt_kind == KM_PROMPT_FIND_FILE &&
        prompt_ends_in_separator(loop) &&
        !(loop->prompt_len == 1 ||
          (loop->prompt_len == 3 && loop->prompt_text[1] == ':'))) {
        --loop->prompt_len;
        while (loop->prompt_len != 0 &&
               loop->prompt_text[loop->prompt_len - 1] != '/' &&
               loop->prompt_text[loop->prompt_len - 1] != '\\') {
            --loop->prompt_len;
        }
        loop->prompt_text[loop->prompt_len] = '\0';
    } else {
        prompt_backspace(loop);
    }
    return refresh_prompt_completions(loop, error);
}

static KmStatus accept_prompt(KmCommandLoop *loop, KmView *view,
                              bool use_completion, KmError *error)
{
    if (use_completion && loop->prompt_len != 0 &&
        loop->completion_count != 0) {
        const char *selected = loop->completions[loop->completion_index];
        KmStatus status = replace_prompt_text(loop, selected, error);
        if (status != KM_OK) return status;
        if (loop->prompt_kind == KM_PROMPT_FIND_FILE &&
            prompt_ends_in_separator(loop)) {
            return refresh_prompt_completions(loop, error);
        }
    }
    if (loop->prompt_kind == KM_PROMPT_COMMAND) {
        return execute_named_command(loop, view, error);
    }
    if (loop->prompt_kind == KM_PROMPT_GOTO_LINE) {
        int64_t line = 0;
        size_t i;

        if (loop->prompt_len == 0) {
            return fail(error, KM_ERR_INVALID, "line number");
        }
        for (i = 0; i < loop->prompt_len; ++i) {
            uint8_t byte = loop->prompt_text[i];
            if (byte < '0' || byte > '9' ||
                line > (INT64_MAX - (int64_t)(byte - '0')) / 10) {
                return fail(error, KM_ERR_INVALID, "line number");
            }
            line = line * 10 + (int64_t)(byte - '0');
        }
        {
            KmStatus status = move_to_line(view, line, error);
            if (status != KM_OK) return status;
        }
        loop->prompt_kind = KM_PROMPT_NONE;
        return KM_OK;
    }
    loop->request = loop->prompt_kind == KM_PROMPT_FIND_FILE
                        ? KM_COMMAND_REQUEST_FIND_FILE
                        : KM_COMMAND_REQUEST_SWITCH_BUFFER;
    loop->prompt_kind = KM_PROMPT_NONE;
    return KM_OK;
}

static KmStatus dispatch_minibuffer_text(KmCommandLoop *loop,
                                         const KmEvent *event,
                                         KmError *error)
{
    if (loop->prompt_kind == KM_PROMPT_CONFIRM_KILL ||
        loop->prompt_kind == KM_PROMPT_CONFIRM_EXIT) {
        return fail(error, KM_ERR_INVALID, "confirmation key");
    }
    if (event->kind == KM_EVENT_PASTE) {
        KmStatus status =
            append_prompt_text(loop, event->text, event->text_len, error);
        return status == KM_OK ? refresh_prompt_completions(loop, error)
                               : status;
    }
    if (event->kind == KM_EVENT_TEXT) {
        KmStatus status;
        if (event->text != NULL || event->text_len != 0) {
            status = append_prompt_text(loop, event->text, event->text_len,
                                        error);
        } else {
            uint8_t bytes[4];
            size_t len = encode_scalar(event->codepoint, bytes);
            status = append_prompt_text(loop, bytes, len, error);
        }
        return status == KM_OK ? refresh_prompt_completions(loop, error)
                               : status;
    }
    return fail(error, KM_ERR_INVALID, "minibuffer key");
}

static bool search_match_at(const KmCommandLoop *loop,
                            const KmDocument *document, const uint8_t *text,
                            KmBytePos begv, KmBytePos zv, size_t scan)
{
    KmBytePos candidate = {begv.v + scan};
    KmBytePos candidate_end = {candidate.v + loop->search_len};

    return candidate_end.v <= zv.v &&
           km_document_is_boundary(document, candidate) &&
           km_document_is_boundary(document, candidate_end) &&
           memcmp(text + scan, loop->search_query, loop->search_len) == 0;
}

static KmBytePos accessible_search_origin(const KmCommandLoop *loop,
                                          const KmView *view)
{
    KmBytePos begv = km_anchor_get(view->buffer->begv);
    KmBytePos zv = km_anchor_get(view->buffer->zv);
    KmBytePos origin = loop->search_origin;

    if (origin.v < begv.v) return begv;
    if (origin.v > zv.v) return zv;
    return origin;
}

static KmStatus search_query(KmCommandLoop *loop, KmView *view,
                             bool repeat, KmError *error)
{
    const KmDocument *document = view->buffer->document;
    KmBytePos begv = km_anchor_get(view->buffer->begv);
    KmBytePos zv = km_anchor_get(view->buffer->zv);
    KmBytePos origin = accessible_search_origin(loop, view);
    uint8_t *text;
    size_t len = zv.v - begv.v;
    KmBytePos point = km_anchor_get(view->point);
    size_t relative_origin = origin.v - begv.v;
    size_t relative_point = point.v <= begv.v
                                ? 0
                                : point.v >= zv.v ? len : point.v - begv.v;
    size_t found = SIZE_MAX;
    bool continue_wrapped = !repeat && loop->search_wrapped;
    bool wrapped = continue_wrapped;

    if (loop->search_len == 0) {
        loop->search_failed = false;
        loop->search_wrapped = false;
        return km_anchor_set(view->point, origin, error);
    }
    if (loop->search_len > len) {
        loop->search_failed = true;
        return KM_OK;
    }
    text = len == 0 ? NULL : (uint8_t *)malloc(len);
    if (len != 0 && text == NULL) return fail(error, KM_ERR_OOM, "search");
    if (len != 0) {
        KmStatus status = km_document_copy(document, begv, len, text, error);
        if (status != KM_OK) {
            free(text);
            return status;
        }
    }
    /* ponytail: byte scan; add a search index only after profiling demands it. */
    if (loop->search_forward) {
        size_t max_start = len - loop->search_len;
        size_t start = repeat && relative_point < len
                           ? relative_point + 1
                           : continue_wrapped ? relative_point
                                              : relative_origin;
        size_t scan;

        for (scan = start; scan <= max_start; ++scan) {
            if (search_match_at(loop, document, text, begv, zv, scan)) {
                found = scan;
                break;
            }
        }
        if ((repeat || continue_wrapped) && found == SIZE_MAX) {
            for (scan = 0; scan < start && scan <= max_start; ++scan) {
                if (search_match_at(loop, document, text, begv, zv, scan)) {
                    found = scan;
                    wrapped = true;
                    break;
                }
            }
        }
    } else {
        size_t max_start = len - loop->search_len;
        size_t first = 0;
        bool has_first;

        if (repeat) {
            has_first = relative_point != 0;
            if (has_first) first = relative_point - 1;
        } else if (continue_wrapped) {
            has_first = true;
            first = relative_point;
        } else {
            has_first = relative_origin >= loop->search_len;
            if (has_first) first = relative_origin - loop->search_len;
        }
        if (has_first) {
            size_t scan = first < max_start ? first : max_start;
            first = scan;
            for (;;) {
                if (search_match_at(loop, document, text, begv, zv, scan)) {
                    found = scan;
                    break;
                }
                if (scan == 0) break;
                --scan;
            }
        }
        if ((repeat || continue_wrapped) && found == SIZE_MAX &&
            (!has_first || first < max_start)) {
            size_t scan = max_start;
            for (;;) {
                if ((!has_first || scan > first) &&
                    search_match_at(loop, document, text, begv, zv, scan)) {
                    found = scan;
                    wrapped = true;
                    break;
                }
                if (scan == 0 || (has_first && scan <= first)) break;
                --scan;
            }
        }
    }
    free(text);
    loop->search_failed = found == SIZE_MAX;
    loop->search_wrapped = wrapped && (found != SIZE_MAX || continue_wrapped);
    if (found == SIZE_MAX) return KM_OK;
    return km_anchor_set(view->point, (KmBytePos){begv.v + found}, error);
}

static KmStatus append_search_query(KmCommandLoop *loop, KmView *view,
                                    const uint8_t *text, size_t len,
                                    KmError *error)
{
    size_t required;
    size_t capacity;
    uint8_t *query;

    if (!valid_utf8_block(text, len) || len > SIZE_MAX - loop->search_len) {
        return fail(error, KM_ERR_INVALID, "search text");
    }
    required = loop->search_len + len;
    if (required > loop->search_cap) {
        capacity = loop->search_cap == 0 ? 32 : loop->search_cap;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        query = (uint8_t *)realloc(loop->search_query, capacity);
        if (query == NULL) return fail(error, KM_ERR_OOM, "search text");
        loop->search_query = query;
        loop->search_cap = capacity;
    }
    if (len != 0) memcpy(loop->search_query + loop->search_len, text, len);
    loop->search_len = required;
    return search_query(loop, view, false, error);
}

static KmStatus search_backspace(KmCommandLoop *loop, KmView *view,
                                 KmError *error)
{
    if (loop->search_len == 0) return KM_OK;
    --loop->search_len;
    while (loop->search_len != 0 &&
           (loop->search_query[loop->search_len] & 0xc0u) == 0x80u) {
        --loop->search_len;
    }
    return search_query(loop, view, false, error);
}

static KmKeymapId active_keymap(const KmCommandLoop *loop)
{
    if (loop->search_active) return KM_KEYMAP_ISEARCH;
    if (loop->prompt_kind == KM_PROMPT_CONFIRM_KILL ||
        loop->prompt_kind == KM_PROMPT_CONFIRM_EXIT) {
        return KM_KEYMAP_CONFIRMATION;
    }
    if (loop->prompt_kind != KM_PROMPT_NONE) return KM_KEYMAP_MINIBUFFER;
    return KM_KEYMAP_GLOBAL;
}

static KmStatus command_universal_argument(KmCommandLoop *loop, KmView *view,
                                           int64_t argument, KmError *error)
{
    (void)view;
    (void)argument;
    return universal_argument(loop, error);
}

static KmStatus command_request_exit(KmCommandLoop *loop, KmView *view,
                                     int64_t argument, KmError *error)
{
    (void)view;
    (void)argument;
    (void)error;
    loop->request = KM_COMMAND_REQUEST_EXIT;
    return KM_OK;
}

static KmStatus command_request_save(KmCommandLoop *loop, KmView *view,
                                     int64_t argument, KmError *error)
{
    (void)view;
    (void)argument;
    (void)error;
    loop->request = KM_COMMAND_REQUEST_SAVE;
    return KM_OK;
}

static KmStatus command_request_save_all_exit(KmCommandLoop *loop,
                                              KmView *view, int64_t argument,
                                              KmError *error)
{
    (void)view;
    (void)argument;
    (void)error;
    loop->request = KM_COMMAND_REQUEST_SAVE_ALL_EXIT;
    return KM_OK;
}

static KmStatus start_search(KmCommandLoop *loop, KmView *view, bool forward)
{
    loop->search_origin = km_anchor_get(view->point);
    loop->search_len = 0;
    loop->search_active = true;
    loop->search_forward = forward;
    loop->search_failed = false;
    loop->search_wrapped = false;
    return KM_OK;
}

static KmStatus command_start_search_forward(KmCommandLoop *loop, KmView *view,
                                             int64_t argument, KmError *error)
{
    (void)argument;
    (void)error;
    return start_search(loop, view, true);
}

static KmStatus command_start_search_backward(KmCommandLoop *loop,
                                              KmView *view, int64_t argument,
                                              KmError *error)
{
    (void)argument;
    (void)error;
    return start_search(loop, view, false);
}

static KmStatus command_find_file(KmCommandLoop *loop, KmView *view,
                                  int64_t argument, KmError *error)
{
    KmStatus status;
    (void)view;
    (void)argument;
    status = begin_prompt(loop, KM_PROMPT_FIND_FILE, error);
    return status == KM_OK ? refresh_prompt_completions(loop, error) : status;
}

static KmStatus command_switch_buffer(KmCommandLoop *loop, KmView *view,
                                      int64_t argument, KmError *error)
{
    KmStatus status;
    (void)view;
    (void)argument;
    status = begin_prompt(loop, KM_PROMPT_SWITCH_BUFFER, error);
    return status == KM_OK ? refresh_prompt_completions(loop, error) : status;
}

static KmStatus command_kill_buffer(KmCommandLoop *loop, KmView *view,
                                    int64_t argument, KmError *error)
{
    (void)argument;
    if (km_buffer_is_modified(view->buffer)) {
        return begin_prompt(loop, KM_PROMPT_CONFIRM_KILL, error);
    }
    loop->request = KM_COMMAND_REQUEST_KILL_BUFFER;
    return KM_OK;
}

static KmStatus command_extended(KmCommandLoop *loop, KmView *view,
                                 int64_t argument, KmError *error)
{
    bool has_argument = loop->command_has_argument;
    bool page_opposite = loop->command_page_opposite;
    KmStatus status;
    (void)view;

    status = begin_prompt(loop, KM_PROMPT_COMMAND, error);
    if (status == KM_OK) {
        loop->prompt_argument = argument;
        loop->prompt_has_argument = has_argument;
        loop->prompt_page_opposite = page_opposite;
        status = refresh_prompt_completions(loop, error);
    }
    return status;
}

static KmStatus command_keyboard_quit(KmCommandLoop *loop, KmView *view,
                                      int64_t argument, KmError *error)
{
    KmKeymapId context = active_keymap(loop);
    KmStatus status = KM_OK;
    (void)argument;

    if (context == KM_KEYMAP_ISEARCH) {
        status = km_anchor_set(view->point,
                               accessible_search_origin(loop, view), error);
        loop->search_active = false;
        loop->search_failed = false;
        loop->search_wrapped = false;
        view->buffer->mark_active = false;
    } else if (context == KM_KEYMAP_MINIBUFFER ||
               context == KM_KEYMAP_CONFIRMATION) {
        loop->prompt_kind = KM_PROMPT_NONE;
        if (loop->request == KM_COMMAND_REQUEST_COMPLETE_FILE ||
            loop->request == KM_COMMAND_REQUEST_COMPLETE_BUFFER) {
            loop->request = KM_COMMAND_REQUEST_NONE;
        }
    } else {
        view->buffer->mark_active = false;
    }
    loop->quit_requested = true;
    return status;
}

static KmStatus command_minibuffer_backspace(KmCommandLoop *loop, KmView *view,
                                             int64_t argument,
                                             KmError *error)
{
    (void)view;
    (void)argument;
    return fido_backspace(loop, error);
}

static KmStatus command_minibuffer_clear(KmCommandLoop *loop, KmView *view,
                                         int64_t argument, KmError *error)
{
    (void)view;
    (void)argument;
    loop->prompt_len = 0;
    if (loop->prompt_text != NULL) loop->prompt_text[0] = '\0';
    return refresh_prompt_completions(loop, error);
}

static KmStatus command_minibuffer_next(KmCommandLoop *loop, KmView *view,
                                        int64_t argument, KmError *error)
{
    (void)view;
    (void)argument;
    (void)error;
    cycle_completion(loop, true);
    return KM_OK;
}

static KmStatus command_minibuffer_previous(KmCommandLoop *loop, KmView *view,
                                            int64_t argument,
                                            KmError *error)
{
    (void)view;
    (void)argument;
    (void)error;
    cycle_completion(loop, false);
    return KM_OK;
}

static KmStatus command_minibuffer_complete(KmCommandLoop *loop, KmView *view,
                                            int64_t argument,
                                            KmError *error)
{
    (void)view;
    (void)argument;
    return complete_prompt(loop, error);
}

static KmStatus command_minibuffer_accept_original(
    KmCommandLoop *loop, KmView *view, int64_t argument, KmError *error)
{
    (void)argument;
    return accept_prompt(loop, view, false, error);
}

static KmStatus command_minibuffer_accept(KmCommandLoop *loop, KmView *view,
                                          int64_t argument, KmError *error)
{
    (void)argument;
    return accept_prompt(loop, view, true, error);
}

static KmStatus command_isearch_repeat_forward(KmCommandLoop *loop,
                                               KmView *view, int64_t argument,
                                               KmError *error)
{
    (void)argument;
    loop->search_forward = true;
    return search_query(loop, view, true, error);
}

static KmStatus command_isearch_repeat_backward(KmCommandLoop *loop,
                                                KmView *view,
                                                int64_t argument,
                                                KmError *error)
{
    (void)argument;
    loop->search_forward = false;
    return search_query(loop, view, true, error);
}

static KmStatus command_isearch_backspace(KmCommandLoop *loop, KmView *view,
                                          int64_t argument, KmError *error)
{
    (void)argument;
    return search_backspace(loop, view, error);
}

static KmStatus command_isearch_accept(KmCommandLoop *loop, KmView *view,
                                       int64_t argument, KmError *error)
{
    (void)view;
    (void)argument;
    (void)error;
    loop->search_active = false;
    loop->search_failed = false;
    loop->search_wrapped = false;
    loop->last_command = loop->search_forward ? KM_COMMAND_SEARCH_FORWARD
                                               : KM_COMMAND_SEARCH_BACKWARD;
    return KM_OK;
}

static KmStatus command_confirm_yes(KmCommandLoop *loop, KmView *view,
                                    int64_t argument, KmError *error)
{
    (void)view;
    (void)argument;
    (void)error;
    loop->request = loop->prompt_kind == KM_PROMPT_CONFIRM_KILL
                        ? KM_COMMAND_REQUEST_KILL_BUFFER
                        : KM_COMMAND_REQUEST_EXIT_CONFIRMED;
    loop->prompt_kind = KM_PROMPT_NONE;
    return KM_OK;
}

static KmStatus command_confirm_no(KmCommandLoop *loop, KmView *view,
                                   int64_t argument, KmError *error)
{
    (void)view;
    (void)argument;
    (void)error;
    loop->prompt_kind = KM_PROMPT_NONE;
    return KM_OK;
}

static KmStatus command_undefined(KmCommandLoop *loop, KmView *view,
                                  int64_t argument, KmError *error)
{
    (void)loop;
    (void)view;
    (void)argument;
    return fail(error, KM_ERR_INVALID, "undefined key");
}

static KmStatus dispatch_isearch_text(KmCommandLoop *loop, KmView *view,
                                      const KmEvent *event, KmError *error)
{
    if (event->kind == KM_EVENT_TEXT) {
        if (event->text != NULL || event->text_len != 0) {
            return append_search_query(loop, view, event->text, event->text_len,
                                       error);
        } else {
            uint8_t bytes[4];
            size_t len = encode_scalar(event->codepoint, bytes);
            return append_search_query(loop, view, bytes, len, error);
        }
    }
    return fail(error, KM_ERR_INVALID, "search key");
}

typedef struct {
    KmKeymapId keymap;
    size_t count;
    KmKeyStroke sequence[2];
    const char *command_name;
} KmDefaultBinding;

#define KM_BIND1(map, code, mods, command)                                     \
    { (map), 1, {{(code), (mods)}, {0, 0}}, (command) }
#define KM_BIND2(map, code1, mods1, code2, mods2, command)                    \
    { (map), 2, {{(code1), (mods1)}, {(code2), (mods2)}}, (command) }

static const KmDefaultBinding default_bindings[] = {
    KM_BIND1(KM_KEYMAP_GLOBAL, 'a', KM_MOD_CTRL, "beginning-of-line"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'b', KM_MOD_CTRL, "backward-char"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'd', KM_MOD_CTRL, "delete-char"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'e', KM_MOD_CTRL, "end-of-line"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'f', KM_MOD_CTRL, "forward-char"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'g', KM_MOD_CTRL, "keyboard-quit"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'k', KM_MOD_CTRL, "kill-line"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'l', KM_MOD_CTRL, "recenter-top-bottom"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'n', KM_MOD_CTRL, "next-line"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'o', KM_MOD_CTRL, "open-line"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'p', KM_MOD_CTRL, "previous-line"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'r', KM_MOD_CTRL, "isearch-backward"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 's', KM_MOD_CTRL, "isearch-forward"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 't', KM_MOD_CTRL, "transpose-chars"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'u', KM_MOD_CTRL, "universal-argument"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'v', KM_MOD_CTRL, "scroll-up-command"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'w', KM_MOD_CTRL, "kill-region"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'y', KM_MOD_CTRL, "yank"),
    KM_BIND1(KM_KEYMAP_GLOBAL, ' ', KM_MOD_CTRL, "set-mark-command"),
    KM_BIND1(KM_KEYMAP_GLOBAL, '/', KM_MOD_CTRL, "undo"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 0x7f, 0, "delete-backward-char"),
    KM_BIND1(KM_KEYMAP_GLOBAL, KM_KEY_LEFT, 0, "backward-char"),
    KM_BIND1(KM_KEYMAP_GLOBAL, KM_KEY_RIGHT, 0, "forward-char"),
    KM_BIND1(KM_KEYMAP_GLOBAL, KM_KEY_UP, 0, "previous-line"),
    KM_BIND1(KM_KEYMAP_GLOBAL, KM_KEY_DOWN, 0, "next-line"),
    KM_BIND1(KM_KEYMAP_GLOBAL, KM_KEY_HOME, 0, "beginning-of-line"),
    KM_BIND1(KM_KEYMAP_GLOBAL, KM_KEY_END, 0, "end-of-line"),
    KM_BIND1(KM_KEYMAP_GLOBAL, KM_KEY_DELETE, 0, "delete-char"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 0x7f, KM_MOD_ALT, "backward-kill-word"),
    KM_BIND1(KM_KEYMAP_GLOBAL, '{', KM_MOD_ALT, "backward-paragraph"),
    KM_BIND1(KM_KEYMAP_GLOBAL, '{', KM_MOD_ALT | KM_MOD_SHIFT,
             "backward-paragraph"),
    KM_BIND1(KM_KEYMAP_GLOBAL, '}', KM_MOD_ALT, "forward-paragraph"),
    KM_BIND1(KM_KEYMAP_GLOBAL, '}', KM_MOD_ALT | KM_MOD_SHIFT,
             "forward-paragraph"),
    KM_BIND1(KM_KEYMAP_GLOBAL, '\\', KM_MOD_ALT, "delete-horizontal-space"),
    KM_BIND1(KM_KEYMAP_GLOBAL, '<', KM_MOD_ALT, "beginning-of-buffer"),
    KM_BIND1(KM_KEYMAP_GLOBAL, '<', KM_MOD_ALT | KM_MOD_SHIFT,
             "beginning-of-buffer"),
    KM_BIND1(KM_KEYMAP_GLOBAL, '>', KM_MOD_ALT, "end-of-buffer"),
    KM_BIND1(KM_KEYMAP_GLOBAL, '>', KM_MOD_ALT | KM_MOD_SHIFT,
             "end-of-buffer"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'b', KM_MOD_ALT, "backward-word"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'a', KM_MOD_ALT, "backward-sentence"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'd', KM_MOD_ALT, "kill-word"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'e', KM_MOD_ALT, "forward-sentence"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'f', KM_MOD_ALT, "forward-word"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'm', KM_MOD_ALT, "back-to-indentation"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'v', KM_MOD_ALT, "scroll-down-command"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'w', KM_MOD_ALT, "copy-region-as-kill"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 't', KM_MOD_ALT, "transpose-words"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'y', KM_MOD_ALT, "yank-pop"),
    KM_BIND1(KM_KEYMAP_GLOBAL, 'x', KM_MOD_ALT, "execute-extended-command"),
    KM_BIND2(KM_KEYMAP_GLOBAL, 'x', KM_MOD_CTRL, 'b', 0,
             "switch-to-buffer"),
    KM_BIND2(KM_KEYMAP_GLOBAL, 'x', KM_MOD_CTRL, 'c', KM_MOD_CTRL,
             "exit-editor"),
    KM_BIND2(KM_KEYMAP_GLOBAL, 'x', KM_MOD_CTRL, 'f', KM_MOD_CTRL,
             "find-file"),
    KM_BIND2(KM_KEYMAP_GLOBAL, 'x', KM_MOD_CTRL, 'h', 0,
             "mark-whole-buffer"),
    KM_BIND2(KM_KEYMAP_GLOBAL, 'x', KM_MOD_CTRL, 'k', 0, "kill-buffer"),
    KM_BIND2(KM_KEYMAP_GLOBAL, 'x', KM_MOD_CTRL, 'r', KM_MOD_CTRL,
             "undo-redo"),
    KM_BIND2(KM_KEYMAP_GLOBAL, 'x', KM_MOD_CTRL, 's', KM_MOD_CTRL,
             "save-buffer"),
    KM_BIND2(KM_KEYMAP_GLOBAL, 'x', KM_MOD_CTRL, 'u', 0, "undo"),
    KM_BIND2(KM_KEYMAP_GLOBAL, 'x', KM_MOD_CTRL, 'x', KM_MOD_CTRL,
             "exchange-point-and-mark"),
    KM_BIND2(KM_KEYMAP_GLOBAL, 'g', KM_MOD_ALT, 'g', 0, "goto-line"),
    KM_BIND2(KM_KEYMAP_GLOBAL, 'g', KM_MOD_ALT, 'g', KM_MOD_ALT,
             "goto-line"),

    KM_BIND1(KM_KEYMAP_MINIBUFFER, 'g', KM_MOD_CTRL, "keyboard-quit"),
    KM_BIND1(KM_KEYMAP_MINIBUFFER, 'k', KM_MOD_CTRL, "minibuffer-clear"),
    KM_BIND1(KM_KEYMAP_MINIBUFFER, 'r', KM_MOD_CTRL,
             "minibuffer-previous-completion"),
    KM_BIND1(KM_KEYMAP_MINIBUFFER, 's', KM_MOD_CTRL,
             "minibuffer-next-completion"),
    KM_BIND1(KM_KEYMAP_MINIBUFFER, 'j', KM_MOD_ALT,
             "minibuffer-accept-original"),
    KM_BIND1(KM_KEYMAP_MINIBUFFER, 0x7f, 0, "minibuffer-backspace"),
    KM_BIND1(KM_KEYMAP_MINIBUFFER, KM_KEY_LEFT, 0,
             "minibuffer-previous-completion"),
    KM_BIND1(KM_KEYMAP_MINIBUFFER, KM_KEY_RIGHT, 0,
             "minibuffer-next-completion"),
    KM_BIND1(KM_KEYMAP_MINIBUFFER, KM_KEY_TAB, 0, "minibuffer-complete"),
    KM_BIND1(KM_KEYMAP_MINIBUFFER, '\n', 0, "minibuffer-accept"),

    KM_BIND1(KM_KEYMAP_ISEARCH, 'g', KM_MOD_CTRL, "keyboard-quit"),
    KM_BIND1(KM_KEYMAP_ISEARCH, 'r', KM_MOD_CTRL,
             "isearch-repeat-backward"),
    KM_BIND1(KM_KEYMAP_ISEARCH, 's', KM_MOD_CTRL,
             "isearch-repeat-forward"),
    KM_BIND1(KM_KEYMAP_ISEARCH, 0x7f, 0, "isearch-backspace"),
    KM_BIND1(KM_KEYMAP_ISEARCH, '\n', 0, "isearch-accept"),

    KM_BIND1(KM_KEYMAP_CONFIRMATION, 'g', KM_MOD_CTRL, "keyboard-quit"),
    KM_BIND1(KM_KEYMAP_CONFIRMATION, 'y', 0, "confirmation-yes"),
    KM_BIND1(KM_KEYMAP_CONFIRMATION, 'Y', 0, "confirmation-yes"),
    KM_BIND1(KM_KEYMAP_CONFIRMATION, 'n', 0, "confirmation-no"),
    KM_BIND1(KM_KEYMAP_CONFIRMATION, 'N', 0, "confirmation-no"),
};

#undef KM_BIND1
#undef KM_BIND2

static KmStatus dispatch_one(KmCommandLoop *loop, KmView *view,
                             const KmEvent *event, KmError *error)
{
    KmKeymapId context = active_keymap(loop);
    KmKeymap *keymap;
    uint32_t codepoint;
    uint32_t modifiers;
    size_t node;

    if (event->kind == KM_EVENT_RESIZE || event->kind == KM_EVENT_FOCUS ||
        event->kind == KM_EVENT_MOUSE || event->kind == KM_EVENT_EOF) {
        return KM_OK;
    }
    if (event->kind == KM_EVENT_PASTE) {
        if (context == KM_KEYMAP_MINIBUFFER) {
            return dispatch_minibuffer_text(loop, event, error);
        }
        if (context != KM_KEYMAP_GLOBAL) {
            return fail(error, KM_ERR_INVALID, "paste context");
        }
        KmStatus status;
        bool has_argument = loop->prefix_kind != KM_PREFIX_NONE;
        reset_command_input(loop);
        loop->yank_buffer = NULL;
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
        if (context == KM_KEYMAP_MINIBUFFER) {
            return dispatch_minibuffer_text(loop, event, error);
        }
        if (context == KM_KEYMAP_ISEARCH) {
            return dispatch_isearch_text(loop, view, event, error);
        }
        if (context != KM_KEYMAP_GLOBAL) {
            return fail(error, KM_ERR_INVALID, "text context");
        }
        int64_t argument = current_argument(loop);
        KmStatus status;
        reset_command_input(loop);
        loop->yank_buffer = NULL;
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
    if (loop->key_node == 0) {
        loop->keymap_id = context;
    } else {
        context = loop->keymap_id;
    }
    keymap = &loop->keymaps[context];
    if (context == KM_KEYMAP_GLOBAL && loop->key_node == 0) {
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

    node = find_key_child(keymap, loop->key_node, codepoint, modifiers);
    if (node == KM_NO_KEY_NODE) {
        if (event->kind == KM_EVENT_TEXT && loop->key_node == 0) {
            if (context == KM_KEYMAP_GLOBAL) {
                return dispatch_text(loop, view, codepoint, error);
            }
            if (context == KM_KEYMAP_MINIBUFFER) {
                return dispatch_minibuffer_text(loop, event, error);
            }
            if (context == KM_KEYMAP_ISEARCH) {
                return dispatch_isearch_text(loop, view, event, error);
            }
        }
        reset_command_input(loop);
        return fail(error, KM_ERR_INVALID, "undefined key");
    }
    if (keymap->nodes[node].child != KM_NO_KEY_NODE) {
        loop->key_node = node;
        return KM_OK;
    }
    if (keymap->nodes[node].command == KM_COMMAND_NONE) {
        reset_command_input(loop);
        return fail(error, KM_ERR_INVALID, "undefined key");
    }
    return execute_registered_command(loop, view,
                                      keymap->nodes[node].command,
                                      context, error);
}

KmStatus km_command_loop_create(KmCommandLoop **out_loop, KmError *error)
{
    KmCommandLoop *loop;
    size_t i;
    KmStatus status;

    km_error_clear(error);
    if (out_loop == NULL) {
        return fail(error, KM_ERR_INVALID, "command loop create");
    }
    *out_loop = NULL;
    loop = (KmCommandLoop *)calloc(1, sizeof(*loop));
    if (loop == NULL) {
        return fail(error, KM_ERR_OOM, "command loop create");
    }
    for (i = 0; i < KM_KEYMAP_COUNT; ++i) {
        status = reserve_keymap(&loop->keymaps[i], 1, error);
        if (status != KM_OK) goto fail;
        loop->keymaps[i].nodes[0] = (KmKeyNode){
            0, 0, KM_COMMAND_NONE, KM_NO_KEY_NODE, KM_NO_KEY_NODE,
        };
        loop->keymaps[i].count = 1;
    }
    for (i = 0; i < sizeof(default_bindings) / sizeof(default_bindings[0]); ++i) {
        const KmDefaultBinding *binding = &default_bindings[i];
        const KmCommandSpec *command = find_command_name(binding->command_name);
        status = bind_key(loop, binding->keymap, binding->sequence,
                          binding->count, command, error);
        if (status != KM_OK) goto fail;
    }
    *out_loop = loop;
    return KM_OK;

fail:
    km_command_loop_destroy(loop);
    return status;
}

void km_command_loop_destroy(KmCommandLoop *loop)
{
    if (loop != NULL) {
        size_t i;
        clear_completions(loop);
        free(loop->prompt_text);
        free(loop->search_query);
        for (i = 0; i < loop->kill_count; ++i) {
            free(loop->kill_ring[i].text);
        }
        for (i = 0; i < KM_KEYMAP_COUNT; ++i) {
            free(loop->keymaps[i].nodes);
        }
    }
    free(loop);
}

KmStatus km_command_loop_bind_key(KmCommandLoop *loop, KmKeymapId keymap,
                                  const KmKeyStroke *sequence, size_t count,
                                  const char *command_name, KmError *error)
{
    const KmCommandSpec *command;

    km_error_clear(error);
    if (loop == NULL) {
        return fail(error, KM_ERR_INVALID, "keymap binding");
    }
    if (loop->key_node != 0) {
        return fail(error, KM_ERR_CONFLICT, "keymap binding");
    }
    command = find_command_name(command_name);
    return bind_key(loop, keymap, sequence, count, command, error);
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
        KmPromptKind prompt_kind = loop->prompt_kind;
        bool search_active = loop->search_active;
        status = dispatch_one(loop, view, event, error);
        if (status != KM_OK) return status;
        if (loop->prompt_kind != prompt_kind ||
            loop->search_active != search_active) {
            break;
        }
        if (loop->request != KM_COMMAND_REQUEST_NONE &&
            !((loop->request == KM_COMMAND_REQUEST_COMPLETE_FILE ||
               loop->request == KM_COMMAND_REQUEST_COMPLETE_BUFFER) &&
              event->kind == KM_EVENT_TEXT && event->codepoint != '\n')) {
            break;
        }
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

KmCommandRequest km_command_loop_request(const KmCommandLoop *loop)
{
    return loop == NULL ? KM_COMMAND_REQUEST_NONE : loop->request;
}

const char *km_command_loop_request_text(const KmCommandLoop *loop)
{
    return loop == NULL || loop->prompt_text == NULL
               ? ""
               : (const char *)loop->prompt_text;
}

int64_t km_command_loop_request_argument(const KmCommandLoop *loop)
{
    return loop == NULL ? 1 : loop->request_argument;
}

bool km_command_loop_request_has_argument(const KmCommandLoop *loop)
{
    return loop != NULL && loop->request_has_argument;
}

bool km_command_loop_request_page_opposite(const KmCommandLoop *loop)
{
    return loop != NULL && loop->request_page_opposite;
}

void km_command_loop_clear_request(KmCommandLoop *loop)
{
    if (loop != NULL) {
        loop->request = KM_COMMAND_REQUEST_NONE;
        loop->request_argument = 0;
        loop->request_has_argument = false;
        loop->request_page_opposite = false;
    }
}

KmStatus km_command_loop_set_prompt_text(KmCommandLoop *loop,
                                         const char *text, KmError *error)
{
    KmStatus status;

    km_error_clear(error);
    if (loop == NULL || loop->prompt_kind == KM_PROMPT_NONE ||
        loop->prompt_kind == KM_PROMPT_CONFIRM_KILL ||
        loop->prompt_kind == KM_PROMPT_CONFIRM_EXIT) {
        return fail(error, KM_ERR_INVALID, "set minibuffer text");
    }
    status = replace_prompt_text(loop, text, error);
    return status == KM_OK ? refresh_prompt_completions(loop, error) : status;
}

KmStatus km_command_loop_set_completions(KmCommandLoop *loop,
                                         const char *const *items,
                                         size_t count, const char *common,
                                         KmError *error)
{
    km_error_clear(error);
    if (loop == NULL ||
        (loop->prompt_kind != KM_PROMPT_FIND_FILE &&
         loop->prompt_kind != KM_PROMPT_SWITCH_BUFFER)) {
        return fail(error, KM_ERR_INVALID, "set minibuffer completions");
    }
    return replace_completions(loop, items, count, common, error);
}

KmStatus km_command_loop_select_completion(KmCommandLoop *loop,
                                           const char *item, KmError *error)
{
    size_t i;

    km_error_clear(error);
    if (loop == NULL || item == NULL) {
        return fail(error, KM_ERR_INVALID, "select minibuffer completion");
    }
    for (i = 0; i < loop->completion_count; ++i) {
        if (strcmp(loop->completions[i], item) == 0) {
            loop->completion_index = i;
            return KM_OK;
        }
    }
    return fail(error, KM_ERR_INVALID, "select minibuffer completion");
}

KmStatus km_command_loop_confirm_exit(KmCommandLoop *loop, KmError *error)
{
    km_error_clear(error);
    if (loop == NULL) return fail(error, KM_ERR_INVALID, "confirm exit");
    return begin_prompt(loop, KM_PROMPT_CONFIRM_EXIT, error);
}

bool km_command_loop_search_active(const KmCommandLoop *loop)
{
    return loop != NULL && loop->search_active;
}

bool km_command_loop_prompt_active(const KmCommandLoop *loop)
{
    return loop != NULL && loop->prompt_kind != KM_PROMPT_NONE;
}

void km_command_loop_format_prompt(const KmCommandLoop *loop,
                                   char *destination, size_t capacity)
{
    const char *prefix;
    const uint8_t *text;
    size_t text_len;
    size_t used;
    size_t offset = 0;

    if (destination == NULL || capacity == 0) return;
    destination[0] = '\0';
    if (loop == NULL) return;
    if (loop->search_active) {
        if (loop->search_failed) {
            prefix = loop->search_forward ? "Failing I-search: "
                                          : "Failing I-search backward: ";
        } else if (loop->search_wrapped) {
            prefix = loop->search_forward ? "Wrapped I-search: "
                                          : "Wrapped I-search backward: ";
        } else {
            prefix = loop->search_forward ? "I-search: "
                                          : "I-search backward: ";
        }
        text = loop->search_query;
        text_len = loop->search_len;
    } else {
        text = loop->prompt_text;
        text_len = loop->prompt_len;
        switch (loop->prompt_kind) {
        case KM_PROMPT_FIND_FILE:
            prefix = "Find file: ";
            break;
        case KM_PROMPT_SWITCH_BUFFER:
            prefix = "Switch to buffer: ";
            break;
        case KM_PROMPT_COMMAND:
            prefix = "M-x ";
            break;
        case KM_PROMPT_GOTO_LINE:
            prefix = "Goto line: ";
            break;
        case KM_PROMPT_CONFIRM_KILL:
            prefix = "Buffer modified; kill anyway? (y or n) ";
            text_len = 0;
            break;
        case KM_PROMPT_CONFIRM_EXIT:
            prefix = "Modified buffers exist; exit anyway? (y or n) ";
            text_len = 0;
            break;
        case KM_PROMPT_NONE:
        default:
            return;
        }
    }
    used = strlen(prefix);
    if (used >= capacity) return;
    memcpy(destination, prefix, used);
    while (offset < text_len) {
        int32_t codepoint;
        size_t consumed;
        if (km_unicode_decode(text, text_len, offset,
                              &codepoint, &consumed, NULL) != KM_OK) {
            break;
        }
        if (codepoint < 0x20 || codepoint == 0x7f) {
            if (used + 1 >= capacity) break;
            destination[used++] = '?';
        } else {
            if (consumed > capacity - used - 1) break;
            memcpy(destination + used, text + offset, consumed);
            used += consumed;
        }
        offset += consumed;
    }
    destination[used] = '\0';
}

static void append_completion_display(char *destination, size_t capacity,
                                      size_t *used, const char *text)
{
    size_t len = strlen(text);
    if (*used >= capacity - 1) return;
    if (len > capacity - *used - 1) len = capacity - *used - 1;
    while (len != 0 &&
           ((unsigned char)text[len] & 0xc0u) == 0x80u) {
        --len;
    }
    memcpy(destination + *used, text, len);
    *used += len;
    destination[*used] = '\0';
}

void km_command_loop_format_completions(const KmCommandLoop *loop,
                                        char *destination, size_t capacity)
{
    size_t used = 0;
    size_t i;

    if (destination == NULL || capacity == 0) return;
    destination[0] = '\0';
    if (loop == NULL ||
        (loop->prompt_kind != KM_PROMPT_FIND_FILE &&
         loop->prompt_kind != KM_PROMPT_SWITCH_BUFFER &&
         loop->prompt_kind != KM_PROMPT_COMMAND)) {
        return;
    }
    if (loop->prompt_len == 0) return;
    if (loop->completion_count == 0) {
        append_completion_display(destination, capacity, &used,
                                  " [No matches]");
        return;
    }
    if (loop->completion_common != NULL &&
        strlen(loop->completion_common) > loop->prompt_len &&
        memcmp(loop->completion_common, loop->prompt_text,
               loop->prompt_len) == 0) {
        append_completion_display(destination, capacity, &used, " [");
        append_completion_display(destination, capacity, &used,
                                  loop->completion_common + loop->prompt_len);
        append_completion_display(destination, capacity, &used, "]");
    }
    if (loop->completion_count == 1) {
        append_completion_display(destination, capacity, &used, " [Matched]");
        return;
    }
    append_completion_display(destination, capacity, &used, " {");
    for (i = 0; i < loop->completion_count; ++i) {
        size_t index = (loop->completion_index + i) % loop->completion_count;
        if (i != 0) {
            append_completion_display(destination, capacity, &used, " | ");
        }
        append_completion_display(destination, capacity, &used,
                                  loop->completions[index]);
    }
    append_completion_display(destination, capacity, &used, "}");
}
