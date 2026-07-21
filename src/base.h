#ifndef KM_BASE_H
#define KM_BASE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t v;
} KmBytePos;

typedef uint64_t KmRevision;
typedef uint64_t KmStateId;
typedef uint64_t KmAnchorId;

typedef enum {
    KM_OK = 0,
    KM_ERR_OOM,
    KM_ERR_INVALID,
    KM_ERR_IO,
    KM_ERR_PERMISSION,
    KM_ERR_CONFLICT,
    KM_ERR_UNSUPPORTED,
    KM_ERR_CANCELLED
} KmStatus;

typedef struct {
    KmStatus code;
    uint64_t os_code;
    const char *operation;
} KmError;

void km_error_clear(KmError *error);
const char *km_status_string(KmStatus status);

#endif
