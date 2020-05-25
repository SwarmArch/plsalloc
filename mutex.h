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

#include <stdint.h>
#include <xmmintrin.h>

/* TICKET LOCK: Provides FIFO ordering for fairness.
 * WARNING: Will not work with more than 64K threads
 */

#define TICKET_MASK ((1<<16) - 1)

static inline void ticket_init(volatile uint32_t* lock) {
    *lock = 0;
    __sync_synchronize();
}

static inline void ticket_lock(volatile uint32_t* lock) {
    /* Technically, we want to do this, but I'm guessing the 64-bit
     * datapath is not very well optimized for 16-bit xadd...
     * volatile uint16_t* low = ((volatile uint16_t*) lock) + 1;
     * uint32_t ticket = atomic_fetchadd_16(low, 1);
     */
    uint32_t val, hi, newLo;
    while (true) {
        val = *lock;
        hi = val & (TICKET_MASK << 16);
        newLo = (val + 1) & TICKET_MASK;
        if (__sync_bool_compare_and_swap(lock, val, (hi | newLo))) break;
    }

    uint32_t ticket = val & TICKET_MASK;

    while ((((*lock) >> 16) & TICKET_MASK) != ticket) {
        _mm_pause();
    }
}

static inline int ticket_trylock(volatile uint32_t* lock) {
    uint32_t val = *lock;
    uint32_t hi = (val >> 16) & TICKET_MASK;
    uint32_t lo = val & TICKET_MASK;
    uint32_t newLo = (lo + 1) & TICKET_MASK;
    return (hi == lo /*This is up for grabs*/ && __sync_bool_compare_and_swap(lock, val, ((hi << 16) | newLo)) /*T&S*/);
}

static inline void ticket_unlock(volatile uint32_t* lock) {
    __sync_fetch_and_add(lock, 1<<16);
}


class mutex {
    private:
        volatile uint32_t tlock;
    public:
        mutex() { ticket_init(&tlock); }
        void lock() { ticket_lock(&tlock); }
        void unlock() { ticket_unlock(&tlock); }
        bool trylock() { return ticket_trylock(&tlock); }
};

class scoped_mutex {
    private:
        mutex& mut;
    public:
        scoped_mutex(mutex& _mut) : mut(_mut) { mut.lock(); }
        ~scoped_mutex() { mut.unlock(); }
};
