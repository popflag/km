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
    KM_KEYMAP_GLOBAL = 0,
    KM_KEYMAP_MINIBUFFER,
    KM_KEYMAP_ISEARCH,
    KM_KEYMAP_CONFIRMATION,
    KM_KEYMAP_COUNT
} KmKeymapId;

typedef struct {
    uint32_t codepoint;
    uint32_t modifiers;
} KmKeyStroke;

typedef enum {
    KM_COMMAND_REQUEST_NONE = 0,
    KM_COMMAND_REQUEST_SAVE,
    KM_COMMAND_REQUEST_SAVE_ALL_EXIT,
    KM_COMMAND_REQUEST_EXIT,
    KM_COMMAND_REQUEST_EXIT_CONFIRMED,
    KM_COMMAND_REQUEST_FIND_FILE,
    KM_COMMAND_REQUEST_SWITCH_BUFFER,
    KM_COMMAND_REQUEST_KILL_BUFFER,
    KM_COMMAND_REQUEST_COMPLETE_FILE,
    KM_COMMAND_REQUEST_COMPLETE_BUFFER,
    KM_COMMAND_REQUEST_SCROLL_UP,
    KM_COMMAND_REQUEST_SCROLL_DOWN,
    KM_COMMAND_REQUEST_RECENTER,
    KM_COMMAND_REQUEST_MOVE_TO_WINDOW_LINE,
    KM_COMMAND_REQUEST_GLOBAL_DISPLAY_LINE_NUMBERS_MODE,
    KM_COMMAND_REQUEST_SPLIT_WINDOW_BELOW,
    KM_COMMAND_REQUEST_DELETE_WINDOW,
    KM_COMMAND_REQUEST_DELETE_OTHER_WINDOWS,
    KM_COMMAND_REQUEST_OTHER_WINDOW,
    KM_COMMAND_REQUEST_LIST_BUFFERS
} KmCommandRequest;

typedef enum {
#define KM_COMMAND_ID(id, value) id = value,
#define KM_PUBLIC_COMMAND(id, value, name, callback, contexts, flags) id = value,
#define KM_INTERNAL_COMMAND(id, value, name, callback, contexts, flags)
#include "commands.def"
#undef KM_INTERNAL_COMMAND
#undef KM_PUBLIC_COMMAND
#undef KM_COMMAND_ID
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
bool km_buffer_rectangle_mark_mode(const KmBuffer *buffer);
bool km_buffer_line_numbers_visible(const KmBuffer *buffer);
void km_buffer_set_line_numbers_visible(KmBuffer *buffer, bool visible);

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
KmStatus km_view_forward_word(KmView *view, KmError *error);
KmStatus km_view_backward_word(KmView *view, KmError *error);
KmStatus km_view_delete_char(KmView *view, KmError *error);
KmStatus km_view_delete_backward_char(KmView *view, KmError *error);
KmStatus km_view_beginning_of_line(KmView *view, KmError *error);
KmStatus km_view_end_of_line(KmView *view, KmError *error);
KmStatus km_view_beginning_of_buffer(KmView *view, KmError *error);
KmStatus km_view_end_of_buffer(KmView *view, KmError *error);
KmStatus km_view_forward_paragraph(KmView *view, KmError *error);
KmStatus km_view_backward_paragraph(KmView *view, KmError *error);
KmStatus km_view_forward_sentence(KmView *view, KmError *error);
KmStatus km_view_backward_sentence(KmView *view, KmError *error);
KmStatus km_view_next_line(KmView *view, KmError *error);
KmStatus km_view_previous_line(KmView *view, KmError *error);
KmStatus km_view_undo(KmView *view, KmError *error);
KmStatus km_view_redo(KmView *view, KmError *error);

KmStatus km_command_loop_create(KmCommandLoop **out_loop, KmError *error);
void km_command_loop_destroy(KmCommandLoop *loop);
KmStatus km_command_loop_dispatch(KmCommandLoop *loop, KmView *view,
                                  const KmEvent *event, KmError *error);
/* Rebinding is rejected when the command is not valid in the target keymap. */
KmStatus km_command_loop_bind_key(KmCommandLoop *loop, KmKeymapId keymap,
                                  const KmKeyStroke *sequence, size_t count,
                                  const char *command_name, KmError *error);
KmCommandId km_command_loop_last_command(const KmCommandLoop *loop);
bool km_command_loop_quit_requested(const KmCommandLoop *loop);
void km_command_loop_clear_quit(KmCommandLoop *loop);
KmCommandRequest km_command_loop_request(const KmCommandLoop *loop);
const char *km_command_loop_request_text(const KmCommandLoop *loop);
int64_t km_command_loop_request_argument(const KmCommandLoop *loop);
bool km_command_loop_request_has_argument(const KmCommandLoop *loop);
bool km_command_loop_request_page_opposite(const KmCommandLoop *loop);
void km_command_loop_clear_request(KmCommandLoop *loop);
KmStatus km_command_loop_set_prompt_text(KmCommandLoop *loop,
                                         const char *text, KmError *error);
KmStatus km_command_loop_set_completions(KmCommandLoop *loop,
                                         const char *const *items,
                                         size_t count, const char *common,
                                         KmError *error);
KmStatus km_command_loop_select_completion(KmCommandLoop *loop,
                                           const char *item, KmError *error);
KmStatus km_command_loop_confirm_exit(KmCommandLoop *loop, KmError *error);
bool km_command_loop_search_active(const KmCommandLoop *loop);
bool km_command_loop_prompt_active(const KmCommandLoop *loop);
void km_command_loop_format_prompt(const KmCommandLoop *loop,
                                   char *destination, size_t capacity);
void km_command_loop_format_completions(const KmCommandLoop *loop,
                                        char *destination, size_t capacity);

#endif
