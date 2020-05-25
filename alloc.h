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

#pragma once

/* Internal allocator implementation. This could be on a cpp file, but for
 * performance reasons it is a single-include file. The using program or
 * library should include this file and provide wrppers around the internal
 * alloc interface (do_alloc, do_dealloc, chunk_size, valid_chunk). This
 * organization allows multiple users (e.g., libplsalloc and the simulator).
 */

//#include <cstdlib>
//#include <string.h>
//#include <errno.h>
//#include <malloc.h>
#include <sys/mman.h>
#include <tuple>
//#include "swarm/hooks.h"
#include "common.h"
#include "central_free_list.h"
#include "large_heap.h"
#include "mutex.h"

namespace plsalloc {

#undef DEBUG
#define DEBUG(args...) //info(args)

// Set to 0 if you like serial code and lots of lock spinning
#define USE_THREADCACHE 1

// Enables bulk allocations from central freelists, which reduce contention on
// the central freeList
#define BULK_ALLOC 1

// Set to >1 to use banked central freelists, which reduce lock contention but
// take extra capacity
#define CENTRAL_FREE_LIST_BANKS 1

/* Layout */

// FIXME: Using char* const instead of constexpr due to lack of int -> ptr
// casts with constexpr. This may cause accesses to TRACKED memory with
// unoptimized builds (with -O3, everything will be constant-propagated).
// See https://stackoverflow.com/a/10376574

static char* const trackedBase  = (char*) PLSALLOC_TRACKED_BASEADDR;
// FIXME: either do bounds checking to ensure trackedBump and trackedEnd don't
// go past trackedBound, or get rid of this unused constant:
//static char* const trackedBound = (char*) 0x0b0000000000ul;

static char* const untrackedBase  = (char*) PLSALLOC_UNTRACKED_BASEADDR;
// FIXME: either do bounds checking to ensure sizemapBump and sizemapEnd don't
// go past untrackedBound, or get rid of this unused constant:
//static char* const untrackedBound = (char*) 0x0c0000000000ul;

/* Global data */

// A page is the minimum amount of space devoted to fixed-size elements
static constexpr size_t kPageBits = 15;  // 32 KB
static constexpr size_t kPageSize = 1ul << kPageBits;
static inline size_t sizeToPages(size_t sz) { return (sz + kPageSize - 1) >> kPageBits; }

// Use 256 freelists, with sizes 64 bytes - 16 KB in 64-byte increments.
static constexpr size_t kMaxClasses = 256;
static inline size_t sizeToClass(size_t sz) { return (sz + 63ul) >> 6ul; }
static inline size_t classToSize(size_t cl) { return cl << 6ul; }
static inline bool isLargeAlloc(size_t sz) { return sizeToClass(sz) >= kMaxClasses; }

// Pin supports 2048 threads tops
static constexpr uint32_t kMaxThreads = 2048;

// A thread cache that grows beyond this limit will donate to the central freelists
static constexpr size_t kMaxThreadCacheSize = 4096 * 1024;

// Thread caches try to fetch this much data per central list access
static constexpr size_t kFetchTargetSize = 32 * 1024;

class ThreadCache {
    private:
        size_t cacheSize;
        BlockedDeque<void*> classLists[kMaxClasses];

    public:
        ThreadCache() : cacheSize(0) {}
        inline void* alloc(size_t cl);
        inline void dealloc(void* p, size_t cl);
        inline size_t size(size_t cl) { return classLists[cl].size(); }
} ATTR_LINE_ALIGNED;

#if CENTRAL_FREE_LIST_BANKS <= 1
typedef CentralFreeList CentralFreeListType;
#else
typedef BankedCentralFreeList<CENTRAL_FREE_LIST_BANKS> CentralFreeListType;
#endif

// All globals go here, so we can allocate them in untracked memory
struct AllocState {
    CentralFreeListType classLists[kMaxClasses];
    LargeHeap largeHeap;

#if USE_THREADCACHE
    ThreadCache threadCaches[kMaxThreads];
#endif

    char* volatile trackedBump;  // volatile b/c used unlocked for valid checks
    char* trackedEnd;

    char* sizemapBump;
    char* sizemapEnd;

