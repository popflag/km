#include "layout.h"
#include "configuration.h"
#include "file.h"
#include "platform.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    KmBuffer *buffer;
} AppBuffer;

typedef struct {
    AppBuffer *items;
    size_t count;
    size_t capacity;
    size_t current;
} AppBuffers;

typedef struct {
    KmView *view;
    size_t buffer;
    size_t scroll_row;
    size_t rows;
} AppWindow;

typedef struct {
    AppWindow *items;
    size_t count;
    size_t capacity;
    size_t current;
} AppWindows;

typedef struct {
    char error[256];
    char message[256];
    char completion[4096];
    KmPlatform *platform;
    KmCellGrid *front_grid;
    KmCommandLoop *loop;
    AppBuffers buffers;
    AppWindows windows;
} App;

typedef enum {
    APP_STEP_ERROR = -1,
    APP_STEP_EXIT = 0,
    APP_STEP_CONTINUE = 1
} AppStep;

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
    buffers->items[buffers->count++] = (AppBuffer){buffer};
    return KM_OK;
}

static KmStatus reserve_windows(AppWindows *windows, size_t count,
                                KmError *error)
{
    AppWindow *items;
    size_t capacity;

    if (count <= windows->capacity) return KM_OK;
    capacity = windows->capacity == 0 ? 4 : windows->capacity;
    while (capacity < count) {
        if (capacity > SIZE_MAX / 2) {
            capacity = count;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*items)) {
        return app_fail(error, KM_ERR_OOM, "window list");
    }
    items = (AppWindow *)realloc(windows->items, capacity * sizeof(*items));
    if (items == NULL) return app_fail(error, KM_ERR_OOM, "window list");
    windows->items = items;
    windows->capacity = capacity;
    return KM_OK;
}

static KmStatus create_initial_window(AppWindows *windows, KmBuffer *buffer,
                                      KmError *error)
{
    KmView *view = NULL;
    KmStatus status = reserve_windows(windows, 1, error);

    if (status != KM_OK) return status;
    status = km_view_create(buffer, &view, error);
    if (status != KM_OK) return status;
    windows->items[0] = (AppWindow){view, 0, 0, 0};
    windows->count = 1;
    return KM_OK;
}

static bool can_split_window(const AppWindows *windows, size_t terminal_rows)
{
    size_t available_rows = terminal_rows > 1 ? terminal_rows - 1 : terminal_rows;
    size_t minimum_rows = km_config_mode_line_format() ? 2u : 1u;

    return windows->count != SIZE_MAX &&
           windows->count + 1 <= available_rows / minimum_rows;
}

static KmStatus split_window_below(AppWindows *windows, size_t terminal_rows,
                                   KmError *error)
{
    AppWindow *current;
    AppWindow split;
    size_t insertion;
    KmStatus status;

    if (windows->count == 0 || windows->current >= windows->count ||
        !can_split_window(windows, terminal_rows)) {
        return app_fail(error, KM_ERR_INVALID, "split window");
    }
    status = reserve_windows(windows, windows->count + 1, error);
    if (status != KM_OK) return status;
    current = &windows->items[windows->current];
    split = (AppWindow){0};
    status = km_view_create(km_view_buffer(current->view), &split.view, error);
    if (status != KM_OK) return status;
    status = km_view_set_point(split.view, km_view_point(current->view), error);
    if (status != KM_OK) {
        (void)km_view_destroy(split.view, NULL);
        return status;
    }
    split.buffer = current->buffer;
    split.scroll_row = current->scroll_row;
    insertion = windows->current + 1;
    memmove(&windows->items[insertion + 1], &windows->items[insertion],
            (windows->count - insertion) * sizeof(*windows->items));
    windows->items[insertion] = split;
    ++windows->count;
    return KM_OK;
}

