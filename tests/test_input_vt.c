#include "input_vt.h"
#include "configuration.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

typedef struct {
    KmEventKind kind;
    uint32_t codepoint;
    uint32_t modifiers;
    uint8_t text[16];
    size_t text_len;
} CapturedEvent;

typedef struct {
    CapturedEvent events[32];
    size_t count;
} Capture;

static void capture_event(Capture *capture, const KmEvent *event)
{
    CapturedEvent *saved;

    CHECK(capture->count < sizeof(capture->events) / sizeof(capture->events[0]));
    saved = &capture->events[capture->count++];
    memset(saved, 0, sizeof(*saved));
    saved->kind = event->kind;
    saved->codepoint = event->codepoint;
    saved->modifiers = event->modifiers;
    saved->text_len = event->text_len;
    CHECK(event->text_len <= sizeof(saved->text));
    if (event->text_len != 0) {
        memcpy(saved->text, event->text, event->text_len);
    }
}

static Capture parse_chunks(const uint8_t *bytes, size_t len, size_t chunk)
{
    Capture capture = {0};
    KmInputVt *input = NULL;
    KmError error;
    size_t offset = 0;

    CHECK(chunk != 0);
    CHECK(km_input_vt_create(&input, &error) == KM_OK);
    while (offset < len) {
        size_t end = len - offset < chunk ? len : offset + chunk;

        while (offset < end) {
            KmEvent event;
            size_t consumed = 0;
            bool ready = false;
            KmStatus status = km_input_vt_feed(
                input, bytes + offset, end - offset, false, &consumed,
                &event, &ready, &error);

            CHECK(status == KM_OK);
            CHECK(consumed != 0 || ready);
            offset += consumed;
            if (ready) capture_event(&capture, &event);
        }
    }
    for (;;) {
        KmEvent event;
        size_t consumed = 0;
        bool ready = false;

        CHECK(km_input_vt_feed(input, NULL, 0, true, &consumed, &event,
                               &ready, &error) == KM_OK);
        CHECK(ready);
        capture_event(&capture, &event);
        if (event.kind == KM_EVENT_EOF) break;
    }
    km_input_vt_destroy(input);
    return capture;
}

static void check_same_capture(const Capture *left, const Capture *right)
{
    CHECK(left->count == right->count);
    for (size_t i = 0; i < left->count; ++i) {
        CHECK(left->events[i].kind == right->events[i].kind);
        CHECK(left->events[i].codepoint == right->events[i].codepoint);
        CHECK(left->events[i].modifiers == right->events[i].modifiers);
        CHECK(left->events[i].text_len == right->events[i].text_len);
        CHECK(left->events[i].text_len == 0 ||
              memcmp(left->events[i].text, right->events[i].text,
                     left->events[i].text_len) == 0);
    }
}

