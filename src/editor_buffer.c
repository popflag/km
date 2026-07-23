#include "editor_internal.h"

#include "configuration.h"
#include "file.h"
#include "unicode.h"

#include "utf8proc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

KmStatus fail(KmError *error, KmStatus status, const char *operation)
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

void reset_preferred_columns(KmBuffer *buffer)
{
    KmView *view;

    for (view = buffer->views; view != NULL; view = view->next) {
        view->preferred_column_set = false;
    }
}

KmStatus validate_view(KmView *view, bool mutable, KmBytePos *point,
                       KmBytePos *start, KmBytePos *end, KmError *error)
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

KmStatus prepare_mark(KmBuffer *buffer, KmBytePos position,
                      KmMarkPlan *plan, KmError *error)
{
    *plan = (KmMarkPlan){NULL, position};
    if (!buffer->mark_set ||
        km_anchor_get(buffer->mark).v == position.v) {
        return KM_OK;
    }
    return km_anchor_create(buffer->document, km_anchor_get(buffer->mark),
                            KM_ANCHOR_BEFORE, &plan->saved_mark, error);
}

void discard_mark(KmMarkPlan *plan)
{
    km_anchor_destroy(plan->saved_mark);
    plan->saved_mark = NULL;
}

void commit_mark(KmBuffer *buffer, KmMarkPlan *plan, bool active)
{
    (void)km_anchor_set(buffer->mark, plan->position, NULL);
    if (plan->saved_mark != NULL) {
        if (buffer->mark_ring_count == km_config_mark_ring_capacity()) {
            km_anchor_destroy(
                buffer->mark_ring[km_config_mark_ring_capacity() - 1]);
        } else {
            ++buffer->mark_ring_count;
        }
        memmove(&buffer->mark_ring[1], &buffer->mark_ring[0],
                (buffer->mark_ring_count - 1) *
                    sizeof(buffer->mark_ring[0]));
        buffer->mark_ring[0] = plan->saved_mark;
        plan->saved_mark = NULL;
    }
    buffer->mark_set = true;
    buffer->mark_active = active;
    buffer->rectangle_mark_mode = false;
}

