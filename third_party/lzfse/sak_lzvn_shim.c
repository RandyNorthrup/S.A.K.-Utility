/*
 * Minimal C shim over the vendored Apple lzfse/lzvn reference library so the
 * C++ HFS+ decmpfs codec can call stable, allocation-managed entry points
 * without including the library's internal headers.
 */

#include <stdlib.h>
#include <string.h>

#include "lzfse.h"
#include "lzfse_internal.h"
#include "lzvn_decode_base.h"
#include "lzvn_encode_base.h"

size_t sak_lzvn_encode(void* dst, size_t dst_size, const void* src, size_t src_size) {
    void* work = malloc(LZVN_ENCODE_WORK_SIZE);
    if (work == NULL) {
        return 0;
    }
    const size_t written = lzvn_encode_buffer(dst, dst_size, src, src_size, work);
    free(work);
    return written;
}

size_t sak_lzvn_decode(void* dst, size_t dst_size, const void* src, size_t src_size) {
    lzvn_decoder_state state;
    memset(&state, 0, sizeof state);
    state.src = (const unsigned char*)src;
    state.src_end = (const unsigned char*)src + src_size;
    state.dst = (unsigned char*)dst;
    state.dst_begin = (unsigned char*)dst;
    state.dst_end = (unsigned char*)dst + dst_size;
    state.dst_current = (unsigned char*)dst;
    lzvn_decode(&state);
    return (size_t)(state.dst - (unsigned char*)dst);
}

size_t sak_lzfse_encode(void* dst, size_t dst_size, const void* src, size_t src_size) {
    void* scratch = malloc(lzfse_encode_scratch_size());
    if (scratch == NULL) {
        return 0;
    }
    const size_t written = lzfse_encode_buffer(dst, dst_size, src, src_size, scratch);
    free(scratch);
    return written;
}

size_t sak_lzfse_decode(void* dst, size_t dst_size, const void* src, size_t src_size) {
    void* scratch = malloc(lzfse_decode_scratch_size());
    if (scratch == NULL) {
        return 0;
    }
    const size_t written = lzfse_decode_buffer(dst, dst_size, src, src_size, scratch);
    free(scratch);
    return written;
}
