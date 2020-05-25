/** $lic$
 * Copyright (C) 2017-2020 by Massachusetts Institute of Technology
 *
 * This file is part of plsalloc.
 *
 * plsalloc is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * plsalloc was developed as part of the Swarm architecture project. If you use
 * this software in your research, we request that you reference the Swarm
 * MICRO 2018 paper ("Harmonizing Speculative and Non-Speculative Execution in
 * Architectures for Ordered Parallelism", Jeffrey et al., MICRO-51, 2018) as
 * the source of plsalloc in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * plsalloc is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* malloc interface around plsalloc */

#include <cstdlib>
#include <string.h>
#include <errno.h>
#include <malloc.h>
//#include <sys/mman.h>
#include <tuple>
#include "swarm/hooks.h"

/* Layout (NOTE: Keep in sync with simulator tracked/untracked segments) */
// 512 GB tracked + 512GB untracked (leave first 512GB of each segment to sim)
#define PLSALLOC_TRACKED_BASEADDR (0x0a8000000000ul)
#define PLSALLOC_UNTRACKED_BASEADDR (0x0b8000000000ul)

#include "alloc.h"

/* Helper methods for abort / commit handlers (all of which call dealloc) */

static void dealloc_task(uint64_t ts, void* p) { plsalloc::do_dealloc(p); }

template <bool onAbort>
static void enqueue_dealloc(void* ptr) {
    // We can't import the Swarm API and swarm::enqueue because we're in a fairly
    // restricted environment (e.g., we implement the malloc/free that
    // memTupleRunners use, and don't want to acquire a dep on libswarm). So do a
    // raw enqueue.
    uint64_t numArgs = 1;
    uint64_t hintFlags = EnqFlags::SAMEHINT | EnqFlags::CANTSPEC | EnqFlags::NOTIMESTAMP;
    if (onAbort) hintFlags |= EnqFlags::RUNONABORT;
    uint64_t magicOp = (MAGIC_OP_TASK_ENQUEUE_BEGIN + numArgs) | hintFlags;
    sim_magic_op_2(magicOp, reinterpret_cast<uint64_t>(ptr), reinterpret_cast<uint64_t>(&dealloc_task));
}

static void on_abort_dealloc(void* ptr) {
    if (sim_priv_isdoomed()) plsalloc::do_dealloc(ptr);
    else enqueue_dealloc<true>(ptr);
}

static void on_commit_dealloc(void* ptr) {
    if (sim_isirrevocable()) plsalloc::do_dealloc(ptr);
    else enqueue_dealloc<false>(ptr);
}

/* External malloc interface */

void* malloc(size_t size) {
    if (unlikely(!size)) return nullptr;
    sim_priv_call();
    void* p = plsalloc::do_alloc(size);
    on_abort_dealloc(p);
    sim_priv_ret();
    return p;
}

void* calloc(size_t nmemb, size_t size) {
    size_t sz = nmemb * size;
    if (unlikely(!sz)) return nullptr;
    sim_priv_call();
    void* p = plsalloc::do_alloc(sz);
    on_abort_dealloc(p);
    sim_priv_ret();
    memset(p, 0, sz);
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);

    sim_priv_call();
    if (plsalloc::valid_chunk(ptr)) {
        if (!size) {
            sim_priv_ret();
            on_commit_dealloc(ptr);
            return nullptr;
        }

        size_t chunkSize = plsalloc::chunk_size(ptr);
        // If it fits and we're not wasting too much space, do nothing
        if (chunkSize >= size && chunkSize/2 <= size) return ptr;

        void* newPtr = plsalloc::do_alloc(size);
        on_abort_dealloc(newPtr);
        sim_priv_ret();
        memcpy(newPtr, ptr, (size < chunkSize) ? size : chunkSize);
        on_commit_dealloc(ptr);
        return newPtr;
    } else {
        sim_priv_ret();
        // This is an invalid chunk, so take an exception
        sim_serialize();
        std::abort();
    }
}

void free(void* ptr) {
    if (!ptr) return;
    on_commit_dealloc(ptr);
}

void cfree(void* ptr) { free(ptr); }

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    // NOTE: On failure, posix_memalign doesn't modify memptr
    if (size == 0) {
        *memptr = nullptr;
    } else if (!alignment || (alignment & (alignment - 1))
               || (alignment % sizeof(void*))) {
        return EINVAL;
    } else {
        // FIXME(dsm): Cache-line aligned for now
        void* ptr = malloc(size);
        if (!ptr) return ENOMEM;
        *memptr = ptr;
    }
    return 0;
}

void* aligned_alloc(size_t alignment, size_t size) {
    void* ptr;
    int dc = posix_memalign(&ptr, alignment, size);
    if (dc != 0) ptr = nullptr;
    return ptr;
}

void* memalign(size_t alignment, size_t size) {
    return aligned_alloc(alignment, size);
}

// The version of <string.h> header we have installed declares
// the argument of strdup to be non-null. Perhaps we should
// follow the standard and assert/assume src is non-null?
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wtautological-pointer-compare"
#elif defined(__GNUC__) && (__GNUC__ >= 6)
#pragma GCC diagnostic ignored "-Wnonnull-compare"
#endif
char* strdup(const char* src) {
    if (src == nullptr) return nullptr;
    size_t len = strlen(src);
    char* dst = (char*)malloc(len);
    memcpy(dst, src, len);
    return dst;
}
#pragma GCC diagnostic pop

size_t malloc_usable_size(void* ptr) {
    sim_priv_call();
    if (plsalloc::valid_chunk(ptr)) {
        size_t chunkSz = plsalloc::chunk_size(ptr);
        sim_priv_ret();
        return chunkSz;
    } else {
        sim_priv_ret();
        // Invalid chunk, take an exception
        sim_serialize();
        std::abort();
        return 0;
    }
}

/* Unimplemented functions below. Programs rarely use these, so rather than
 * implementing the library in full, we do these on demand */

static void abort_unimplemented(const char* fn) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "Aborting: sim-alloc function unimplemented: %s", fn);
    sim_magic_op_1(MAGIC_OP_WRITE_STD_OUT, reinterpret_cast<uint64_t>(&buf[0]));
    std::abort();
}

void* valloc(size_t size) {
    abort_unimplemented(__FUNCTION__);
    return nullptr;
}

void* pvalloc(size_t size) {
    abort_unimplemented(__FUNCTION__);
    return nullptr;
}

void* malloc_get_state(void) {
    abort_unimplemented(__FUNCTION__);
    return nullptr;
}

int malloc_set_state(void*) {
    abort_unimplemented(__FUNCTION__);
    return -1;
}

int malloc_info(int, FILE*) {
    abort_unimplemented(__FUNCTION__);
    return -1;
}

void malloc_stats(void) {
    abort_unimplemented(__FUNCTION__);
}

int malloc_trim(size_t) {
    abort_unimplemented(__FUNCTION__);
    return -1;
}

// http://www.gnu.org/software/libc/manual/html_node/Hooks-for-Malloc.html
// __malloc_hook
// __malloc_initialize_hook
// __free_hook
