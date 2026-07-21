#ifndef KM_EDITOR_H
#define KM_EDITOR_H

#include "document.h"
#include "event.h"

#include <stdbool.h>

typedef struct KmBuffer KmBuffer;
typedef struct KmView KmView;
typedef struct KmCommandLoop KmCommandLoop;
typedef struct KmFile KmFile;

typedef enum {
    KM_COMMAND_REQUEST_NONE = 0,
    KM_COMMAND_REQUEST_SAVE,
    KM_COMMAND_REQUEST_SAVE_ALL_EXIT,
    KM_COMMAND_REQUEST_EXIT,
    KM_COMMAND_REQUEST_EXIT_CONFIRMED,
    KM_COMMAND_REQUEST_FIND_FILE,
    KM_COMMAND_REQUEST_SWITCH_BUFFER,
    KM_COMMAND_REQUEST_KILL_BUFFER
} KmCommandRequest;

typedef enum {
    KM_COMMAND_NONE = 0,
    KM_COMMAND_INSERT_UTF8_BLOCK,
    KM_COMMAND_PASTE,
    KM_COMMAND_FORWARD_CHAR,
    KM_COMMAND_BACKWARD_CHAR,
    KM_COMMAND_DELETE_CHAR,
    KM_COMMAND_DELETE_BACKWARD_CHAR,
    KM_COMMAND_BEGINNING_OF_LINE,
    KM_COMMAND_END_OF_LINE,
    KM_COMMAND_NEXT_LINE,
    KM_COMMAND_PREVIOUS_LINE,
    KM_COMMAND_SET_MARK,
    KM_COMMAND_EXCHANGE_POINT_AND_MARK,
    KM_COMMAND_KILL_REGION,
    KM_COMMAND_COPY_REGION,
    KM_COMMAND_KILL_LINE,
    KM_COMMAND_YANK,
    KM_COMMAND_SEARCH_FORWARD,
    KM_COMMAND_UNDO,
    KM_COMMAND_REDO
} KmCommandId;

KmStatus km_buffer_create_base(const uint8_t *text, size_t len,
                               KmBuffer **out_buffer, KmError *error);
KmStatus km_buffer_create_file(KmFile *file, const uint8_t *text, size_t len,
                               KmBuffer **out_buffer, KmError *error);
KmStatus km_buffer_create_indirect(KmBuffer *base, KmBuffer **out_buffer,
                                   KmError *error);
KmStatus km_buffer_destroy(KmBuffer *buffer, KmError *error);

const KmDocument *km_buffer_document(const KmBuffer *buffer);
KmBytePos km_buffer_accessible_start(const KmBuffer *buffer);
KmBytePos km_buffer_accessible_end(const KmBuffer *buffer);
KmStatus km_buffer_narrow(KmBuffer *buffer, KmBytePos start, KmBytePos end,
                          KmError *error);
KmStatus km_buffer_widen(KmBuffer *buffer, KmError *error);

bool km_buffer_is_read_only(const KmBuffer *buffer);
void km_buffer_set_read_only(KmBuffer *buffer, bool read_only);
bool km_buffer_is_modified(const KmBuffer *buffer);
bool km_buffer_is_visited(const KmBuffer *buffer);
KmStatus km_buffer_save(KmBuffer *buffer, KmError *error);
const char *km_buffer_name(const KmBuffer *buffer);
KmStatus km_buffer_set_name(KmBuffer *buffer, const char *name,
                            KmError *error);
bool km_buffer_visits_same_file(const KmBuffer *left, const KmBuffer *right);
bool km_buffer_mark_active(const KmBuffer *buffer);
KmBytePos km_buffer_mark(const KmBuffer *buffer);

KmStatus km_view_create(KmBuffer *buffer, KmView **out_view, KmError *error);
KmStatus km_view_destroy(KmView *view, KmError *error);
KmBuffer *km_view_buffer(const KmView *view);
KmStatus km_view_set_buffer(KmView *view, KmBuffer *buffer, KmError *error);
KmBytePos km_view_point(const KmView *view);
KmStatus km_view_set_point(KmView *view, KmBytePos point, KmError *error);

KmStatus km_view_insert_utf8_block(KmView *view, const uint8_t *text,
                                   size_t len, KmError *error);
KmStatus km_view_forward_char(KmView *view, KmError *error);
KmStatus km_view_backward_char(KmView *view, KmError *error);
KmStatus km_view_delete_char(KmView *view, KmError *error);
KmStatus km_view_delete_backward_char(KmView *view, KmError *error);
KmStatus km_view_beginning_of_line(KmView *view, KmError *error);
KmStatus km_view_end_of_line(KmView *view, KmError *error);
KmStatus km_view_next_line(KmView *view, KmError *error);
KmStatus km_view_previous_line(KmView *view, KmError *error);
KmStatus km_view_undo(KmView *view, KmError *error);
KmStatus km_view_redo(KmView *view, KmError *error);

KmStatus km_command_loop_create(KmCommandLoop **out_loop, KmError *error);
void km_command_loop_destroy(KmCommandLoop *loop);
KmStatus km_command_loop_dispatch(KmCommandLoop *loop, KmView *view,
                                  const KmEvent *event, KmError *error);
KmCommandId km_command_loop_last_command(const KmCommandLoop *loop);
bool km_command_loop_quit_requested(const KmCommandLoop *loop);
void km_command_loop_clear_quit(KmCommandLoop *loop);
KmCommandRequest km_command_loop_request(const KmCommandLoop *loop);
const char *km_command_loop_request_text(const KmCommandLoop *loop);
void km_command_loop_clear_request(KmCommandLoop *loop);
KmStatus km_command_loop_confirm_exit(KmCommandLoop *loop, KmError *error);
bool km_command_loop_search_active(const KmCommandLoop *loop);
bool km_command_loop_prompt_active(const KmCommandLoop *loop);
void km_command_loop_format_prompt(const KmCommandLoop *loop,
                                   char *destination, size_t capacity);

#endif
