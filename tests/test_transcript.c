#include "editor.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) return 1;                                            \
    } while (0)

static KmStatus key(KmCommandLoop *loop, KmView *view, uint32_t codepoint,
                    uint32_t modifiers, KmError *error)
{
    KmEvent event = {0};
    event.kind = KM_EVENT_KEY;
    event.codepoint = codepoint;
    event.modifiers = modifiers;
    event.repeat = 1;
    return km_command_loop_dispatch(loop, view, &event, error);
}

static size_t character_point(const KmBuffer *buffer, KmBytePos point)
{
    const KmDocument *document = km_buffer_document(buffer);
    uint8_t *text = point.v == 0 ? NULL : (uint8_t *)malloc(point.v);
    KmError error;
    size_t result = 1;
    size_t i;

    if (point.v != 0 && text == NULL) return 0;
    if (km_document_copy(document, (KmBytePos){0}, point.v, text, &error) !=
        KM_OK) {
        free(text);
        return 0;
    }
    for (i = 0; i < point.v; ++i) {
        if ((text[i] & 0xc0u) != 0x80u) ++result;
    }
    free(text);
    return result;
}

static int emit(const char *name, KmBuffer *buffer, KmView *view)
{
    const KmDocument *document = km_buffer_document(buffer);
    size_t len = km_document_len(document);
    uint8_t *text = len == 0 ? NULL : (uint8_t *)malloc(len);
    KmError error;
    size_t i;

    if (len != 0 && text == NULL) return 1;
    if (km_document_copy(document, (KmBytePos){0}, len, text, &error) != KM_OK) {
        free(text);
        return 1;
    }
    printf("%s:", name);
    for (i = 0; i < len; ++i) printf("%02x", text[i]);
    printf(":%zu\n", character_point(buffer, km_view_point(view)));
    free(text);
    return 0;
}

