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

/* Blocked deque class used for alloc freelists. Based on the Swarm runtime's
 * BlockedDeque, but using specialized allocation and assertions so that it can
 * be used inside malloc.
 */

#include <stdint.h>
#include "common.h"

// Elements per block. Keep this a power of 2
#define DQBLOCK_SIZE (32u)
#define DQBLOCK_MASK (DQBLOCK_SIZE-1)

/* Nomenclature: head grows UP, tail grows down. Tail grows block list in next,
 * head does so in prev
 */

//#define BQDEBUG(args...) __print(args)
#define BQDEBUG(args...)

template <class T>
struct DequeBlock {
    struct DequeBlock<T>* __restrict__ prev;
    struct DequeBlock<T>* __restrict__ next;
    T elems[DQBLOCK_SIZE];

    // dsm: Temporarily use simalloc; should use own base untracked alloc eventually.
    static DequeBlock<T>* __restrict__ alloc() {
        void* p = sim_zero_cycle_untracked_malloc(sizeof(DequeBlock<T>));
        assert(p);
        return (DequeBlock<T>*) p;
    }

    static void dealloc(DequeBlock<T>* blk) {
        sim_zero_cycle_free(blk);
    }
};

template <class T>
class BlockedDeque {
    private:
        DequeBlock<T>* bhead;
        DequeBlock<T>* btail;
        uint64_t phead;  // first USED position
        uint64_t ptail;  // first FREE position

        template<bool head>
        void expand() {
            DequeBlock<T>* newBlock = DequeBlock<T>::alloc();
            if (!bhead) {
                assert(!btail);
                bhead = btail = newBlock;
            } else if (head) {
                bhead->prev = newBlock;
                newBlock->next = bhead;
                bhead = newBlock;
            } else {
                btail->next = newBlock;
                newBlock->prev = btail;
                btail = newBlock;
            }
        }

        template <bool head>
        void shrink() {
            auto block = head? bhead : btail;
            assert(block);
            if (head) {
                bhead = bhead->next;
            } else {
                btail = btail->prev;
            }
            DequeBlock<T>::dealloc(block);
        }

        void reset() {
            assert(phead == ptail);
            assert(bhead == btail);
            DequeBlock<T>::dealloc(bhead);
            bhead = nullptr;
            btail = nullptr;
            phead = 0;
            ptail = 0;
        }

    public:
        void init() {
            bhead = nullptr;
            btail = nullptr;
            phead = 0;
            ptail = 0;
        }

        inline uint64_t size() const {
            return ptail - phead;
        }

        inline bool empty() const {
            return !bhead;  // faster
            //return ptail == phead;  // more general
        }

        inline void push_front(T val) {
            BQDEBUG("enqueueHead bhead=%p phead=%d numElems=%ld\n", bhead, phead, numElems);
            if ((phead & DQBLOCK_MASK) == 0) expand<true>();
            bhead->elems[--phead & DQBLOCK_MASK] = val;
        }

        inline void push_back(T val) {
            BQDEBUG("enqueueTail btail=%p ptail=%d numElems=%ld\n", btail, ptail, size());
            if ((ptail & DQBLOCK_MASK) == 0) expand<false>();
            btail->elems[ptail & DQBLOCK_MASK] = val;
            ptail++;
        }

        inline T front() const { return bhead->elems[phead & DQBLOCK_MASK]; }
        inline T back()  const { return btail->elems[(ptail-1) & DQBLOCK_MASK]; }

        inline void pop_front() {
            assert(!empty());
            phead++;
            if (__builtin_expect(phead == ptail, 0)) { reset(); return; }
            if (__builtin_expect(!(phead & DQBLOCK_MASK), 0)) shrink<true>();
        }

        inline void pop_back() {
            assert(!empty());
            ptail--;
            if (__builtin_expect(phead == ptail, 0)) { reset(); return; }
            if (__builtin_expect(!(ptail & DQBLOCK_MASK), 0)) shrink<false>();
        }

        // Equivalent to back() + pop_back(), but slightly faster
        inline T dequeue_back() {
            T res = btail->elems[(--ptail) & DQBLOCK_MASK];
            if (unlikely(!(ptail & DQBLOCK_MASK))) {
                if (bhead == btail) reset();
                else shrink<false>();
            }
            return res;
        }

        // Splices front of list in full blocks. Invariants:
        // - Head must be aligned at block granularity
        // - Must have at least one more block than spliced blocks (i.e., can't
        //   leave an empty list)
        inline BlockedDeque<T> splice_front(size_t blocks) {
            assert(!(phead & DQBLOCK_MASK));
            BlockedDeque<T> res;
            res.bhead = bhead;
            DequeBlock<T>* splicePoint = bhead;
            for (size_t b = 1; b < blocks; b++) splicePoint = splicePoint->next;
            assert(splicePoint && splicePoint->next);

            res.btail = splicePoint;
            res.phead = 0;
            res.ptail = blocks * DQBLOCK_SIZE;

            bhead = splicePoint->next;
            splicePoint->next = nullptr;
            bhead->prev = nullptr;
            phead += blocks * DQBLOCK_SIZE;

            return res;
        }

        // Merges a list to the front. Invariants:
        // - Both bheads must be block-aligned
        // - list must not be empty
        inline void merge_front(BlockedDeque<T>& __restrict__ list) {
            if (empty()) {
                *this = list;  // just take over
            } else {
                assert(!(phead & DQBLOCK_MASK));
                assert(!(list.ptail & DQBLOCK_MASK));
                phead -= list.size();
                list.btail->next = bhead;
                bhead->prev = list.btail;
                bhead = list.bhead;
            }
        }

        // Steals front block to an empty list. Invariants:
        // - Must have at least one full block.
        inline void steal_front(BlockedDeque<T>& __restrict__ dstList) {
            dstList.bhead = bhead;
            dstList.btail = bhead;
            dstList.phead = 0;
            dstList.ptail = DQBLOCK_SIZE;

            if (bhead == btail) {
                init();  // we're now empty
            } else {
                bhead = bhead->next;
                bhead->prev = nullptr;
                phead += DQBLOCK_SIZE;
            }
        }
};
