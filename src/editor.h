#ifndef KM_EDITOR_H
#define KM_EDITOR_H

#include "document.h"
#include "event.h"

#include <stdbool.h>

typedef struct KmBuffer KmBuffer;
typedef struct KmView KmView;
typedef struct KmCommandLoop KmCommandLoop;

typedef enum {
    KM_COMMAND_NONE = 0,
    KM_COMMAND_INSERT_UTF8_BLOCK,
    KM_COMMAND_PASTE,
    KM_COMMAND_FORWARD_CHAR,
    KM_COMMAND_BACKWARD_CHAR,
    KM_COMMAND_DELETE_CHAR,
    KM_COMMAND_DELETE_BACKWARD_CHAR,
    KM_COMMAND_UNDO,
    KM_COMMAND_REDO
} KmCommandId;

KmStatus km_buffer_create_base(const uint8_t *text, size_t len,
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

KmStatus km_view_create(KmBuffer *buffer, KmView **out_view, KmError *error);
KmStatus km_view_destroy(KmView *view, KmError *error);
KmBytePos km_view_point(const KmView *view);
KmStatus km_view_set_point(KmView *view, KmBytePos point, KmError *error);

KmStatus km_view_insert_utf8_block(KmView *view, const uint8_t *text,
                                   size_t len, KmError *error);
KmStatus km_view_forward_char(KmView *view, KmError *error);
KmStatus km_view_backward_char(KmView *view, KmError *error);
KmStatus km_view_delete_char(KmView *view, KmError *error);
KmStatus km_view_delete_backward_char(KmView *view, KmError *error);
KmStatus km_view_undo(KmView *view, KmError *error);
KmStatus km_view_redo(KmView *view, KmError *error);

KmStatus km_command_loop_create(KmCommandLoop **out_loop, KmError *error);
void km_command_loop_destroy(KmCommandLoop *loop);
KmStatus km_command_loop_dispatch(KmCommandLoop *loop, KmView *view,
                                  const KmEvent *event, KmError *error);
KmCommandId km_command_loop_last_command(const KmCommandLoop *loop);
bool km_command_loop_quit_requested(const KmCommandLoop *loop);
void km_command_loop_clear_quit(KmCommandLoop *loop);
bool km_command_loop_exit_requested(const KmCommandLoop *loop);

#endif