static void test_chunk_independence(void)
{
    static const uint8_t bytes[] = {
        0x07, 0x1f, '\t', 0xc2, 'A', '\r', 0x7f, 0x00,
        0x1b, '[', 'A',
        0x1b, 'O', 'F',
        0x1b, '[', '3', '~',
        0x1b, '[', '1', '~', 0x1b, '[', '7', '~',
        0x1b, '[', '4', '~', 0x1b, '[', '8', '~',
        0x1b, '3',
        0x1b, '[', '2', '0', '0', '~',
        'p', 0, 'q', '\r', 'x', '\r', '\n', 'y', '\n', 'z',
        0x1b, '[', '2', '0', '1', '~',
        0x1b, ']', 't', 'i', 't', 'l', 'e', 0x07, 'X',
        0x1b, ']', 't', 'i', 't', 'l', 'e', 0x1b, '\\', 'Y',
        0x1b, '[',
        '1', '1', '1', '1', '1', '1', '1', '1', '1', '1',
        '1', '1', '1', '1', '1', '1', '1', '1', '1', '1', 'z', 'Z',
        0xe2, 0x82,
    };
    Capture whole = parse_chunks(bytes, sizeof(bytes), sizeof(bytes));
    Capture bytes_at_a_time = parse_chunks(bytes, sizeof(bytes), 1);
    Capture chunks = parse_chunks(bytes, sizeof(bytes), 7);

    check_same_capture(&whole, &bytes_at_a_time);
    check_same_capture(&whole, &chunks);
    if (whole.count != 23) {
        fprintf(stderr, "captured %zu VT events:", whole.count);
        for (size_t i = 0; i < whole.count; ++i) {
            fprintf(stderr, " %d/%x/%x", (int)whole.events[i].kind,
                    (unsigned)whole.events[i].codepoint,
                    (unsigned)whole.events[i].modifiers);
        }
        fputc('\n', stderr);
    }
    CHECK(whole.count == 23);
    CHECK(whole.events[0].kind == KM_EVENT_KEY &&
          whole.events[0].codepoint == 'g' &&
          whole.events[0].modifiers == KM_MOD_CTRL);
    CHECK(whole.events[3].kind == KM_EVENT_TEXT &&
          whole.events[3].codepoint == 0xfffd);
    CHECK(whole.events[8].codepoint == KM_KEY_UP);
    CHECK(whole.events[9].codepoint == KM_KEY_END);
    CHECK(whole.events[10].codepoint == KM_KEY_DELETE);
    CHECK(whole.events[11].codepoint == KM_KEY_HOME);
    CHECK(whole.events[13].codepoint == KM_KEY_END);
    CHECK(whole.events[15].codepoint == '3' &&
          whole.events[15].modifiers == KM_MOD_ALT);
    CHECK(whole.events[16].kind == KM_EVENT_PASTE &&
          whole.events[16].text_len == 9 &&
          memcmp(whole.events[16].text, "p\0q\nx\ny\nz", 9) == 0);
    CHECK(whole.events[17].codepoint == 'X');
    CHECK(whole.events[18].codepoint == 'Y');
    CHECK(whole.events[19].codepoint == KM_KEY_ESCAPE);
    CHECK(whole.events[20].codepoint == 'Z');
    CHECK(whole.events[21].codepoint == 0xfffd);
    CHECK(whole.events[22].kind == KM_EVENT_EOF);
}

static void test_timeout(void)
{
    KmInputVt *input = NULL;
    KmError error;
    KmEvent event;
    size_t consumed;
    bool ready;
    static const uint8_t escape = 0x1b;
    static const uint8_t meta_start[] = {0x1b, 0xe2};

    CHECK(km_input_vt_create(&input, &error) == KM_OK);
    CHECK(km_input_vt_feed(input, &escape, 1, false, &consumed, &event,
                           &ready, &error) == KM_OK);
    CHECK(consumed == 1 && !ready && km_input_vt_wants_timeout(input));
    CHECK(km_input_vt_timeout(input, &event, &ready, &error) == KM_OK);
    CHECK(ready && event.kind == KM_EVENT_KEY &&
          event.codepoint == KM_KEY_ESCAPE);
    CHECK(km_input_vt_feed(input, meta_start, sizeof(meta_start), false,
                           &consumed, &event, &ready, &error) == KM_OK);
    CHECK(consumed == sizeof(meta_start) && !ready &&
          km_input_vt_wants_timeout(input));
    CHECK(km_input_vt_timeout(input, &event, &ready, &error) == KM_OK);
    CHECK(ready && event.codepoint == 0xfffd &&
          event.modifiers == KM_MOD_ALT);
    km_input_vt_destroy(input);
}