static KmStatus delete_window(AppWindows *windows, KmError *error)
{
    size_t victim;
    KmStatus status;

    if (windows->count <= 1 || windows->current >= windows->count) {
        return app_fail(error, KM_ERR_INVALID, "delete window");
    }
    victim = windows->current;
    status = km_view_destroy(windows->items[victim].view, error);
    if (status != KM_OK) return status;
    memmove(&windows->items[victim], &windows->items[victim + 1],
            (windows->count - victim - 1) * sizeof(*windows->items));
    --windows->count;
    if (windows->current == windows->count) --windows->current;
    return KM_OK;
}

static KmStatus delete_other_windows(AppWindows *windows, KmError *error)
{
    AppWindow selected;
    size_t i;

    if (windows->count == 0 || windows->current >= windows->count) {
        return app_fail(error, KM_ERR_INVALID, "delete other windows");
    }
    selected = windows->items[windows->current];
    for (i = 0; i < windows->count; ++i) {
        KmStatus status;
        if (i == windows->current) continue;
        status = km_view_destroy(windows->items[i].view, error);
        if (status != KM_OK) return status;
    }
    windows->items[0] = selected;
    windows->count = 1;
    windows->current = 0;
    return KM_OK;
}

static KmStatus other_window(AppWindows *windows, int64_t argument,
                             KmError *error)
{
    uint64_t magnitude;
    size_t offset;

    if (windows->count <= 1 || windows->current >= windows->count) {
        return app_fail(error, KM_ERR_INVALID, "other window");
    }
    magnitude = argument < 0 ? (uint64_t)(-(argument + 1)) + 1u
                             : (uint64_t)argument;
    offset = (size_t)(magnitude % windows->count);
    if (argument < 0) {
        windows->current =
            (windows->current + windows->count - offset) % windows->count;
    } else {
        windows->current = (windows->current + offset) % windows->count;
    }
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

static void set_global_display_line_numbers(AppBuffers *buffers, bool enabled)
{
    size_t i;

    km_config_set_global_display_line_numbers_mode(enabled);
    for (i = 0; i < buffers->count; ++i) {
        km_buffer_set_line_numbers_visible(buffers->items[i].buffer, enabled);
    }
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

static KmStatus switch_buffer(AppBuffers *buffers, AppWindow *window,
                              size_t index, KmError *error)
{
    bool changed;
    KmStatus status;

    if (window == NULL || index >= buffers->count) {
        return app_fail(error, KM_ERR_INVALID, "switch buffer");
    }
    changed = window->buffer != index;
    status = km_view_set_buffer(window->view, buffers->items[index].buffer,
                                error);
    if (status == KM_OK) {
        window->buffer = index;
        if (changed) window->scroll_row = 0;
        buffers->current = index;
    }
    return status;
}

static KmStatus visit_file(AppBuffers *buffers, AppWindow *window,
                           const char *name, KmError *error)
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
        return switch_buffer(buffers, window, existing, error);
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
    status = switch_buffer(buffers, window, buffers->count - 1, error);
    if (status != KM_OK) {
        --buffers->count;
        (void)km_buffer_destroy(buffer, NULL);
    }
    return status;
}

static KmStatus kill_current_buffer(AppBuffers *buffers, AppWindows *windows,
                                    KmError *error)
{
    size_t victim;
    size_t target;
    size_t i;
    KmBuffer *replacement = NULL;
    KmStatus status;

    if (windows->count == 0 || windows->current >= windows->count) {
        return app_fail(error, KM_ERR_INVALID, "kill buffer");
    }
    victim = windows->items[windows->current].buffer;
    if (buffers->count == 1) {
        status = km_buffer_create_base(NULL, 0, &replacement, error);
        if (status != KM_OK) return status;
        status = append_buffer(buffers, replacement, error);
        if (status != KM_OK) {
            (void)km_buffer_destroy(replacement, NULL);
            return status;
        }
    }
    target = (victim + 1) % buffers->count;
    for (i = 0; i < windows->count; ++i) {
        if (windows->items[i].buffer == victim) {
            status = switch_buffer(buffers, &windows->items[i], target, error);
            if (status != KM_OK) {
                return status;
            }
        }
    }
    status = km_buffer_destroy(buffers->items[victim].buffer, error);
    if (status != KM_OK) return status;
    memmove(&buffers->items[victim], &buffers->items[victim + 1],
            (buffers->count - victim - 1) * sizeof(*buffers->items));
    --buffers->count;
    for (i = 0; i < windows->count; ++i) {
        if (windows->items[i].buffer > victim) --windows->items[i].buffer;
    }
    buffers->current = windows->items[windows->current].buffer;
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

static void destroy_windows(AppWindows *windows)
{
    size_t i;

    for (i = 0; i < windows->count; ++i) {
        (void)km_view_destroy(windows->items[i].view, NULL);
    }
    free(windows->items);
}

static KmStatus remove_buffer(AppBuffers *buffers, AppWindows *windows,
                              size_t victim, size_t replacement,
                              KmError *error)
{
    size_t i;
    KmStatus status;

    if (victim >= buffers->count || replacement >= buffers->count ||
        victim == replacement) {
        return app_fail(error, KM_ERR_INVALID, "remove buffer");
    }
    for (i = 0; i < windows->count; ++i) {
        if (windows->items[i].buffer == victim) {
            status = switch_buffer(buffers, &windows->items[i], replacement,
                                   error);
            if (status != KM_OK) return status;
        }
    }
    status = km_buffer_destroy(buffers->items[victim].buffer, error);
    if (status != KM_OK) return status;
    memmove(&buffers->items[victim], &buffers->items[victim + 1],
            (buffers->count - victim - 1) * sizeof(*buffers->items));
    --buffers->count;
    for (i = 0; i < windows->count; ++i) {
        if (windows->items[i].buffer > victim) --windows->items[i].buffer;
    }
    buffers->current = windows->items[windows->current].buffer;
    return KM_OK;
}

static KmStatus build_buffer_list_text(const AppBuffers *buffers,
                                       size_t selected, size_t skip,
                                       uint8_t **out_text, size_t *out_len,
                                       KmError *error)
{
    static const char header[] = " MR Buffer  Size\n";
    size_t capacity = sizeof(header);
    size_t used;
    size_t i;
    uint8_t *text;

    if (out_text == NULL || out_len == NULL || selected >= buffers->count) {
        return app_fail(error, KM_ERR_INVALID, "buffer list text");
    }
    *out_text = NULL;
    *out_len = 0;
    for (i = 0; i < buffers->count; ++i) {
        if (i == skip) continue;
        size_t name_len = strlen(km_buffer_name(buffers->items[i].buffer));
        if (name_len > SIZE_MAX - capacity ||
            64u > SIZE_MAX - capacity - name_len) {
            return app_fail(error, KM_ERR_OOM, "buffer list text");
        }
        capacity += name_len + 64u;
    }
    text = (uint8_t *)malloc(capacity);
    if (text == NULL) return app_fail(error, KM_ERR_OOM, "buffer list text");
    memcpy(text, header, sizeof(header) - 1);
    used = sizeof(header) - 1;
    for (i = 0; i < buffers->count; ++i) {
        KmBuffer *buffer = buffers->items[i].buffer;
        if (i == skip) continue;
        int length = snprintf(
            (char *)text + used, capacity - used, "%c%c%c %s  %llu\n",
            i == selected ? '.' : ' ',
            km_buffer_is_modified(buffer) ? '*' : ' ',
            km_buffer_is_read_only(buffer) ? '%' : ' ',
            km_buffer_name(buffer),
            (unsigned long long)km_document_len(km_buffer_document(buffer)));
        if (length < 0 || (size_t)length >= capacity - used) {
            free(text);
            return app_fail(error, KM_ERR_INVALID, "buffer list text");
        }
        used += (size_t)length;
    }
    *out_text = text;
    *out_len = used;
    return KM_OK;
}

static KmStatus list_buffers(AppBuffers *buffers, AppWindows *windows,
                             size_t terminal_rows, KmError *error)
{
    static const char name[] = "*Buffer List*";
    size_t selected;
    size_t existing;
    size_t target_window;
    uint8_t *text = NULL;
    size_t text_len = 0;
    KmBuffer *list = NULL;
    bool split_created = false;
    KmStatus status;

    if (windows->count == 0 || windows->current >= windows->count) {
        return app_fail(error, KM_ERR_INVALID, "list buffers");
    }
    if (windows->count == 1 && !can_split_window(windows, terminal_rows)) {
        return app_fail(error, KM_ERR_INVALID, "list buffers");
    }
    selected = windows->items[windows->current].buffer;
    existing = find_buffer(buffers, name);
    if (existing != SIZE_MAX && selected == existing) {
        for (selected = 0; selected < buffers->count; ++selected) {
            if (selected != existing) break;
        }
        if (selected == buffers->count) {
            KmBuffer *scratch = NULL;
            status = km_buffer_create_base(NULL, 0, &scratch, error);
            if (status != KM_OK) return status;
            status = append_buffer(buffers, scratch, error);
            if (status != KM_OK) {
                (void)km_buffer_destroy(scratch, NULL);
                return status;
            }
            selected = buffers->count - 1;
        }
    }
    if (existing != SIZE_MAX) {
        size_t i;
        for (i = 0; i < windows->count; ++i) {
            if (windows->items[i].buffer == existing) {
                status = switch_buffer(buffers, &windows->items[i], selected,
                                       error);
                if (status != KM_OK) {
                    buffers->current =
                        windows->items[windows->current].buffer;
                    return status;
                }
            }
        }
        buffers->current = windows->items[windows->current].buffer;
    }
    status = build_buffer_list_text(buffers, selected, existing, &text,
                                    &text_len, error);
    if (status != KM_OK) return status;
    status = km_buffer_create_base(text, text_len, &list, error);
    free(text);
    if (status != KM_OK) return status;
    status = km_buffer_set_name(list, name, error);
    if (status != KM_OK) {
        (void)km_buffer_destroy(list, NULL);
        return status;
    }
    km_buffer_set_read_only(list, true);
    km_buffer_set_line_numbers_visible(list, false);
    status = append_buffer(buffers, list, error);
    if (status != KM_OK) {
        (void)km_buffer_destroy(list, NULL);
        return status;
    }
    if (windows->count == 1) {
        status = split_window_below(windows, terminal_rows, error);
        if (status != KM_OK) goto discard_list;
        split_created = true;
    }
    target_window = (windows->current + 1) % windows->count;
    status = switch_buffer(buffers, &windows->items[target_window],
                           buffers->count - 1, error);
    if (status != KM_OK) {
        if (split_created) {
            (void)km_view_destroy(windows->items[target_window].view, NULL);
            memmove(&windows->items[target_window],
                    &windows->items[target_window + 1],
                    (windows->count - target_window - 1) *
                        sizeof(*windows->items));
            --windows->count;
        }
        goto discard_list;
    }
    if (existing != SIZE_MAX) {
        status = remove_buffer(buffers, windows, existing, selected, error);
        if (status != KM_OK) return status;
    }
    buffers->current = windows->items[windows->current].buffer;
    return KM_OK;

discard_list:
    --buffers->count;
    (void)km_buffer_destroy(list, NULL);
    buffers->current = windows->items[windows->current].buffer;
    return status;
}

static void set_core_error(char *destination, size_t capacity,
                           const KmError *error)
{
    const char *operation = error->operation == NULL ? "editor" : error->operation;
    (void)snprintf(destination, capacity, "%s: %s", operation,
                   km_status_string(error->code));
}

static int redraw(App *app, bool prompt_active)
{
    KmCellGrid *grid = NULL;
    KmLayoutResult layout;
    KmLayoutWindow *layout_windows;
    KmError core_error;
    unsigned columns;
    unsigned rows;
    size_t i;
    int result;

    if (app->windows.count == 0 ||
        app->windows.current >= app->windows.count ||
        app->windows.count > SIZE_MAX / sizeof(*layout_windows)) {
        (void)snprintf(app->error, sizeof(app->error),
                       "layout frame: invalid state");
        return -1;
    }
    layout_windows =
        (KmLayoutWindow *)calloc(app->windows.count, sizeof(*layout_windows));
    if (layout_windows == NULL) {
        (void)snprintf(app->error, sizeof(app->error),
                       "layout frame: out of memory");
        return -1;
    }
    for (i = 0; i < app->windows.count; ++i) {
        if (app->windows.items[i].buffer >= app->buffers.count) {
            free(layout_windows);
            (void)snprintf(app->error, sizeof(app->error),
                           "layout frame: invalid buffer");
            return -1;
        }
        layout_windows[i].buffer =
            app->buffers.items[app->windows.items[i].buffer].buffer;
        layout_windows[i].view = app->windows.items[i].view;
        layout_windows[i].scroll_row = app->windows.items[i].scroll_row;
    }
    km_platform_size(app->platform, &columns, &rows);
    if (km_layout_frame(layout_windows, app->windows.count,
                        app->windows.current, rows, columns, app->message,
                        app->completion, prompt_active, &grid, &layout,
                        &core_error) != KM_OK) {
        free(layout_windows);
        set_core_error(app->error, sizeof(app->error), &core_error);
        return -1;
    }
    for (i = 0; i < app->windows.count; ++i) {
        app->windows.items[i].scroll_row = layout_windows[i].scroll_row;
        app->windows.items[i].rows = layout_windows[i].rows;
    }
    free(layout_windows);
    result = km_platform_draw_grid(app->platform, app->front_grid, grid,
                                   layout.cursor_row, layout.cursor_column,
                                   app->error, sizeof(app->error));
    if (result == 0) {
        km_cell_grid_destroy(app->front_grid);
        app->front_grid = grid;
    } else {
        km_cell_grid_destroy(grid);
    }
    return result;
}

static void app_destroy(App *app)
{
    km_cell_grid_destroy(app->front_grid);
    km_command_loop_destroy(app->loop);
    destroy_windows(&app->windows);
    destroy_buffers(&app->buffers);
    km_platform_close(app->platform);
}

static bool app_initialize(App *app, int argc, char **argv, bool *show_usage)
{
    KmPath *path = NULL;
    KmFile *file = NULL;
    KmBuffer *initial_buffer = NULL;
    uint8_t *initial_text = NULL;
    size_t initial_len = 0;
    KmError core_error;
    KmStatus status;
    bool initialized = false;

    *show_usage = false;
    status = km_configuration_validate(&core_error);
    if (status != KM_OK) goto core_failure;
    status = km_path_from_command_line(argc, argv, &path, &core_error);
    if (status != KM_OK) {
        *show_usage = true;
        goto core_failure;
    }
    if (path != NULL) {
        status = km_file_load(path, &file, &initial_text, &initial_len,
                              &core_error);
        path = NULL;
        if (status != KM_OK) goto core_failure;
    }
    if (km_platform_open(&app->platform, app->error, sizeof(app->error)) != 0) {
        goto done;
    }
    if (!km_platform_is_interactive(app->platform)) {
        (void)snprintf(app->error, sizeof(app->error),
                       "interactive terminal required");
        goto done;
    }
    status = file != NULL
                 ? km_buffer_create_file(file, initial_text, initial_len,
                                         &initial_buffer, &core_error)
                 : km_buffer_create_base(NULL, 0, &initial_buffer,
                                         &core_error);
    if (status != KM_OK) goto core_failure;
    file = NULL;
    free(initial_text);
    initial_text = NULL;
    status = append_buffer(&app->buffers, initial_buffer, &core_error);
    if (status != KM_OK) goto core_failure;
    initial_buffer = NULL;
    status = create_initial_window(&app->windows,
                                   app->buffers.items[0].buffer, &core_error);
    if (status != KM_OK) goto core_failure;
    status = km_command_loop_create(&app->loop, &core_error);
    if (status != KM_OK) goto core_failure;
    initialized = true;
    goto done;

core_failure:
    set_core_error(app->error, sizeof(app->error), &core_error);
done:
    if (initial_buffer != NULL) (void)km_buffer_destroy(initial_buffer, NULL);
    free(initial_text);
    km_file_destroy(file);
    km_path_destroy(path);
    return initialized;
}

static void set_request_message(App *app, KmStatus status,
                                const KmError *error, const char *success)
{
    if (status != KM_OK) {
        set_core_error(app->message, sizeof(app->message), error);
    } else if (success != NULL) {
        (void)snprintf(app->message, sizeof(app->message), "%s", success);
    }
}

static AppStep app_handle_request(App *app)
{
    AppWindow *active = &app->windows.items[app->windows.current];
    KmView *view = active->view;
    KmError core_error;
    KmStatus status;

    switch (km_command_loop_request(app->loop)) {
        case KM_COMMAND_REQUEST_SAVE:
            km_command_loop_clear_request(app->loop);
            status = km_buffer_save(app->buffers.items[app->buffers.current].buffer,
                                    &core_error);
            set_request_message(app, status, &core_error, "Wrote");
            break;
        case KM_COMMAND_REQUEST_SAVE_ALL_EXIT:
            km_command_loop_clear_request(app->loop);
            status = save_modified_files(&app->buffers, &core_error);
            if (status != KM_OK) {
                set_request_message(app, status, &core_error, NULL);
                break;
            }
            if (!any_modified_buffer(&app->buffers)) return APP_STEP_EXIT;
            status = km_command_loop_confirm_exit(app->loop, &core_error);
            if (status != KM_OK) {
                set_core_error(app->error, sizeof(app->error), &core_error);
                return APP_STEP_ERROR;
            }
            break;
        case KM_COMMAND_REQUEST_FIND_FILE: {
            const char *name = km_command_loop_request_text(app->loop);
            km_command_loop_clear_request(app->loop);
            status = visit_file(&app->buffers, active,
                                name[0] == '\0' ? "." : name,
                                &core_error);
            set_request_message(app, status, &core_error, "Opened");
            break;
        }
        case KM_COMMAND_REQUEST_SWITCH_BUFFER: {
            const char *name = km_command_loop_request_text(app->loop);
            size_t target = name[0] == '\0'
                                ? (app->buffers.current + 1) % app->buffers.count
                                : find_buffer(&app->buffers, name);
            km_command_loop_clear_request(app->loop);
            status = switch_buffer(&app->buffers, active, target, &core_error);
            set_request_message(app, status, &core_error, "Switched");
            break;
        }
        case KM_COMMAND_REQUEST_KILL_BUFFER: {
            km_command_loop_clear_request(app->loop);
            status = kill_current_buffer(&app->buffers, &app->windows,
                                         &core_error);
            set_request_message(app, status, &core_error, "Killed");
            break;
        }
        case KM_COMMAND_REQUEST_COMPLETE_FILE: {
            const char *prefix = km_command_loop_request_text(app->loop);
            KmPathCompletions candidates = {0};
            km_command_loop_clear_request(app->loop);
            status = km_path_completions_utf8(prefix, &candidates,
                                              &core_error);
            if (status == KM_OK) {
                status = km_command_loop_set_completions(
                    app->loop, (const char *const *)candidates.items,
                    candidates.count, candidates.common, &core_error);
            }
            km_path_completions_destroy(&candidates);
            set_request_message(app, status, &core_error, NULL);
            break;
        }
        case KM_COMMAND_REQUEST_COMPLETE_BUFFER: {
            const char *prefix = km_command_loop_request_text(app->loop);
            km_command_loop_clear_request(app->loop);
            status = set_buffer_completions(&app->buffers, app->loop, prefix,
                                            &core_error);
            set_request_message(app, status, &core_error, NULL);
            break;
        }
        case KM_COMMAND_REQUEST_SCROLL_UP:
        case KM_COMMAND_REQUEST_SCROLL_DOWN: {
            KmCommandRequest request = km_command_loop_request(app->loop);
            int64_t argument = km_command_loop_request_argument(app->loop);
            bool has_argument =
                km_command_loop_request_has_argument(app->loop);
            bool forward = request == KM_COMMAND_REQUEST_SCROLL_UP;
            size_t *scroll_row = &active->scroll_row;
            unsigned columns;
            unsigned rows;
            size_t layout_rows;
            if (km_command_loop_request_page_opposite(app->loop)) {
                forward = !forward;
                has_argument = false;
            }
            km_command_loop_clear_request(app->loop);
            km_platform_size(app->platform, &columns, &rows);
            layout_rows = active->rows + (rows > 1 ? 1u : 0u);
            status = km_layout_scroll_view(
                app->buffers.items[app->buffers.current].buffer, view,
                layout_rows,
                columns,
                *scroll_row, forward, has_argument, argument, scroll_row,
                &core_error);
            set_request_message(app, status, &core_error, NULL);
            break;
        }
        case KM_COMMAND_REQUEST_RECENTER: {
            size_t *scroll_row = &active->scroll_row;
            KmCellGrid *scratch = NULL;
            KmLayoutResult layout;
            int64_t argument = km_command_loop_request_argument(app->loop);
            bool has_argument =
                km_command_loop_request_has_argument(app->loop);
            unsigned columns;
            unsigned rows;
            size_t layout_rows;

            km_command_loop_clear_request(app->loop);
            km_platform_size(app->platform, &columns, &rows);
            layout_rows = active->rows + (rows > 1 ? 1u : 0u);
            status = km_layout_view(
                app->buffers.items[app->buffers.current].buffer, view,
                layout_rows,
                columns,
                *scroll_row, NULL, NULL, false, &scratch, &layout,
                &core_error);
            if (status != KM_OK) {
                set_request_message(app, status, &core_error, NULL);
                break;
            }
            km_cell_grid_destroy(scratch);
            status = km_layout_recenter_scroll(
                &layout, layout_rows, has_argument, argument, scroll_row,
                &core_error);
            set_request_message(app, status, &core_error, NULL);
            break;
        }
        case KM_COMMAND_REQUEST_MOVE_TO_WINDOW_LINE: {
            int64_t argument = km_command_loop_request_argument(app->loop);
            bool has_argument =
                km_command_loop_request_has_argument(app->loop);
            unsigned columns;
            unsigned rows;

            km_command_loop_clear_request(app->loop);
            km_platform_size(app->platform, &columns, &rows);
            status = km_layout_move_to_window_line(
                app->buffers.items[app->buffers.current].buffer, view,
                active->rows + (rows > 1 ? 1u : 0u), columns,
                active->scroll_row, has_argument,
                argument, &core_error);
            set_request_message(app, status, &core_error, NULL);
            break;
        }
        case KM_COMMAND_REQUEST_GLOBAL_DISPLAY_LINE_NUMBERS_MODE: {
            bool enabled = km_command_loop_request_argument(app->loop) > 0;
            km_command_loop_clear_request(app->loop);
            set_global_display_line_numbers(&app->buffers, enabled);
            break;
        }
        case KM_COMMAND_REQUEST_SPLIT_WINDOW_BELOW: {
            unsigned columns;
            unsigned rows;
            (void)columns;
            km_command_loop_clear_request(app->loop);
            km_platform_size(app->platform, &columns, &rows);
            status = split_window_below(&app->windows, rows, &core_error);
            set_request_message(app, status, &core_error, NULL);
            break;
        }
        case KM_COMMAND_REQUEST_DELETE_WINDOW:
            km_command_loop_clear_request(app->loop);
            status = delete_window(&app->windows, &core_error);
            set_request_message(app, status, &core_error, NULL);
            app->buffers.current =
                app->windows.items[app->windows.current].buffer;
            break;
        case KM_COMMAND_REQUEST_DELETE_OTHER_WINDOWS:
            km_command_loop_clear_request(app->loop);
            status = delete_other_windows(&app->windows, &core_error);
            set_request_message(app, status, &core_error, NULL);
            app->buffers.current =
                app->windows.items[app->windows.current].buffer;
            break;
        case KM_COMMAND_REQUEST_OTHER_WINDOW: {
            int64_t argument = km_command_loop_request_argument(app->loop);
            km_command_loop_clear_request(app->loop);
            status = other_window(&app->windows, argument, &core_error);
            set_request_message(app, status, &core_error, NULL);
            app->buffers.current =
                app->windows.items[app->windows.current].buffer;
            break;
        }
        case KM_COMMAND_REQUEST_LIST_BUFFERS: {
            unsigned columns;
            unsigned rows;
            (void)columns;
            km_command_loop_clear_request(app->loop);
            km_platform_size(app->platform, &columns, &rows);
            status = list_buffers(&app->buffers, &app->windows, rows,
                                  &core_error);
            set_request_message(app, status, &core_error, NULL);
            break;
        }
        case KM_COMMAND_REQUEST_EXIT:
            km_command_loop_clear_request(app->loop);
            if (!any_modified_buffer(&app->buffers)) return APP_STEP_EXIT;
            status = km_command_loop_confirm_exit(app->loop, &core_error);
            if (status != KM_OK) {
                set_core_error(app->error, sizeof(app->error), &core_error);
                return APP_STEP_ERROR;
            }
            break;
        case KM_COMMAND_REQUEST_EXIT_CONFIRMED:
            km_command_loop_clear_request(app->loop);
            return APP_STEP_EXIT;
        case KM_COMMAND_REQUEST_NONE:
        default:
            break;
    }
    return APP_STEP_CONTINUE;
}

static AppStep app_process_event(App *app)
{
    AppWindow *active = &app->windows.items[app->windows.current];
    KmEvent event;
    KmError core_error;
    KmStatus status;
    AppStep step;
    bool prompt_active;

    app->buffers.current = active->buffer;
    if (km_platform_read_event(app->platform, &event, app->error,
                               sizeof(app->error)) != 0) {
        return APP_STEP_ERROR;
    }
    if (event.kind == KM_EVENT_EOF) return APP_STEP_EXIT;
    status = km_command_loop_dispatch(app->loop, active->view, &event,
                                      &core_error);
    if (status == KM_OK) {
        app->message[0] = '\0';
    } else if (status == KM_ERR_INVALID || status == KM_ERR_PERMISSION ||
               status == KM_ERR_CANCELLED) {
        set_core_error(app->message, sizeof(app->message), &core_error);
    } else {
        set_core_error(app->error, sizeof(app->error), &core_error);
        return APP_STEP_ERROR;
    }
    if (km_command_loop_quit_requested(app->loop)) {
        (void)snprintf(app->message, sizeof(app->message), "Quit");
        km_command_loop_clear_quit(app->loop);
    }
    step = app_handle_request(app);
    if (step != APP_STEP_CONTINUE) return step;
    prompt_active = km_command_loop_search_active(app->loop) ||
                    km_command_loop_prompt_active(app->loop);
    if (prompt_active) {
        km_command_loop_format_prompt(app->loop, app->message,
                                      sizeof(app->message));
        km_command_loop_format_completions(app->loop, app->completion,
                                           sizeof(app->completion));
    } else {
        app->completion[0] = '\0';
    }
    return redraw(app, prompt_active) == 0 ? APP_STEP_CONTINUE
                                           : APP_STEP_ERROR;
}

static int app_run(App *app)
{
    AppStep step;

    if (redraw(app, false) != 0) return 1;
    do {
        step = app_process_event(app);
    } while (step == APP_STEP_CONTINUE);
    return step == APP_STEP_EXIT ? 0 : 1;
}

static int app_main(int argc, char **argv)
{
    App app = {0};
    bool show_usage;
    int result = 1;

    if (app_initialize(&app, argc, argv, &show_usage)) result = app_run(&app);
    app_destroy(&app);
    if (result != 0) {
        if (show_usage) fprintf(stderr, "usage: %s [file]\n", argv[0]);
        fprintf(stderr, "km: %s\n", app.error);
    }
    return result;
}

int main(int argc, char **argv)
{
    return app_main(argc, argv);
}