    mutex sysAllocLock ATTR_LINE_ALIGNED;
} ATTR_LINE_ALIGNED;

// Both AllocState and the sizemap have fixed locations in untracked mem
static AllocState& gs = *((AllocState*)untrackedBase);
static uint8_t* const sizemap = (uint8_t*) (untrackedBase + sizeof(AllocState));

/* Initialization (delicate...) */

// Since the loader calls initialization routines in whatever order it wants,
// this variable ensures we initialize before the first alloc, by having
// do_alloc call init. It'd be nice (and more importantly avoid the drag on all
// do_allocs) if we could specify that this is the very first init function to
// be called, but I've tried a bunch of things unsuccessfully (constructor
// priorities, linker flags, __malloc_initialization_hook, ...).
static bool __initialized = false;
// [victory] This constructor attribute somehow caused a crash on Ubuntu 18.04.
//__attribute__((constructor (101)))
void __plsalloc_init() {
    // [mcj] This DEBUG causes a segmentation fault
    //DEBUG("init start");
    if (__initialized) return;

    size_t sz = ((sizeof(AllocState) + 2 * 1024 * 1024) >> 21) << 21;
    void* mem = mmap(untrackedBase, sz, (PROT_READ|PROT_WRITE), (MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED), -1, 0);
    if (!mem) exit(183);  // we don't even have libstdc++ here... just die with a hopefully unique exit code

    // At this point, gs exists but is not initialized...
    gs.trackedBump = trackedBase;
    gs.trackedEnd = trackedBase;

    gs.sizemapBump = (char*) sizemap;
    gs.sizemapEnd = untrackedBase + sz;

    // NOTE: Placement new is OK here because these classes don't call alloc
    // internally. Keep it that way!
    for (size_t cl = 1; cl < kMaxClasses; cl++) {
        uint32_t elemsPerFetch = kFetchTargetSize / classToSize(cl);
        elemsPerFetch = std::min((uint32_t)DQBLOCK_SIZE, std::max(elemsPerFetch, 2u));
        new (&gs.classLists[cl]) CentralFreeListType(classToSize(cl), elemsPerFetch);
    }
    new (&gs.largeHeap) LargeHeap();

#if USE_THREADCACHE
    for (uint32_t tid = 0; tid < kMaxThreads; tid++) {
        new (&gs.threadCaches[tid]) ThreadCache();
    }
#endif

    new (&gs.sysAllocLock) mutex();

    __initialized = true;
    //DEBUG("init done %ld", sz);
}

/* System alloc and sizemap management */

static std::tuple<char*, char*> sysAlloc(size_t chunkSize) {
    size_t minPages = sizeToPages(chunkSize);
    // To reduce freelist fragmentation and reduce the number of calls to the
    // allocator, give out 32 pages at once (32*32KB*256 = 256MB overage in the
    // worst case, i.e. all freelists used and they use only one element)
    size_t pages = std::max(32ul, minPages);
    size_t allocSize = pages << kPageBits;
    assert(allocSize >= chunkSize);

    auto allocContiguous = [](size_t sz, char*& bump, char*& end) {
        char* alloc = bump;
        bump += sz;
        if (bump > end) {
            // mmap at least 2MB (so we can use superpages)
            size_t mmapSz = (((bump - end) >> 21ul) + 1ul) << 21ul;
            void* mem = mmap(end, mmapSz, (PROT_READ|PROT_WRITE), (MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED), -1, 0);
            (void) mem;
            assert(mem);
            end += mmapSz;
            assert(end >= bump);
        }
        return alloc;
    };

    scoped_mutex sm(gs.sysAllocLock);

    // Grab tracked memory
    char* trackedBump = gs.trackedBump;
    char* alloc = allocContiguous(allocSize, trackedBump, gs.trackedEnd);

    // Update trackedBump (which is volatile b/c valid_chunk uses it unlocked)
    gs.trackedBump = trackedBump;
    __sync_synchronize();

    // Grab sizemap memory
    allocContiguous(pages, gs.sizemapBump, gs.sizemapEnd);

    // If it's a small alloc, set sizemap entries to the right class.
    // (no need to initialize anything with large allocs, because large-alloc
    // pages use class 0 and mmap returns zero'd mem)
    if (!isLargeAlloc(chunkSize)) {
        size_t cl = sizeToClass(chunkSize);
        size_t base = (alloc - trackedBase) >> kPageBits;
        for (size_t page = 0; page < pages; page++) {
            sizemap[base + page] = cl;
        }
    }
    return std::make_tuple(alloc, alloc + allocSize);
}

static inline uint8_t chunkToClass(void* p) {
    return sizemap[((char*)p - trackedBase) >> kPageBits];
}

/* Thread cache methods (performance-sensitive) */

void* ThreadCache::alloc(size_t cl) {
#if BULK_ALLOC
    if (unlikely(classLists[cl].empty())) {
        DEBUG("bulkAlloc start class %ld", classToSize(cl));
        gs.classLists[cl].bulkAlloc(classLists[cl]);
        cacheSize += classToSize(cl) * classLists[cl].size();
        DEBUG("bulkAlloc done elems %ld", classLists[cl].size());
    }
    void* res = classLists[cl].dequeue_back();
    cacheSize -= classToSize(cl);
#else
    void* res;
    if (unlikely(classLists[cl].empty())) {
        res = gs.classLists[cl].alloc();
    } else {
        res = classLists[cl].dequeue_back();
        cacheSize -= classToSize(cl);
    }
#endif
    return res;
}

void ThreadCache::dealloc(void* p, size_t cl) {
    classLists[cl].push_back(p);
    cacheSize += classToSize(cl);

    // NOTE: This code takes about 10K cycles to traverse all 256 classLists.
    // It likely blows up the L1. However, this is rare enough that it doesn't
    // matter. I tried remembering the used classes in a bitset to accelerate
    // the process. This brings down the cost of a collection with a single
    // used class from ~11Kcycles to ~2Kcycles. And yet, because thet bitset
    // must be touched by every bulkAlloc() and dealloc() call, it slightly
    // worsens performance in the common case.
    if (unlikely(cacheSize > kMaxThreadCacheSize)) {
        // Donate ~half of our cache to the central freeLists
        DEBUG("TC: Donating, start size %ld", cacheSize);
        for (size_t cl = 1; cl < kMaxClasses; cl++) {
            size_t elems = classLists[cl].size();
            if (!elems) continue;
            assert(elems);
            size_t elemsToDonate = (elems + 1) / 2;
            gs.classLists[cl].bulkDealloc(classLists[cl], elemsToDonate);
            cacheSize -= (elems - classLists[cl].size()) * classToSize(cl);
        }
        DEBUG("TC: Donation done, end size %ld", cacheSize);
    }
}

/* Internal alloc interface. All external functions use only these four.
 * do_dealloc and chunk_size assume the pointer is valid. External functions
 * must first check this with valid_chunk.
 */

static inline void* do_alloc(size_t chunkSize) {
    DEBUG("do_alloc(%ld) large=%d", chunkSize, isLargeAlloc(chunkSize));
    // Ensure initialization if other inits (constructors) fire before ours
    if (unlikely(!__initialized)) __plsalloc_init();
    void* res;
    if (likely(!isLargeAlloc(chunkSize))) {
        size_t cl = sizeToClass(chunkSize);
#if USE_THREADCACHE
        uint64_t tid = sim_get_tid();
        DEBUG("do_alloc cl %ld tid %ld sz %ld",
              cl, tid, gs.threadCaches[tid].size(cl));
        res = gs.threadCaches[tid].alloc(cl);
#else
        res = gs.classLists[cl].alloc();
#endif
    } else {
        size_t sz = (chunkSize + 63ul) & (~63ul);  // round to cache line size
        res = gs.largeHeap.alloc(sz);
    }
    DEBUG("do_alloc(%ld) -> %p", chunkSize, res);
    return res;
}

static inline void do_dealloc(void* p) {
    DEBUG("do_dealloc(%p)", p);
    if (!p) return;
    uint8_t cl = chunkToClass(p);
    if (cl) {
#if USE_THREADCACHE
        uint64_t tid = sim_get_tid();
        DEBUG("do_dealloc cl %d tid %ld sz %ld",
              cl, tid, gs.threadCaches[tid].size(cl));
        gs.threadCaches[tid].dealloc(p, cl);
#else
        gs.classLists[cl].dealloc(p);
#endif
    } else {
        // largeHeap-managed chunks have class 0
        gs.largeHeap.dealloc(p);
    }
}

static inline size_t chunk_size(void* p) {
    uint8_t cl = chunkToClass(p);
    return cl ? classToSize(cl) : gs.largeHeap.chunkToSize_noassert(p);
}

static inline bool valid_chunk(void* p) {
    char* ptr = (char*) p;
    return (ptr >= trackedBase) && (ptr <= gs.trackedBump);
}

};  // namespace plsalloc
