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

static int redraw(KmPlatform *platform, KmCellGrid **front,
                  const AppBuffers *buffers, AppWindows *windows,
                  const char *message,
                  const char *completion, bool prompt_active, char *error,
                  size_t error_cap)
{
    KmCellGrid *grid = NULL;
    KmLayoutResult layout;
    KmLayoutWindow *layout_windows;
    KmError core_error;
    unsigned columns;
    unsigned rows;
    size_t i;
    int result;

    if (windows->count == 0 || windows->current >= windows->count ||
        windows->count > SIZE_MAX / sizeof(*layout_windows)) {
        (void)snprintf(error, error_cap, "layout frame: invalid state");
        return -1;
    }
    layout_windows =
        (KmLayoutWindow *)calloc(windows->count, sizeof(*layout_windows));
    if (layout_windows == NULL) {
        (void)snprintf(error, error_cap, "layout frame: out of memory");
        return -1;
    }
    for (i = 0; i < windows->count; ++i) {
        if (windows->items[i].buffer >= buffers->count) {
            free(layout_windows);
            (void)snprintf(error, error_cap, "layout frame: invalid buffer");
            return -1;
        }
        layout_windows[i].buffer =
            buffers->items[windows->items[i].buffer].buffer;
        layout_windows[i].view = windows->items[i].view;
        layout_windows[i].scroll_row = windows->items[i].scroll_row;
    }
    km_platform_size(platform, &columns, &rows);
    if (km_layout_frame(layout_windows, windows->count, windows->current, rows,
                        columns, message, completion, prompt_active, &grid,
                        &layout, &core_error) != KM_OK) {
        free(layout_windows);
        set_core_error(error, error_cap, &core_error);
        return -1;
    }
    for (i = 0; i < windows->count; ++i) {
        windows->items[i].scroll_row = layout_windows[i].scroll_row;
        windows->items[i].rows = layout_windows[i].rows;
    }
    free(layout_windows);
    result = km_platform_draw_grid(platform, *front, grid, layout.cursor_row,
                                   layout.cursor_column, error, error_cap);
    if (result == 0) {
        km_cell_grid_destroy(*front);
        *front = grid;
    } else {
        km_cell_grid_destroy(grid);
    }
    return result;
}