static void test_eof_and_discard_edges(void)
{
    static const uint8_t final_chunk[] = {'a', 'b'};
    static const uint8_t osc_escape_bel[] = {
        0x1b, ']', 'x', 0x1b, 0x07, 'Z',
    };
    static const uint8_t overlong[] = {
        0x1b, '[',
        '1', '1', '1', '1', '1', '1', '1', '1',
        '1', '1', '1', '1', '1', '1', '1', '1',
    };
    static const uint8_t discard_end[] = {'1', '1', 'z', 'Q'};
    KmInputVt *input = NULL;
    KmError error;
    KmEvent event;
    size_t consumed = 0;
    bool ready = false;

    CHECK(km_input_vt_create(&input, &error) == KM_OK);
    CHECK(km_input_vt_feed(input, final_chunk, sizeof(final_chunk), true,
                           &consumed, &event, &ready, &error) == KM_OK);
    CHECK(consumed == 1 && ready && event.codepoint == 'a');
    CHECK(km_input_vt_feed(input, final_chunk + consumed,
                           sizeof(final_chunk) - consumed, true, &consumed,
                           &event, &ready, &error) == KM_OK);
    CHECK(consumed == 1 && ready && event.codepoint == 'b');
    CHECK(km_input_vt_feed(input, NULL, 0, true, &consumed, &event,
                           &ready, &error) == KM_OK);
    CHECK(ready && event.kind == KM_EVENT_EOF);
    km_input_vt_destroy(input);

    CHECK(km_input_vt_create(&input, &error) == KM_OK);
    CHECK(km_input_vt_feed(input, osc_escape_bel, sizeof(osc_escape_bel),
                           false, &consumed, &event, &ready, &error) == KM_OK);
    CHECK(consumed == sizeof(osc_escape_bel) && ready &&
          event.codepoint == 'Z');
    km_input_vt_destroy(input);

    CHECK(km_input_vt_create(&input, &error) == KM_OK);
    CHECK(km_input_vt_feed(input, overlong, sizeof(overlong), false,
                           &consumed, &event, &ready, &error) == KM_OK);
    CHECK(consumed == sizeof(overlong) && ready &&
          event.codepoint == KM_KEY_ESCAPE);
    CHECK(!km_input_vt_wants_timeout(input));
    CHECK(km_input_vt_timeout(input, &event, &ready, &error) == KM_OK);
    CHECK(!ready);
    CHECK(km_input_vt_feed(input, discard_end, sizeof(discard_end), false,
                           &consumed, &event, &ready, &error) == KM_OK);
    CHECK(consumed == sizeof(discard_end) && ready && event.codepoint == 'Q');
    km_input_vt_destroy(input);
}

static void test_paste_errors(void)
{
    static const uint8_t start[] = {0x1b, '[', '2', '0', '0', '~'};
    static const uint8_t end[] = {0x1b, '[', '2', '0', '1', '~'};
    size_t payload;
    size_t len;
    uint8_t *bytes;
    KmInputVt *input = NULL;
    KmError error;
    size_t offset = 0;
    KmStatus status = KM_OK;

    if (km_config_max_paste_bytes() > 16u * 1024u * 1024u) return;
    payload = km_config_max_paste_bytes() + 1u;
    len = sizeof(start) + payload + sizeof(end);
    bytes = (uint8_t *)malloc(len);
    CHECK(bytes != NULL);
    memcpy(bytes, start, sizeof(start));
    memset(bytes + sizeof(start), 'x', payload);
    memcpy(bytes + sizeof(start) + payload, end, sizeof(end));
    CHECK(km_input_vt_create(&input, &error) == KM_OK);
    while (offset < len && status == KM_OK) {
        KmEvent event;
        size_t consumed = 0;
        size_t count = len - offset < 4096 ? len - offset : 4096;
        bool ready = false;

        status = km_input_vt_feed(input, bytes + offset, count, false,
                                  &consumed, &event, &ready, &error);
        offset += consumed;
        CHECK(!ready);
    }
    CHECK(status == KM_ERR_INVALID && offset == len);
    CHECK(error.operation != NULL &&
          strstr(error.operation, "configured limit") != NULL);
    {
        KmEvent event;
        size_t consumed = 0;
        bool ready = false;
        CHECK(km_input_vt_feed(input, NULL, 0, true, &consumed, &event,
                               &ready, &error) == KM_OK);
        CHECK(ready && event.kind == KM_EVENT_EOF);
    }
    km_input_vt_destroy(input);
    free(bytes);

    CHECK(km_input_vt_create(&input, &error) == KM_OK);
    {
        static const uint8_t incomplete[] = {
            0x1b, '[', '2', '0', '0', '~', 'x',
        };
        KmEvent event;
        size_t consumed = 0;
        bool ready = false;

        CHECK(km_input_vt_feed(input, incomplete, sizeof(incomplete), false,
                               &consumed, &event, &ready, &error) == KM_OK);
        CHECK(consumed == sizeof(incomplete) && !ready);
        CHECK(km_input_vt_feed(input, NULL, 0, true, &consumed, &event,
                               &ready, &error) == KM_ERR_INVALID);
        CHECK(error.operation != NULL &&
              strstr(error.operation, "incomplete delimiter") != NULL);
        CHECK(km_input_vt_feed(input, NULL, 0, false, &consumed, &event,
                               &ready, &error) == KM_OK);
        CHECK(ready && event.kind == KM_EVENT_EOF);
    }
    km_input_vt_destroy(input);
}

int main(void)
{
    test_chunk_independence();
    test_timeout();
    test_eof_and_discard_edges();
    test_paste_errors();
    puts("input VT tests passed");
    return 0;
}