KmStatus apply_view_splice(KmView *view, KmBytePos start, KmBytePos end,
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

KmStatus apply_view_splice_and_set_point(
    KmView *view, KmBytePos start, KmBytePos end, const uint8_t *insert,
    size_t insert_len, uint64_t command_id, KmBytePos final_point,
    KmError *error)
{
    KmSplice splice = {start, end, insert, insert_len, 0};
    KmTxnMeta meta = {km_document_revision(view->buffer->document),
                      command_id};
    KmStatus status = km_document_apply_and_set_anchor(
        view->buffer->document, &splice, meta, view->point, final_point,
        error);

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

uint64_t command_magnitude(int64_t argument)
{
    return argument < 0 ? (uint64_t)(-argument) : (uint64_t)argument;
}

KmStatus move_chars(KmView *view, int64_t argument, KmError *error)
{
    KmBytePos point;
    KmBytePos original;
    KmBytePos start;
    KmBytePos end;
    KmBytePos next;
    uint64_t remaining = command_magnitude(argument);
    bool backward = argument < 0;
    KmStatus status = validate_view(view, false, &point, &start, &end, error);

    if (status != KM_OK) return status;
    original = point;
    while (remaining != 0) {
        status = adjacent_boundary(view->buffer->document, point, start, end,
                                   backward, &next);
        if (status != KM_OK) break;
        point = next;
        --remaining;
    }
    if (point.v != original.v) {
        KmStatus set_status = km_anchor_set(view->point, point, error);
        if (set_status != KM_OK) return set_status;
        view->preferred_column_set = false;
    }
    return status == KM_OK ? KM_OK
                           : fail(error, status, "move characters");
}

KmStatus copy_accessible_text(KmBuffer *buffer, uint8_t **out_text,
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

bool word_constituent(int32_t codepoint)
{
    utf8proc_category_t category = utf8proc_category(codepoint);

    /* ponytail: Replace this approximation when mode syntax tables exist. */
    return (category >= UTF8PROC_CATEGORY_LU &&
            category <= UTF8PROC_CATEGORY_ME) ||
           (category >= UTF8PROC_CATEGORY_ND &&
            category <= UTF8PROC_CATEGORY_NO) ||
           category == UTF8PROC_CATEGORY_SO;
}

size_t previous_codepoint_start(const uint8_t *text, size_t position)
{
    --position;
    while (position != 0 && (text[position] & 0xc0u) == 0x80u) --position;
    return position;
}

KmStatus scan_words(const uint8_t *text, size_t len, size_t position,
                           int64_t argument, bool clamp,
                           size_t *out_position,
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
                if (!clamp) {
                    return fail(error, KM_ERR_INVALID, "scan words");
                }
                position = 0;
                break;
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
                if (!clamp) {
                    return fail(error, KM_ERR_INVALID, "scan words");
                }
                position = len;
                break;
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

KmStatus move_words(KmView *view, int64_t argument, KmError *error)
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
    status = scan_words(text, len, position, argument, true, &position, error);
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
        if (spaces - after >= km_config_sentence_end_spaces()) {
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

KmStatus move_sentences(KmView *view, int64_t argument,
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
        if (status != KM_OK) break;
    }
    {
        KmStatus set_status = km_anchor_set(
            view->point, (KmBytePos){start.v + position}, error);
        if (set_status != KM_OK) {
            free(text);
            return set_status;
        }
        view->preferred_column_set = false;
    }
    if (status != KM_OK && backward) {
        km_error_clear(error);
        status = KM_OK;
    } else if (status != KM_OK) {
        status = fail(error, status, "scan sentences");
    }

    free(text);
    return status;
}

KmStatus move_to_line(KmView *view, int64_t line, KmError *error)
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

KmStatus move_to_char(KmView *view, int64_t character, KmError *error)
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
    if (character < 1) return fail(error, KM_ERR_INVALID, "character number");
    document = view->buffer->document;
    len = km_document_len(document);
    if (len != 0) {
        text = (uint8_t *)malloc(len);
        if (text == NULL) return fail(error, KM_ERR_OOM, "goto char");
        status = km_document_copy(document, (KmBytePos){0}, len, text, error);
        if (status != KM_OK) goto done;
    }
    while (current < character && position < len) {
        int32_t codepoint;
        size_t consumed;
        status = km_unicode_decode(text, len, position, &codepoint, &consumed,
                                   error);
        if (status != KM_OK) goto done;
        position += consumed;
        ++current;
    }
    if (current != character || position < start.v || position > end.v) {
        status = fail(error, KM_ERR_INVALID,
                      "character outside accessible range");
        goto done;
    }
    status = km_anchor_set(view->point, (KmBytePos){position}, error);
    if (status == KM_OK) view->preferred_column_set = false;

done:
    free(text);
    return status;
}

size_t line_start_at(const uint8_t *text, size_t point)
{
    while (point != 0 && text[point - 1] != '\n') --point;
    return point;
}

size_t line_end_at(const uint8_t *text, size_t len, size_t point)
{
    while (point < len && text[point] != '\n') ++point;
    return point;
}

bool blank_line_at(const uint8_t *text, size_t len, size_t start)
{
    size_t end = line_end_at(text, len, start);

    while (start < end) {
        uint8_t byte = text[start++];
        if (byte != ' ' && byte != '\t' && byte != '\f') return false;
    }
    return true;
}

size_t next_line_at(const uint8_t *text, size_t len, size_t start)
{
    size_t end = line_end_at(text, len, start);
    return end < len ? end + 1 : len;
}

size_t previous_line_at(const uint8_t *text, size_t start)
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

KmStatus move_paragraphs(KmView *view, int64_t argument,
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
        if (next == position) break;
        position = next;
    }
    status = km_anchor_set(view->point, (KmBytePos){start.v + position}, error);
    if (status == KM_OK) view->preferred_column_set = false;

    free(text);
    return status;
}

KmStatus move_vertical(KmView *view, int64_t argument, KmError *error)
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
        status = km_unicode_line_column_at(
            text, len, line_start_at(text, position), position, &preferred,
            error);
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
        {
            size_t actual_column;
            status = km_unicode_line_position_at_column(
                text, len, target_start, target_end, preferred, &position,
                &actual_column, error);
        }
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

KmStatus move_line_edge(KmView *view, bool to_end, KmError *error)
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

KmStatus move_back_to_indentation(KmView *view, KmError *error)
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

KmStatus delete_chars(KmView *view, int64_t argument,
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

KmStatus insert_utf8_repeated(KmView *view, const uint8_t *text,
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
    size_t i;

    clamp_anchor(buffer->saved_point, start, end);
    clamp_anchor(buffer->mark, start, end);
    for (i = 0; i < buffer->mark_ring_count; ++i) {
        clamp_anchor(buffer->mark_ring[i], start, end);
    }
    for (view = buffer->views; view != NULL; view = view->next) {
        clamp_anchor(view->point, start, end);
    }
}

static void destroy_anchors(KmBuffer *buffer)
{
    size_t i;

    for (i = 0; i < buffer->mark_ring_count; ++i) {
        km_anchor_destroy(buffer->mark_ring[i]);
    }
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
    if (status == KM_OK) {
        if (km_config_mark_ring_capacity() >
            SIZE_MAX / sizeof(*buffer->mark_ring)) {
            destroy_anchors(buffer);
            return fail(error, KM_ERR_OOM, "mark ring");
        }
        buffer->mark_ring = (KmAnchor **)calloc(
            km_config_mark_ring_capacity(), sizeof(*buffer->mark_ring));
        if (buffer->mark_ring == NULL) {
            destroy_anchors(buffer);
            status = fail(error, KM_ERR_OOM, "mark ring");
        }
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
    buffer->line_numbers_visible =
        km_config_global_display_line_numbers_mode();
    buffer->name = copy_name(km_config_scratch_buffer_name());
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
    free(buffer->mark_ring);
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
    KmFileSaveResult result = {0};
    KmStateId state;
    KmStatus status;

    km_error_clear(error);
    if (buffer == NULL || buffer->base != NULL || buffer->file == NULL) {
        return fail(error, KM_ERR_UNSUPPORTED, "save buffer");
    }
    state = km_document_history_state(buffer->document);
    status = km_file_save(buffer->file, buffer->document, &result, error);
    if (result.committed &&
        km_document_history_state(buffer->document) == state) {
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

bool km_buffer_rectangle_mark_mode(const KmBuffer *buffer)
{
    return buffer != NULL && buffer->rectangle_mark_mode;
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

KmStatus move_buffer_edge(KmView *view, bool to_end, KmError *error)
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