int main(int argc, char **argv)
{
    char platform_error[256] = {0};
    char message[256] = {0};
    char completion[4096] = {0};
    KmPlatform *platform = NULL;
    KmCellGrid *front_grid = NULL;
    KmBuffer *initial_buffer = NULL;
    KmCommandLoop *loop = NULL;
    AppBuffers buffers = {0};
    AppWindows windows = {0};
    KmPath *path = NULL;
    KmFile *file = NULL;
    uint8_t *initial_text = NULL;
    size_t initial_len = 0;
    KmEvent event;
    KmError core_error;
    int result = 1;

    if (km_configuration_validate(&core_error) != KM_OK) {
        set_core_error(platform_error, sizeof(platform_error), &core_error);
        fprintf(stderr, "km: %s\n", platform_error);
        return 1;
    }
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
    if (create_initial_window(&windows, buffers.items[0].buffer, &core_error) !=
            KM_OK ||
        km_command_loop_create(&loop, &core_error) != KM_OK) {
        set_core_error(platform_error, sizeof(platform_error), &core_error);
        goto done;
    }
    if (redraw(platform, &front_grid, &buffers, &windows, message, completion,
               false, platform_error, sizeof(platform_error)) != 0) {
        goto done;
    }

    for (;;) {
        AppWindow *active = &windows.items[windows.current];
        KmView *view = active->view;
        KmStatus status;

        buffers.current = active->buffer;

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
            status = visit_file(&buffers, active,
                                name[0] == '\0' ? "." : name,
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
            status = switch_buffer(&buffers, active, target, &core_error);
            if (status == KM_OK) {
                (void)snprintf(message, sizeof(message), "Switched");
            } else {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        }
        case KM_COMMAND_REQUEST_KILL_BUFFER: {
            km_command_loop_clear_request(loop);
            status = kill_current_buffer(&buffers, &windows, &core_error);
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
            size_t *scroll_row = &active->scroll_row;
            unsigned columns;
            unsigned rows;
            size_t layout_rows;
            if (km_command_loop_request_page_opposite(loop)) {
                forward = !forward;
                has_argument = false;
            }
            km_command_loop_clear_request(loop);
            km_platform_size(platform, &columns, &rows);
            layout_rows = active->rows + (rows > 1 ? 1u : 0u);
            status = km_layout_scroll_view(
                buffers.items[buffers.current].buffer, view, layout_rows,
                columns,
                *scroll_row, forward, has_argument, argument, scroll_row,
                &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        }
        case KM_COMMAND_REQUEST_RECENTER: {
            size_t *scroll_row = &active->scroll_row;
            KmCellGrid *scratch = NULL;
            KmLayoutResult layout;
            int64_t argument = km_command_loop_request_argument(loop);
            bool has_argument =
                km_command_loop_request_has_argument(loop);
            unsigned columns;
            unsigned rows;
            size_t layout_rows;

            km_command_loop_clear_request(loop);
            km_platform_size(platform, &columns, &rows);
            layout_rows = active->rows + (rows > 1 ? 1u : 0u);
            status = km_layout_view(
                buffers.items[buffers.current].buffer, view, layout_rows,
                columns,
                *scroll_row, NULL, NULL, false, &scratch, &layout,
                &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
                break;
            }
            km_cell_grid_destroy(scratch);
            status = km_layout_recenter_scroll(
                &layout, layout_rows, has_argument, argument, scroll_row,
                &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        }
        case KM_COMMAND_REQUEST_MOVE_TO_WINDOW_LINE: {
            int64_t argument = km_command_loop_request_argument(loop);
            bool has_argument =
                km_command_loop_request_has_argument(loop);
            unsigned columns;
            unsigned rows;

            km_command_loop_clear_request(loop);
            km_platform_size(platform, &columns, &rows);
            status = km_layout_move_to_window_line(
                buffers.items[buffers.current].buffer, view,
                active->rows + (rows > 1 ? 1u : 0u), columns,
                active->scroll_row, has_argument,
                argument, &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        }
        case KM_COMMAND_REQUEST_GLOBAL_DISPLAY_LINE_NUMBERS_MODE: {
            bool enabled = km_command_loop_request_argument(loop) > 0;
            km_command_loop_clear_request(loop);
            set_global_display_line_numbers(&buffers, enabled);
            break;
        }
        case KM_COMMAND_REQUEST_SPLIT_WINDOW_BELOW: {
            unsigned columns;
            unsigned rows;
            (void)columns;
            km_command_loop_clear_request(loop);
            km_platform_size(platform, &columns, &rows);
            status = split_window_below(&windows, rows, &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
            }
            break;
        }
        case KM_COMMAND_REQUEST_DELETE_WINDOW:
            km_command_loop_clear_request(loop);
            status = delete_window(&windows, &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
            }
            buffers.current = windows.items[windows.current].buffer;
            break;
        case KM_COMMAND_REQUEST_DELETE_OTHER_WINDOWS:
            km_command_loop_clear_request(loop);
            status = delete_other_windows(&windows, &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
            }
            buffers.current = windows.items[windows.current].buffer;
            break;
        case KM_COMMAND_REQUEST_OTHER_WINDOW: {
            int64_t argument = km_command_loop_request_argument(loop);
            km_command_loop_clear_request(loop);
            status = other_window(&windows, argument, &core_error);
            if (status != KM_OK) {
                set_core_error(message, sizeof(message), &core_error);
            }
            buffers.current = windows.items[windows.current].buffer;
            break;
        }
        case KM_COMMAND_REQUEST_LIST_BUFFERS: {
            unsigned columns;
            unsigned rows;
            (void)columns;
            km_command_loop_clear_request(loop);
            km_platform_size(platform, &columns, &rows);
            status = list_buffers(&buffers, &windows, rows, &core_error);
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
        if (redraw(platform, &front_grid, &buffers, &windows, message,
                   completion, km_command_loop_search_active(loop) ||
                       km_command_loop_prompt_active(loop),
                   platform_error, sizeof(platform_error)) != 0) {
            goto done;
        }
    }

done:
    km_cell_grid_destroy(front_grid);
    km_command_loop_destroy(loop);
    destroy_windows(&windows);
    destroy_buffers(&buffers);
    if (initial_buffer != NULL) (void)km_buffer_destroy(initial_buffer, NULL);
    free(initial_text);
    km_file_destroy(file);
    km_path_destroy(path);
    km_platform_close(platform);
    if (result != 0) fprintf(stderr, "km: %s\n", platform_error);
    return result;
}
