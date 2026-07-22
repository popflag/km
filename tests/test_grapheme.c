#include "unicode.h"

#include <errno.h>
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

static size_t append_utf8(uint8_t *text, size_t len, size_t cap,
                          unsigned long codepoint)
{
    size_t count;

    if (codepoint <= 0x7f) {
        count = 1;
        CHECK(len <= cap - count);
        text[len] = (uint8_t)codepoint;
    } else if (codepoint <= 0x7ff) {
        count = 2;
        CHECK(len <= cap - count);
        text[len] = (uint8_t)(0xc0u | (codepoint >> 6));
        text[len + 1] = (uint8_t)(0x80u | (codepoint & 0x3fu));
    } else if (codepoint <= 0xffff) {
        count = 3;
        CHECK(len <= cap - count);
        text[len] = (uint8_t)(0xe0u | (codepoint >> 12));
        text[len + 1] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu));
        text[len + 2] = (uint8_t)(0x80u | (codepoint & 0x3fu));
    } else {
        count = 4;
        CHECK(codepoint <= 0x10ffff && len <= cap - count);
        text[len] = (uint8_t)(0xf0u | (codepoint >> 18));
        text[len + 1] = (uint8_t)(0x80u | ((codepoint >> 12) & 0x3fu));
        text[len + 2] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu));
        text[len + 3] = (uint8_t)(0x80u | (codepoint & 0x3fu));
    }
    return len + count;
}

static void check_case(char *line, size_t source_line, size_t *case_count)
{
    uint8_t text[1024];
    size_t boundaries[256];
    size_t len = 0;
    size_t boundary_count = 0;
    char *token = strtok(line, " \t\r\n");

    if (token == NULL || token[0] == '#') return;
    for (;;) {
        bool boundary = strcmp(token, "\xc3\xb7") == 0;
        char *end;
        unsigned long codepoint;

        CHECK(boundary || strcmp(token, "\xc3\x97") == 0);
        token = strtok(NULL, " \t\r\n");
        if (token == NULL || token[0] == '#') {
            CHECK(boundary && boundary_count < 256);
            boundaries[boundary_count++] = len;
            break;
        }
        if (boundary) {
            CHECK(boundary_count < 256);
            boundaries[boundary_count++] = len;
        }
        errno = 0;
        codepoint = strtoul(token, &end, 16);
        CHECK(errno == 0 && end != token && *end == '\0');
        CHECK(codepoint < 0xd800 || codepoint > 0xdfff);
        len = append_utf8(text, len, sizeof(text), codepoint);
        token = strtok(NULL, " \t\r\n");
        CHECK(token != NULL);
    }
    CHECK(boundary_count >= 2 && boundaries[0] == 0 &&
          boundaries[boundary_count - 1] == len);
    for (size_t i = 0; i + 1 < boundary_count; ++i) {
        KmGrapheme grapheme;
        KmError error;
        KmStatus status = km_unicode_next_grapheme(
            text, len, boundaries[i], &grapheme, &error);

        if (status != KM_OK || grapheme.end != boundaries[i + 1]) {
            fprintf(stderr,
                    "GraphemeBreakTest line %zu: offset %zu ended at %zu, "
                    "expected %zu\n",
                    source_line, boundaries[i],
                    status == KM_OK ? grapheme.end : SIZE_MAX,
                    boundaries[i + 1]);
            exit(1);
        }
    }
    ++*case_count;
}

static void test_regional_indicator_internal_starts(void)
{
    static const uint8_t text[] = {
        0xf0, 0x9f, 0x87, 0xa6, 0xf0, 0x9f, 0x87, 0xa7,
        0xf0, 0x9f, 0x87, 0xa8, 0xf0, 0x9f, 0x87, 0xa9,
        0xf0, 0x9f, 0x87, 0xaa,
    };
    static const size_t expected[] = {8, 8, 16, 16, 20};

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        KmGrapheme grapheme;
        KmError error;
        CHECK(km_unicode_next_grapheme(text, sizeof(text), i * 4, &grapheme,
                                       &error) == KM_OK);
        CHECK(grapheme.end == expected[i]);
    }
}

int main(void)
{
    FILE *file = fopen("tests/data/GraphemeBreakTest.txt", "rb");
    char line[1024];
    size_t source_line = 0;
    size_t case_count = 0;

    CHECK(file != NULL);
    while (fgets(line, sizeof(line), file) != NULL) {
        ++source_line;
        CHECK(strchr(line, '\n') != NULL || feof(file));
        check_case(line, source_line, &case_count);
    }
    CHECK(!ferror(file));
    CHECK(fclose(file) == 0);
    CHECK(case_count > 700);
    test_regional_indicator_internal_starts();
    printf("grapheme tests passed (%zu Unicode cases)\n", case_count);
    return 0;
}
