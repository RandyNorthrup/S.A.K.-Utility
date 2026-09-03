/*
 * Minimal C shim over the vendored Apple lzfse/lzvn reference library so the
 * C++ HFS+ decmpfs codec can call stable, allocation-managed entry points
 * without including the library's internal headers.
 */

#include "lzfse.h"
#include "lzfse_internal.h"
#include "lzvn_decode_base.h"
#include "lzvn_encode_base.h"

#include <stdlib.h>
#include <string.h>

size_t sakLzvnEncode(void* dst, size_t dst_size, const void* src, size_t src_size) {
    void* work = malloc(LZVN_ENCODE_WORK_SIZE);
    if (work == NULL) {
        return 0;
    }
    const size_t written = lzvn_encode_buffer(dst, dst_size, src, src_size, work);
    free(work);
    return written;
}

size_t sakLzvnDecode(void* dst, size_t dst_size, const void* src, size_t src_size) {
    lzvn_decoder_state state;
    const unsigned char* src_bytes = src;
    unsigned char* dst_bytes = dst;
    memset(&state, 0, sizeof state);
    state.src = src_bytes;
    state.src_end = src_bytes + src_size;
    state.dst = dst_bytes;
    state.dst_begin = dst_bytes;
    state.dst_end = dst_bytes + dst_size;
    state.dst_current = dst_bytes;
    lzvn_decode(&state);
    return state.dst - dst_bytes;
}

size_t sakLzfseEncode(void* dst, size_t dst_size, const void* src, size_t src_size) {
    void* scratch = malloc(lzfse_encode_scratch_size());
    if (scratch == NULL) {
        return 0;
    }
    const size_t written = lzfse_encode_buffer(dst, dst_size, src, src_size, scratch);
    free(scratch);
    return written;
}

size_t sakLzfseDecode(void* dst, size_t dst_size, const void* src, size_t src_size) {
    void* scratch = malloc(lzfse_decode_scratch_size());
    if (scratch == NULL) {
        return 0;
    }
    const size_t written = lzfse_decode_buffer(dst, dst_size, src, src_size, scratch);
    free(scratch);
    return written;
}
