#ifndef KM_EDITOR_INTERNAL_H
#define KM_EDITOR_INTERNAL_H

#include "editor.h"

struct KmView {
    KmBuffer *buffer;
    KmAnchor *point;
    KmView *next;
    KmView **prev_next;
    size_t preferred_column;
    bool preferred_column_set;
};

#define KM_MARK_RING_MAX 16

struct KmBuffer {
    KmDocument *document;
    KmBuffer *base;
    KmAnchor *begv;
    KmAnchor *zv;
    KmAnchor *mark;
    KmAnchor *mark_ring[KM_MARK_RING_MAX];
    size_t mark_ring_count;
    KmAnchor *saved_point;
    KmView *views;
    size_t view_count;
    size_t indirect_count;
    KmStateId saved_state;
    KmFile *file;
    char *name;
    bool read_only;
    bool mark_set;
    bool mark_active;
    bool line_numbers_visible;
};

typedef struct {
    KmAnchor *saved_mark;
    KmBytePos position;
} KmMarkPlan;

KmStatus fail(KmError *error, KmStatus status, const char *operation);
void reset_preferred_columns(KmBuffer *buffer);
KmStatus validate_view(KmView *view, bool mutable, KmBytePos *point,
                       KmBytePos *start, KmBytePos *end, KmError *error);
KmStatus prepare_mark(KmBuffer *buffer, KmBytePos position, KmMarkPlan *plan,
                      KmError *error);
void discard_mark(KmMarkPlan *plan);
void commit_mark(KmBuffer *buffer, KmMarkPlan *plan, bool active);
KmStatus apply_view_splice(KmView *view, KmBytePos start, KmBytePos end,
                           const uint8_t *insert, size_t insert_len,
                           uint64_t command_id, KmError *error);
KmStatus apply_view_splice_and_set_point(
    KmView *view, KmBytePos start, KmBytePos end, const uint8_t *insert,
    size_t insert_len, uint64_t command_id, KmBytePos final_point,
    KmError *error);
uint64_t command_magnitude(int64_t argument);
KmStatus move_chars(KmView *view, int64_t argument, KmError *error);
KmStatus copy_accessible_text(KmBuffer *buffer, uint8_t **out_text,
                              size_t *out_len, size_t *out_point,
                              KmView *view, KmError *error);
bool word_constituent(int32_t codepoint);
size_t previous_codepoint_start(const uint8_t *text, size_t position);
KmStatus scan_words(const uint8_t *text, size_t len, size_t position,
                    int64_t argument, bool clamp, size_t *out_position,
                    KmError *error);
KmStatus move_words(KmView *view, int64_t argument, KmError *error);
KmStatus move_sentences(KmView *view, int64_t argument, KmError *error);
KmStatus move_to_line(KmView *view, int64_t line, KmError *error);
KmStatus move_to_char(KmView *view, int64_t character, KmError *error);
size_t line_start_at(const uint8_t *text, size_t point);
size_t line_end_at(const uint8_t *text, size_t len, size_t point);
bool blank_line_at(const uint8_t *text, size_t len, size_t start);
size_t next_line_at(const uint8_t *text, size_t len, size_t start);
size_t previous_line_at(const uint8_t *text, size_t start);
KmStatus move_paragraphs(KmView *view, int64_t argument, KmError *error);
KmStatus move_vertical(KmView *view, int64_t argument, KmError *error);
KmStatus move_line_edge(KmView *view, bool to_end, KmError *error);
KmStatus move_back_to_indentation(KmView *view, KmError *error);
KmStatus delete_chars(KmView *view, int64_t argument, uint64_t command_id,
                      KmError *error);
KmStatus insert_utf8_repeated(KmView *view, const uint8_t *text, size_t len,
                              int64_t argument, uint64_t command_id,
                              KmError *error);
KmStatus move_buffer_edge(KmView *view, bool to_end, KmError *error);

#endif