static int run_case(const char *name, const uint8_t *text, size_t len,
                    size_t point, int scenario)
{
    KmBuffer *buffer = NULL;
    KmView *view = NULL;
    KmCommandLoop *loop = NULL;
    KmError error;
    int result = 1;

    if (km_buffer_create_base(text, len, &buffer, &error) != KM_OK ||
        km_view_create(buffer, &view, &error) != KM_OK ||
        km_command_loop_create(&loop, &error) != KM_OK ||
        km_view_set_point(view, (KmBytePos){point}, &error) != KM_OK) {
        goto done;
    }
    if (scenario == 0) {
        CHECK(key(loop, view, 'c', KM_MOD_ALT, &error) == KM_OK);
        CHECK(km_view_set_point(view, (KmBytePos){7}, &error) == KM_OK);
        CHECK(key(loop, view, 'u', KM_MOD_ALT, &error) == KM_OK);
    } else if (scenario == 1) {
        CHECK(key(loop, view, ' ', KM_MOD_ALT, &error) == KM_OK);
    } else if (scenario == 2) {
        CHECK(key(loop, view, '^', KM_MOD_ALT | KM_MOD_SHIFT, &error) == KM_OK);
    } else if (scenario == 3) {
        CHECK(key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
        CHECK(key(loop, view, 'o', KM_MOD_CTRL, &error) == KM_OK);
    } else if (scenario == 4) {
        CHECK(key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
        CHECK(key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
        CHECK(key(loop, view, 't', KM_MOD_CTRL, &error) == KM_OK);
    } else if (scenario == 5) {
        CHECK(key(loop, view, '3', KM_MOD_ALT, &error) == KM_OK);
        CHECK(key(loop, view, 'g', KM_MOD_ALT, &error) == KM_OK);
        CHECK(key(loop, view, 'c', 0, &error) == KM_OK);
    } else if (scenario == 8) {
        CHECK(key(loop, view, '9', KM_MOD_ALT, &error) == KM_OK);
        CHECK(key(loop, view, 'f', KM_MOD_CTRL, &error) == KM_ERR_INVALID);
    } else if (scenario == 9) {
        CHECK(key(loop, view, '9', KM_MOD_ALT, &error) == KM_OK);
        CHECK(key(loop, view, 'f', KM_MOD_ALT, &error) == KM_OK);
    } else if (scenario == 10) {
        CHECK(key(loop, view, '9', KM_MOD_ALT, &error) == KM_OK);
        CHECK(key(loop, view, '}', KM_MOD_ALT, &error) == KM_OK);
    } else if (scenario == 11) {
        CHECK(key(loop, view, '9', KM_MOD_ALT, &error) == KM_OK);
        CHECK(key(loop, view, 'e', KM_MOD_ALT, &error) == KM_ERR_INVALID);
    } else if (scenario == 12) {
        CHECK(key(loop, view, '9', KM_MOD_ALT, &error) == KM_OK);
        CHECK(key(loop, view, 'a', KM_MOD_ALT, &error) == KM_OK);
    } else if (scenario == 13) {
        CHECK(key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
        CHECK(key(loop, view, ' ', 0, &error) == KM_OK);
        CHECK(km_view_set_point(view, (KmBytePos){19}, &error) == KM_OK);
        CHECK(key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
        CHECK(key(loop, view, 'r', 0, &error) == KM_OK);
        CHECK(key(loop, view, 'k', 0, &error) == KM_OK);
        CHECK(km_view_set_point(view, (KmBytePos){0}, &error) == KM_OK);
        CHECK(key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
        CHECK(key(loop, view, 'r', 0, &error) == KM_OK);
        CHECK(key(loop, view, 'y', 0, &error) == KM_OK);
    } else {
        if (scenario == 7) {
            CHECK(key(loop, view, '2', KM_MOD_ALT, &error) == KM_OK);
        }
        CHECK(key(loop, view, 'x', KM_MOD_CTRL, &error) == KM_OK);
        CHECK(key(loop, view, 't', KM_MOD_CTRL, &error) == KM_OK);
    }
    result = emit(name, buffer, view);

done:
    km_command_loop_destroy(loop);
    if (view != NULL) (void)km_view_destroy(view, NULL);
    if (buffer != NULL) (void)km_buffer_destroy(buffer, NULL);
    return result;
}

int main(void)
{
    static const uint8_t case_text[] = "\xc3\xa9" "COLE foo";
    static const uint8_t goto_text[] = {'A', 0xe4, 0xb8, 0xad, 'B'};

    if (run_case("case", case_text, sizeof(case_text) - 1, 0, 0) != 0 ||
        run_case("space", (const uint8_t *)"a \t b", 5, 3, 1) != 0 ||
        run_case("join", (const uint8_t *)"aa\n  bb", 7, 4, 2) != 0 ||
        run_case("blank", (const uint8_t *)"a\n\n\nb", 5, 2, 3) != 0 ||
        run_case("transpose", (const uint8_t *)"aa\nbb\ncc\ndd", 11, 4,
                 4) != 0 ||
        run_case("goto", goto_text, sizeof(goto_text), 0, 5) != 0) {
        return 1;
    }
    if (run_case("transpose-final", (const uint8_t *)"a\nb", 3, 2, 6) !=
            0 ||
        run_case("transpose-final-2", (const uint8_t *)"a\nb", 3, 2, 7) !=
            0) {
        return 1;
    }
    if (run_case("forward-char-boundary", (const uint8_t *)"ab", 2, 0, 8) !=
            0 ||
        run_case("forward-word-boundary", (const uint8_t *)"a b", 3, 0, 9) !=
            0 ||
        run_case("forward-paragraph-boundary", (const uint8_t *)"a\n\nb", 4,
                 0, 10) != 0 ||
        run_case("forward-sentence-boundary",
                 (const uint8_t *)"One.  Two.", 10, 0, 11) != 0 ||
        run_case("backward-sentence-boundary",
                 (const uint8_t *)"One.  Two.", 10, 10, 12) != 0 ||
        run_case("rectangle",
                 (const uint8_t *)"abcdef\nghijkl\nmnopqr", 20, 1, 13) != 0) {
        return 1;
    }
    return 0;
}
