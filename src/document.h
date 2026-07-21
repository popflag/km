#ifndef KM_DOCUMENT_H
#define KM_DOCUMENT_H

#include "base.h"

#include <stdbool.h>

typedef struct KmDocument KmDocument;
typedef struct KmAnchor KmAnchor;

typedef struct {
    const KmDocument *document;
    KmRevision revision;
    KmBytePos position;
} KmTextIter;

typedef enum {
    KM_ANCHOR_BEFORE = 0,
    KM_ANCHOR_AFTER
} KmAnchorAffinity;

typedef struct {
    KmBytePos start;
    KmBytePos end;
    const uint8_t *insert;
    size_t insert_len;
    uint32_t ordinal;
} KmSplice;

typedef struct {
    KmRevision expected_revision;
    uint64_t command_id;
} KmTxnMeta;

KmStatus km_document_create(const uint8_t *text, size_t len,
                            KmDocument **out_document, KmError *error);
void km_document_destroy(KmDocument *document);

size_t km_document_len(const KmDocument *document);
KmRevision km_document_revision(const KmDocument *document);
KmStateId km_document_history_state(const KmDocument *document);
bool km_document_is_boundary(const KmDocument *document, KmBytePos position);
KmStatus km_document_copy(const KmDocument *document, KmBytePos start,
                          size_t len, uint8_t *destination, KmError *error);
KmStatus km_document_iter_init(const KmDocument *document, KmBytePos start,
                               KmTextIter *out_iterator, KmError *error);
KmStatus km_text_iter_read(KmTextIter *iterator, uint8_t *destination,
                           size_t capacity, size_t *out_len, bool *out_eof,
                           KmError *error);

KmStatus km_anchor_create(KmDocument *document, KmBytePos position,
                          KmAnchorAffinity affinity, KmAnchor **out_anchor,
                          KmError *error);
void km_anchor_destroy(KmAnchor *anchor);
KmBytePos km_anchor_get(const KmAnchor *anchor);
KmAnchorAffinity km_anchor_affinity(const KmAnchor *anchor);
KmAnchorId km_anchor_id(const KmAnchor *anchor);
KmStatus km_anchor_set(KmAnchor *anchor, KmBytePos position, KmError *error);

KmStatus km_document_apply(KmDocument *document, const KmSplice *splices,
                           size_t count, KmTxnMeta meta, KmError *error);
bool km_document_can_undo(const KmDocument *document);
bool km_document_can_redo(const KmDocument *document);
bool km_document_can_undo_in_range(const KmDocument *document,
                                   KmBytePos start, KmBytePos end);
bool km_document_can_redo_in_range(const KmDocument *document,
                                   KmBytePos start, KmBytePos end);
KmStatus km_document_undo(KmDocument *document,
                          KmRevision expected_revision, KmError *error);
KmStatus km_document_redo(KmDocument *document,
                          KmRevision expected_revision, KmError *error);

#endif
