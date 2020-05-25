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

#include "common.h"
#include "blocked_deque.h"
#include "mutex.h"

#define CFDEBUG(args...) //info(args)

namespace plsalloc {

class CentralFreeList {
  private:
    // dsm: Use uint32_t so everything fits in one line
    const uint32_t chunkSize;
    const uint32_t elemsPerFetch;
    BlockedDeque<void*> freeChunks;
    char* bumpStart;
    char* bumpEnd;
    mutex lock;

  public:
    CentralFreeList(uint32_t _chunkSize, uint32_t _elemsPerFetch)
        : chunkSize(_chunkSize), elemsPerFetch(_elemsPerFetch),
          bumpStart(nullptr), bumpEnd(nullptr) {}

    CentralFreeList() : CentralFreeList(0, 0) {}

    void* alloc() {
        scoped_mutex sm(lock);
        if (!freeChunks.empty()) return freeChunks.dequeue_back();
        if (unlikely(bumpStart + chunkSize > bumpEnd))
            std::tie(bumpStart, bumpEnd) = sysAlloc(chunkSize);
        void* res = bumpStart;
        bumpStart += chunkSize;
        assert(bumpStart <= bumpEnd);
        return res;
    }

    void dealloc(void* p) {
        scoped_mutex sm(lock);
        freeChunks.push_back(p);
    }

    void bulkAlloc(BlockedDeque<void*>& __restrict__ dstList) {
        lock.lock();
        CFDEBUG("bulkAlloc start cs %d  ef %d  fcs %ld", chunkSize, elemsPerFetch,
             freeChunks.size());

        // Grab from freeChunks ONLY if you can satisfy the whole allocation.
        // Otherwise, let freeChunks grow from deallocs first.
        if (freeChunks.size() >= elemsPerFetch) {
            if (elemsPerFetch >= DQBLOCK_SIZE) {
                CFDEBUG("CF: Moving full block");
                freeChunks.steal_front(dstList);
            } else {
                for (uint32_t i = 0; i < elemsPerFetch; i++) {
                    dstList.push_back(freeChunks.dequeue_back());
                }
            }
            lock.unlock();
            return;
        }

        // Fallthrough path. For simplicity, allocate either from bump or
        // system. If bump doesn't have enough elements, don't satisfy the
        // entire allocation (this is rare and simplifies code).
        if (bumpStart + chunkSize <= bumpEnd) {
            CFDEBUG("CF: Bump-pointer alloc");
        } else {
            CFDEBUG("CF: Sys alloc");
            std::tie(bumpStart, bumpEnd) = sysAlloc(chunkSize);
        }
        char* start = bumpStart;
        char* end = bumpEnd;
        bumpStart = start + chunkSize * elemsPerFetch;
        lock.unlock();  // unlock early, no need to wait to fill dstList

        if (end - start > chunkSize * elemsPerFetch) {
            end = start + chunkSize * elemsPerFetch;
        } else {
            size_t availElems = (end - start) / chunkSize;
            end = start + chunkSize * availElems;
        }

        for (char* cur = start; cur < end; cur += chunkSize) {
            dstList.push_back(cur);
        }
    }

    void bulkDealloc(BlockedDeque<void*>& __restrict__ srcList, size_t elems) {
        CFDEBUG("bulkDealloc start cs %d el %ld ssz %ld", chunkSize, elems,
                srcList.size());
        if (elems >= DQBLOCK_SIZE) {
            // Move entire blocks front-to-front (fronts are always aligned)
            // NOTE(dsm): We splice source list outside the critical section
            size_t blocks = elems / DQBLOCK_SIZE;
            auto spliced = srcList.splice_front(blocks);
            CFDEBUG("bulkDealloc moving %ld full blocks", blocks);
            lock.lock();
            freeChunks.merge_front(spliced);
            lock.unlock();
        } else {
            // Move single elems back-to-back
            CFDEBUG("bulkDealloc moving single elems");
            lock.lock();
            while (elems--) {
                freeChunks.push_back(srcList.dequeue_back());
            }
            lock.unlock();
        }
        CFDEBUG("bulkDealloc done");
    }
} ATTR_LINE_ALIGNED;

template <size_t NB> class BankedCentralFreeList {
    private:
        CentralFreeList banks[NB];
        inline size_t rb() {
            uint64_t randVal;
            sim_rdrand(&randVal);
            return randVal % NB;
        }

    public:
        BankedCentralFreeList(uint32_t _chunkSize, uint32_t _elemsPerFetch) {
            for (size_t b = 0; b < NB; b++)
                new (&banks[b]) CentralFreeList(_chunkSize, _elemsPerFetch);
        }

        inline void* alloc() { return banks[rb()].alloc(); }
        inline void dealloc(void* p) { banks[rb()].dealloc(); }

        inline void bulkAlloc(BlockedDeque<void*>& __restrict__ dstList) {
            banks[rb()].bulkAlloc(dstList);
        }

        inline void bulkDealloc(BlockedDeque<void*>& __restrict__ srcList, size_t elems) {
            banks[rb()].bulkDealloc(srcList, elems);
        }
};

};
