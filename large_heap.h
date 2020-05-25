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

#include <map>
#include <unordered_set>
#include "common.h"
#include "mutex.h"
#include "stl_untracked_alloc.h"

/* Manages all large-alloc (class 0) pages. Aims for compact storage and space
 * efficiency by merging blocks aggressively.
 */

#define LHDEBUG(args...) //info(args)

namespace plsalloc {

// TODO: If additional users, move auxiliary STL template defs to their own files.
template <typename K, typename V> class u_map : public std::map<K, V, std::less<K>, StlUntrackedAlloc<std::pair<const K, V> > > {};
template <typename K> class u_unordered_set : public std::unordered_set<K, std::hash<K>, std::equal_to<K>, StlUntrackedAlloc<K> > {};

class LargeHeap {
    private:
        u_map<size_t, u_unordered_set<char*>> freeChunkSets;
        u_map<char*, size_t> chunkSizes;
        mutable mutex lock;

    public:
        void* alloc(size_t chunkSize) {
            scoped_mutex sm(lock);
            // Best-fit allocation
            auto fit = freeChunkSets.lower_bound(chunkSize);
            char* start;
            char* end;

            if (fit == freeChunkSets.end()) {
                LHDEBUG("LH: invoking sysAlloc");
                std::tie(start, end) = sysAlloc(chunkSize);
            } else {
                LHDEBUG("LH: chunkSet[%ld] alloc %ld", fit->first, chunkSize);
                auto& chunkSet = fit->second;
                auto cit = chunkSet.begin();
                start = *cit;
                end = start + fit->first;
                chunkSet.erase(cit);
                if (chunkSet.empty()) freeChunkSets.erase(fit);
            }
            chunkSizes[start] = chunkSize;

            char* left = start + chunkSize;
            size_t remaining = end - left;
            if (remaining) {
                LHDEBUG("LH: remaining %p %ld", left, remaining);
                chunkSizes[left] = remaining;
                unlocked_dealloc(left);
            }
            LHDEBUG("LH: alloc done %p", start);
            return start;
        }

        void dealloc(void* p) {
            scoped_mutex sm(lock);
            unlocked_dealloc(p);
        }

        // The only guarantees we have at this point is that chunk isn't
        // invalid memory, but the task may use a stale pointer that doesn't
        // exist anymore. Return a size of 0 in these cases (and don't trigger
        // an assertion).
        size_t chunkToSize_noassert(void* chunk) const {
            scoped_mutex sm(lock);
            auto it = chunkSizes.find((char*)chunk);
            if (unlikely(it == chunkSizes.end())) return 0;
            return it->second;
        }

    private:
        void unlocked_dealloc(void* p) {
            LHDEBUG("LH: dealloc(%p)", p);
            char* chunk = (char*) p;
            auto it = chunkSizes.find(chunk);
            if (it == chunkSizes.end()) {
                info("ERROR: LargeHeap::dealloc: %p is not a tracked chunk (app code is likely broken)", p);
                std::abort();
            }
            size_t chunkSize = it->second;

            // Try to merge with previous
            if (it != chunkSizes.begin()) {
                auto pit = std::prev(it);
                char* prevChunk = pit->first;
                size_t prevChunkSize = pit->second;

                if (prevChunk + prevChunkSize == chunk) {
                    auto fit = freeChunkSets.find(prevChunkSize);
                    if (fit != freeChunkSets.end()) {
                        auto& chunkSet = fit->second;
                        auto cit = chunkSet.find(prevChunk);
                        if (cit != chunkSet.end()) {
                            // Take over previous chunk
                            LHDEBUG("LH: merge with prev: %p %ld -> %p %ld",
                                    chunk, chunkSize, prevChunk,
                                    chunkSize + prevChunkSize);

                            chunkSet.erase(cit);
                            if (chunkSet.empty()) freeChunkSets.erase(fit);

                            chunk = prevChunk;
                            chunkSize += prevChunkSize;

                            chunkSizes.erase(it);
                            it = pit;  // dsm: per standard, pit stays valid

                            it->second = chunkSize;
                        }
                    }
                }
            }

            // Try to merge with next
            auto nit = std::next(it);
            if (nit != chunkSizes.end()) {
                char* nextChunk = nit->first;
                size_t nextChunkSize = nit->second;

                if (chunk + chunkSize == nextChunk) {
                    auto fit = freeChunkSets.find(nextChunkSize);
                    if (fit != freeChunkSets.end()) {
                        auto& chunkSet = fit->second;
                        auto cit = chunkSet.find(nextChunk);
                        if (cit != chunkSet.end()) {
                            // Take over next chunk
                            LHDEBUG("LH: merge with next: %p %ld -> %ld", chunk,
                                    chunkSize, chunkSize + nextChunkSize);

                            chunkSet.erase(cit);
                            if (chunkSet.empty()) freeChunkSets.erase(fit);

                            chunkSize += nextChunkSize;

                            chunkSizes.erase(nit);

                            it->second = chunkSize;
                        }
                    }
                }
            }

            // Because we merge eagerly, no further merging is possible. So add to freeChunkSets!
            auto fit = freeChunkSets.lower_bound(chunkSize);
            if (fit == freeChunkSets.end() || fit->first != chunkSize) {
                u_unordered_set<char*> chunkSet;
                chunkSet.insert(chunk);
                freeChunkSets[chunkSize] = std::move(chunkSet);
            } else {
                fit->second.insert(chunk);
            }
            LHDEBUG("LH: dealloc done");
        }
} ATTR_LINE_ALIGNED;

};
