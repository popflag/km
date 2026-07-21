#include "layout.h"
#include "file.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_core_error(char *destination, size_t capacity,
                           const KmError *error)
{
    const char *operation = error->operation == NULL ? "editor" : error->operation;
    (void)snprintf(destination, capacity, "%s: %s", operation,
                   km_status_string(error->code));
}

static int redraw(KmPlatform *platform, const KmBuffer *buffer,
                  const KmView *view, size_t *scroll_row, const char *message,
                  char *error, size_t error_cap)
{
    KmCellGrid *grid = NULL;
    KmLayoutResult layout;
    KmError core_error;
    unsigned columns;
    unsigned rows;
    int result;

    km_platform_size(platform, &columns, &rows);
    if (km_layout_view(buffer, view, rows, columns, *scroll_row, message,
                       &grid, &layout, &core_error) != KM_OK) {
        set_core_error(error, error_cap, &core_error);
        return -1;
    }
    result = km_platform_draw_grid(platform, grid, layout.cursor_row,
                                   layout.cursor_column, error, error_cap);
    if (result == 0) *scroll_row = layout.scroll_row;
    km_cell_grid_destroy(grid);
    return result;
}

int main(int argc, char **argv)
{
    char platform_error[256] = {0};
    char message[256] = {0};
    KmPlatform *platform = NULL;
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmPath *path = NULL;
    KmFile *file = NULL;
    uint8_t *initial_text = NULL;
    size_t initial_len = 0;
    KmEvent event;
    KmError core_error;
    size_t scroll_row = 0;
    bool discard_armed = false;
    int result = 1;

    if (km_path_from_command_line(argc, argv, &path, &core_error) != KM_OK) {
        set_core_error(platform_error, sizeof(platform_error), &core_error);
        fprintf(stderr, "usage: %s [file]\nkm: %s\n", argv[0], platform_error);
        return 1;
    }
    if (path != NULL &&
        km_file_load(path, &file, &initial_text, &initial_len,
                     &core_error) != KM_OK) {
        path = NULL;
        set_core_error(platform_error, sizeof(platform_error), &core_error);
        fprintf(stderr, "km: %s\n", platform_error);
        return 1;
    }
    path = NULL;
    if (km_platform_open(&platform, platform_error, sizeof(platform_error)) != 0) {
        free(initial_text);
        km_file_destroy(file);
        fprintf(stderr, "km: %s\n", platform_error);
        return 1;
    }
    if (!km_platform_is_interactive(platform)) {
        (void)snprintf(platform_error, sizeof(platform_error),
                       "interactive terminal required");
        goto done;
    }
    if ((file != NULL
             ? km_buffer_create_file(file, initial_text, initial_len, &buffer,
                                     &core_error)
             : km_buffer_create_base(NULL, 0, &buffer, &core_error)) != KM_OK) {
        free(initial_text);
        km_file_destroy(file);
        file = NULL;
        set_core_error(platform_error, sizeof(platform_error), &core_error);
        goto done;
    }
    file = NULL;
    free(initial_text);
    initial_text = NULL;
    if (km_view_create(buffer, &view, &core_error) != KM_OK ||
        km_command_loop_create(&loop, &core_error) != KM_OK) {
        set_core_error(platform_error, sizeof(platform_error), &core_error);
        goto done;
    }
    if (redraw(platform, buffer, view, &scroll_row, message,
               platform_error, sizeof(platform_error)) != 0) {
        goto done;
    }

    for (;;) {
        KmStatus status;

        if (km_platform_read_event(platform, &event, platform_error,
                                   sizeof(platform_error)) != 0) {
            goto done;
        }
        if (event.kind == KM_EVENT_EOF) {
            result = 0;
            break;
        }
        if (discard_armed &&
            !(event.kind == KM_EVENT_KEY && event.modifiers == KM_MOD_CTRL &&
              (event.codepoint == 'x' || event.codepoint == 'c'))) {
            discard_armed = false;
        }
        status = km_command_loop_dispatch(loop, view, &event, &core_error);
        if (status == KM_OK) {
            message[0] = '\0';
        } else if (status == KM_ERR_INVALID || status == KM_ERR_PERMISSION ||
                   status == KM_ERR_CANCELLED) {
            set_core_error(message, sizeof(message), &core_error);
        } else {
            set_core_error(platform_error, sizeof(platform_error), &core_error);
            goto done;
        }
        if (km_command_loop_quit_requested(loop)) {
            (void)snprintf(message, sizeof(message), "Quit");
            km_command_loop_clear_quit(loop);
        }
        if (km_command_loop_save_requested(loop)) {
            km_command_loop_clear_save(loop);
            status = km_buffer_save(buffer, &core_error);
            if (status == KM_OK) {
                (void)snprintf(message, sizeof(message), "Wrote");
                discard_armed = false;
            } else {
                set_core_error(message, sizeof(message), &core_error);
            }
        }
        if (km_command_loop_exit_requested(loop)) {
            if (!km_buffer_is_modified(buffer) || discard_armed) {
                result = 0;
                break;
            }
            discard_armed = true;
            km_command_loop_clear_exit(loop);
            (void)snprintf(message, sizeof(message),
                           "Modified; C-x C-c again to discard");
        }
        if (km_command_loop_search_active(loop)) {
            km_command_loop_format_prompt(loop, message, sizeof(message));
        }
        if (redraw(platform, buffer, view, &scroll_row, message,
                   platform_error, sizeof(platform_error)) != 0) {
            goto done;
        }
    }

done:
    km_command_loop_destroy(loop);
    if (view != NULL) (void)km_view_destroy(view, NULL);
    if (buffer != NULL) (void)km_buffer_destroy(buffer, NULL);
    free(initial_text);
    km_file_destroy(file);
    km_path_destroy(path);
    km_platform_close(platform);
    if (result != 0) fprintf(stderr, "km: %s\n", platform_error);
    return result;
}
