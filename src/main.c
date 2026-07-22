#include "layout.h"
#include "file.h"
#include "platform.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    KmBuffer *buffer;
    size_t scroll_row;
} AppBuffer;

typedef struct {
    AppBuffer *items;
    size_t count;
    size_t capacity;
    size_t current;
} AppBuffers;

static KmStatus app_fail(KmError *error, KmStatus status,
                         const char *operation)
{
    if (error != NULL) {
        error->code = status;
        error->os_code = 0;
        error->operation = operation;
    }
    return status;
}

static KmStatus append_buffer(AppBuffers *buffers, KmBuffer *buffer,
                              KmError *error)
{
    AppBuffer *items;
    size_t capacity;

    if (buffers->count == buffers->capacity) {
        capacity = buffers->capacity == 0 ? 4 : buffers->capacity * 2;
        if (capacity < buffers->capacity ||
            capacity > SIZE_MAX / sizeof(*items)) {
            return app_fail(error, KM_ERR_OOM, "buffer list");
        }
        items = (AppBuffer *)realloc(buffers->items,
                                     capacity * sizeof(*items));
        if (items == NULL) return app_fail(error, KM_ERR_OOM, "buffer list");
        buffers->items = items;
        buffers->capacity = capacity;
    }
    buffers->items[buffers->count++] = (AppBuffer){buffer, 0};
    return KM_OK;
}

