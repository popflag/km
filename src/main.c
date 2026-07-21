#include "layout.h"
#include "platform.h"

#include <stdio.h>
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

int main(void)
{
    char platform_error[256] = {0};
    char message[256] = {0};
    KmPlatform *platform = NULL;
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmEvent event;
    KmError core_error;
    size_t scroll_row = 0;
    int result = 1;

    if (km_platform_open(&platform, platform_error, sizeof(platform_error)) != 0) {
        fprintf(stderr, "km: %s\n", platform_error);
        return 1;
    }
    if (!km_platform_is_interactive(platform)) {
        (void)snprintf(platform_error, sizeof(platform_error),
                       "interactive terminal required");
        goto done;
    }
    if (km_buffer_create_base(NULL, 0, &buffer, &core_error) != KM_OK ||
        km_view_create(buffer, &view, &core_error) != KM_OK ||
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
        if (km_command_loop_exit_requested(loop)) {
            result = 0;
            break;
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
    km_platform_close(platform);
    if (result != 0) fprintf(stderr, "km: %s\n", platform_error);
    return result;
}
