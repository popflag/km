#include "base.h"

void km_error_clear(KmError *error) {
    if (error != NULL) {
        error->code = KM_OK;
        error->os_code = 0;
        error->operation = NULL;
    }
}

const char *km_status_string(KmStatus status) {
    switch (status) {
    case KM_OK: return "ok";
    case KM_ERR_OOM: return "out of memory";
    case KM_ERR_INVALID: return "invalid argument";
    case KM_ERR_IO: return "I/O error";
    case KM_ERR_PERMISSION: return "permission denied";
    case KM_ERR_CONFLICT: return "revision conflict";
    case KM_ERR_UNSUPPORTED: return "unsupported";
    case KM_ERR_CANCELLED: return "cancelled";
    }
    return "unknown status";
}