static size_t find_buffer(const AppBuffers *buffers, const char *name)
{
    size_t i;

    for (i = 0; i < buffers->count; ++i) {
        if (strcmp(km_buffer_name(buffers->items[i].buffer), name) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static KmStatus set_buffer_completions(const AppBuffers *buffers,
                                       KmCommandLoop *loop,
                                       const char *prefix, KmError *error)
{
    size_t prefix_len = strlen(prefix);
    const char **matches;
    size_t count = 0;
    size_t i;
    KmStatus status;

    if (buffers->count > SIZE_MAX / sizeof(*matches)) {
        return app_fail(error, KM_ERR_OOM, "complete buffer");
    }
    matches = buffers->count == 0
                  ? NULL
                  : (const char **)malloc(buffers->count * sizeof(*matches));
    if (buffers->count != 0 && matches == NULL) {
        return app_fail(error, KM_ERR_OOM, "complete buffer");
    }
    for (i = 0; i < buffers->count; ++i) {
        const char *candidate = km_buffer_name(buffers->items[i].buffer);
        size_t candidate_len = strlen(candidate);
        if (candidate_len < prefix_len ||
            (prefix_len != 0 &&
             memcmp(candidate, prefix, prefix_len) != 0)) {
            continue;
        }
        matches[count++] = candidate;
    }
    status = km_command_loop_set_completions(loop, matches, count, NULL, error);
    if (status == KM_OK && prefix_len == 0 && buffers->count != 0) {
        status = km_command_loop_select_completion(
            loop,
            km_buffer_name(
                buffers->items[(buffers->current + 1) % buffers->count].buffer),
            error);
    }
    free(matches);
    return status;
}

static size_t find_file_buffer(const AppBuffers *buffers,
                               const KmBuffer *buffer)
{
    size_t i;

    for (i = 0; i < buffers->count; ++i) {
        if (km_buffer_visits_same_file(buffers->items[i].buffer, buffer)) {
            return i;
        }
    }
    return SIZE_MAX;
}

static KmStatus uniquify_buffer_name(const AppBuffers *buffers,
                                     KmBuffer *buffer, KmError *error)
{
    const char *current = km_buffer_name(buffer);
    size_t len = strlen(current);
    char *base;
    char *candidate;
    size_t ordinal;
    KmStatus status = KM_OK;

    if (find_buffer(buffers, current) == SIZE_MAX) return KM_OK;
    if (len > SIZE_MAX - 32) {
        return app_fail(error, KM_ERR_OOM, "buffer name");
    }
    base = (char *)malloc(len + 1);
    candidate = (char *)malloc(len + 32);
    if (base == NULL || candidate == NULL) {
        free(candidate);
        free(base);
        return app_fail(error, KM_ERR_OOM, "buffer name");
    }
    memcpy(base, current, len + 1);
    for (ordinal = 2; ordinal != SIZE_MAX; ++ordinal) {
        (void)snprintf(candidate, len + 32, "%s<%llu>", base,
                       (unsigned long long)ordinal);
        if (find_buffer(buffers, candidate) == SIZE_MAX) {
            status = km_buffer_set_name(buffer, candidate, error);
            break;
        }
    }
    if (ordinal == SIZE_MAX) {
        status = app_fail(error, KM_ERR_OOM, "buffer name");
    }
    free(candidate);
    free(base);
    return status;
}

static bool any_modified_buffer(const AppBuffers *buffers)
{
    size_t i;

    for (i = 0; i < buffers->count; ++i) {
        if (km_buffer_is_modified(buffers->items[i].buffer)) return true;
    }
    return false;
}

static KmStatus save_modified_files(AppBuffers *buffers, KmError *error)
{
    size_t i;

    for (i = 0; i < buffers->count; ++i) {
        KmBuffer *buffer = buffers->items[i].buffer;
        if (km_buffer_is_modified(buffer) && km_buffer_is_visited(buffer)) {
            KmStatus status = km_buffer_save(buffer, error);
            if (status != KM_OK) return status;
        }
    }
    return KM_OK;
}

static KmStatus switch_buffer(AppBuffers *buffers, KmView *view, size_t index,
                              KmError *error)
{
    KmStatus status;

    if (index >= buffers->count) {
        return app_fail(error, KM_ERR_INVALID, "switch buffer");
    }
    status = km_view_set_buffer(view, buffers->items[index].buffer, error);
    if (status == KM_OK) buffers->current = index;
    return status;
}

static KmStatus visit_file(AppBuffers *buffers, KmView *view, const char *name,
                           KmError *error)
{
    KmPath *path = NULL;
    KmFile *file = NULL;
    KmBuffer *buffer = NULL;
    uint8_t *text = NULL;
    size_t len = 0;
    size_t existing;
    KmStatus status;

    status = km_path_from_utf8(name, &path, error);
    if (status != KM_OK) return status;
    status = km_file_load(path, &file, &text, &len, error);
    path = NULL;
    if (status != KM_OK) return status;
    status = km_buffer_create_file(file, text, len, &buffer, error);
    free(text);
    if (status != KM_OK) {
        km_file_destroy(file);
        return status;
    }
    existing = find_file_buffer(buffers, buffer);
    if (existing != SIZE_MAX) {
        (void)km_buffer_destroy(buffer, NULL);
        return switch_buffer(buffers, view, existing, error);
    }
    status = uniquify_buffer_name(buffers, buffer, error);
    if (status != KM_OK) {
        (void)km_buffer_destroy(buffer, NULL);
        return status;
    }
    status = append_buffer(buffers, buffer, error);
    if (status != KM_OK) {
        (void)km_buffer_destroy(buffer, NULL);
        return status;
    }
    status = switch_buffer(buffers, view, buffers->count - 1, error);
    if (status != KM_OK) {
        --buffers->count;
        (void)km_buffer_destroy(buffer, NULL);
    }
    return status;
}

static KmStatus kill_current_buffer(AppBuffers *buffers, KmView *view,
                                    KmError *error)
{
    size_t victim = buffers->current;
    size_t target;
    KmBuffer *replacement = NULL;
    bool replacement_added = false;
    KmStatus status;

    if (buffers->count == 1) {
        status = km_buffer_create_base(NULL, 0, &replacement, error);
        if (status != KM_OK) return status;
        status = append_buffer(buffers, replacement, error);
        if (status != KM_OK) {
            (void)km_buffer_destroy(replacement, NULL);
            return status;
        }
        replacement_added = true;
    }
    target = (victim + 1) % buffers->count;
    status = switch_buffer(buffers, view, target, error);
    if (status != KM_OK) {
        if (replacement_added) {
            --buffers->count;
            (void)km_buffer_destroy(replacement, NULL);
        }
        return status;
    }
    status = km_buffer_destroy(buffers->items[victim].buffer, error);
    if (status != KM_OK) return status;
    memmove(&buffers->items[victim], &buffers->items[victim + 1],
            (buffers->count - victim - 1) * sizeof(*buffers->items));
    --buffers->count;
    buffers->current = target > victim ? target - 1 : target;
    return KM_OK;
}

static void destroy_buffers(AppBuffers *buffers)
{
    size_t i;

    for (i = 0; i < buffers->count; ++i) {
        (void)km_buffer_destroy(buffers->items[i].buffer, NULL);
    }
    free(buffers->items);
}

static void set_core_error(char *destination, size_t capacity,
                           const KmError *error)
{
    const char *operation = error->operation == NULL ? "editor" : error->operation;
    (void)snprintf(destination, capacity, "%s: %s", operation,
                   km_status_string(error->code));
}

static int redraw(KmPlatform *platform, const KmBuffer *buffer,
                  const KmView *view, size_t *scroll_row, const char *message,
                  const char *completion, bool prompt_active,
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
                       completion, prompt_active,
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
    char completion[4096] = {0};
    KmPlatform *platform = NULL;
    KmBuffer *initial_buffer = NULL;
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    AppBuffers buffers = {0};
    KmPath *path = NULL;
    KmFile *file = NULL;
    uint8_t *initial_text = NULL;
    size_t initial_len = 0;
    KmEvent event;
    KmError core_error;
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
             ? km_buffer_create_file(file, initial_text, initial_len,
                                     &initial_buffer, &core_error)
             : km_buffer_create_base(NULL, 0, &initial_buffer,
                                     &core_error)) != KM_OK) {
        free(initial_text);
        km_file_destroy(file);
        file = NULL;
        set_core_error(platform_error, sizeof(platform_error), &core_error);
        goto done;
    }
    file = NULL;
    free(initial_text);
    initial_text = NULL;
    if (append_buffer(&buffers, initial_buffer, &core_error) != KM_OK) {
        set_core_error(platform_error, sizeof(platform_error), &core_error);
        goto done;
    }
    initial_buffer = NULL;
    if (km_view_create(buffers.items[0].buffer, &view, &core_error) != KM_OK ||
        km_command_loop_create(&loop, &core_error) != KM_OK) {
        set_core_error(platform_error, sizeof(platform_error), &core_error);
        goto done;
    }
    if (redraw(platform, buffers.items[0].buffer, view,
               &buffers.items[0].scroll_row, message, completion, false,
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
        switch (km_command_loop_request(loop)) {
        case KM_COMMAND_REQUEST_SAVE:
            km_command_loop_clear_request(loop);
            status = km_buffer_save(buffers.items[buffers.current].buffer,
                                    &core_error);
            if (status == KM_OK) {
                (void)snprintf(message, sizeof(message), "Wrote");
            } else {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        case KM_COMMAND_REQUEST_SAVE_ALL_EXIT:
            km_command_loop_clear_request(loop);
            status = save_modified_files(&buffers, &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
                break;
            }
            if (!any_modified_buffer(&buffers)) {
                result = 0;
                break;
            }
            status = km_command_loop_confirm_exit(loop, &core_error);
            if (status != KM_OK) {
                set_core_error(platform_error, sizeof(platform_error),
                               &core_error);
                goto done;
            }
            break;
        case KM_COMMAND_REQUEST_FIND_FILE: {
            const char *name = km_command_loop_request_text(loop);
            km_command_loop_clear_request(loop);
            status = visit_file(&buffers, view, name[0] == '\0' ? "." : name,
                                &core_error);
            if (status == KM_OK) {
                (void)snprintf(message, sizeof(message), "Opened");
            } else {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        }
        case KM_COMMAND_REQUEST_SWITCH_BUFFER: {
            const char *name = km_command_loop_request_text(loop);
            size_t target = name[0] == '\0'
                                ? (buffers.current + 1) % buffers.count
                                : find_buffer(&buffers, name);
            km_command_loop_clear_request(loop);
            status = switch_buffer(&buffers, view, target, &core_error);
            if (status == KM_OK) {
                (void)snprintf(message, sizeof(message), "Switched");
            } else {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        }
        case KM_COMMAND_REQUEST_KILL_BUFFER: {
            km_command_loop_clear_request(loop);
            status = kill_current_buffer(&buffers, view, &core_error);
            if (status == KM_OK) {
                (void)snprintf(message, sizeof(message), "Killed");
            } else {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        }
        case KM_COMMAND_REQUEST_COMPLETE_FILE: {
            const char *prefix = km_command_loop_request_text(loop);
            KmPathCompletions candidates = {0};
            km_command_loop_clear_request(loop);
            status = km_path_completions_utf8(prefix, &candidates,
                                              &core_error);
            if (status == KM_OK) {
                status = km_command_loop_set_completions(
                    loop, (const char *const *)candidates.items,
                    candidates.count, candidates.common, &core_error);
            }
            km_path_completions_destroy(&candidates);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        }
        case KM_COMMAND_REQUEST_COMPLETE_BUFFER: {
            const char *prefix = km_command_loop_request_text(loop);
            km_command_loop_clear_request(loop);
            status = set_buffer_completions(&buffers, loop, prefix,
                                            &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        }
        case KM_COMMAND_REQUEST_SCROLL_UP:
        case KM_COMMAND_REQUEST_SCROLL_DOWN: {
            KmCommandRequest request = km_command_loop_request(loop);
            int64_t argument = km_command_loop_request_argument(loop);
            bool has_argument =
                km_command_loop_request_has_argument(loop);
            bool forward = request == KM_COMMAND_REQUEST_SCROLL_UP;
            size_t *scroll_row =
                &buffers.items[buffers.current].scroll_row;
            unsigned columns;
            unsigned rows;
            if (km_command_loop_request_page_opposite(loop)) {
                forward = !forward;
                has_argument = false;
            }
            km_command_loop_clear_request(loop);
            km_platform_size(platform, &columns, &rows);
            status = km_layout_scroll_view(
                buffers.items[buffers.current].buffer, view, rows, columns,
                *scroll_row, forward, has_argument, argument, scroll_row,
                &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        }
        case KM_COMMAND_REQUEST_RECENTER: {
            size_t *scroll_row =
                &buffers.items[buffers.current].scroll_row;
            KmCellGrid *scratch = NULL;
            KmLayoutResult layout;
            int64_t argument = km_command_loop_request_argument(loop);
            bool has_argument =
                km_command_loop_request_has_argument(loop);
            unsigned columns;
            unsigned rows;

            km_command_loop_clear_request(loop);
            km_platform_size(platform, &columns, &rows);
            status = km_layout_view(
                buffers.items[buffers.current].buffer, view, rows, columns,
                *scroll_row, NULL, NULL, false, &scratch, &layout,
                &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
                break;
            }
            km_cell_grid_destroy(scratch);
            status = km_layout_recenter_scroll(
                &layout, rows, has_argument, argument, scroll_row,
                &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        }
        case KM_COMMAND_REQUEST_EXIT:
            km_command_loop_clear_request(loop);
            if (!any_modified_buffer(&buffers)) {
                result = 0;
                break;
            }
            status = km_command_loop_confirm_exit(loop, &core_error);
            if (status != KM_OK) {
                set_core_error(platform_error, sizeof(platform_error),
                               &core_error);
                goto done;
            }
            break;
        case KM_COMMAND_REQUEST_EXIT_CONFIRMED:
            km_command_loop_clear_request(loop);
            result = 0;
            break;
        case KM_COMMAND_REQUEST_NONE:
        default:
            break;
        }
        if (result == 0) break;
        if (km_command_loop_search_active(loop) ||
            km_command_loop_prompt_active(loop)) {
            km_command_loop_format_prompt(loop, message, sizeof(message));
            km_command_loop_format_completions(loop, completion,
                                               sizeof(completion));
        } else {
            completion[0] = '\0';
        }
        if (redraw(platform, buffers.items[buffers.current].buffer, view,
                   &buffers.items[buffers.current].scroll_row, message,
                   completion,
                   km_command_loop_search_active(loop) ||
                       km_command_loop_prompt_active(loop),
                   platform_error, sizeof(platform_error)) != 0) {
            goto done;
        }
    }

done:
    km_command_loop_destroy(loop);
    if (view != NULL) (void)km_view_destroy(view, NULL);
    destroy_buffers(&buffers);
    if (initial_buffer != NULL) (void)km_buffer_destroy(initial_buffer, NULL);
    free(initial_text);
    km_file_destroy(file);
    km_path_destroy(path);
    km_platform_close(platform);
    if (result != 0) fprintf(stderr, "km: %s\n", platform_error);
    return result;
}
