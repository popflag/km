#ifndef KM_FILE_H
#define KM_FILE_H

#include "document.h"

typedef struct KmPath KmPath;
typedef struct KmFile KmFile;
typedef struct {
    char **items;
    size_t count;
    char *common;
} KmPathCompletions;

KmStatus km_path_from_command_line(int argc, char **argv, KmPath **out_path,
                                   KmError *error);
KmStatus km_path_from_utf8(const char *path, KmPath **out_path, KmError *error);
/* Returns an allocated longest-prefix completion, or NULL when none match. */
KmStatus km_path_complete_utf8(const char *prefix, char **out_completion,
                               KmError *error);
KmStatus km_path_completions_utf8(const char *prefix,
                                  KmPathCompletions *out_completions,
                                  KmError *error);
void km_path_completions_destroy(KmPathCompletions *completions);
void km_path_destroy(KmPath *path);

/* km_file_load takes ownership of path whether it succeeds or fails. */
KmStatus km_file_load(KmPath *path, KmFile **out_file, uint8_t **out_text,
                      size_t *out_len, KmError *error);
void km_file_destroy(KmFile *file);
const char *km_file_display_name(const KmFile *file);
/* Existing targets compare file identity; missing targets compare path spelling. */
bool km_file_same_target(const KmFile *left, const KmFile *right);
KmStatus km_file_save(KmFile *file, const KmDocument *document,
                      KmError *error);

#endif
