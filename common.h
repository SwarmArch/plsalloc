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

/* Specialized assertion, debug, and miscellaneous macros, suitable for use
 * inside the memory allocator */

#include <tuple>

// Define info/warn macros and include hooks only if this isn't being used from
// the simulator.
#ifndef PLSALLOC_INCLUDED_FROM_SIM
#include "swarm/hooks.h"

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

// dsm: Doesn't alloc any heap memory
#define info(args...) { \
    char buf[1024]; \
    snprintf(buf, sizeof(buf), args); \
    sim_magic_op_1(MAGIC_OP_WRITE_STD_OUT, reinterpret_cast<uint64_t>(&buf[0])); \
}

#ifdef assert
#undef assert
#endif

#ifdef NASSERT
#define assert(cond)
#else
#define assert(cond) \
    if (unlikely(!(cond))) __plsalloc_assert_fail(__FILE__, __LINE__);

static void __plsalloc_assert_fail(const char* file, size_t line) {
    info("%s:%ld : internal plsalloc assertion failed", file, line);
    std::abort();  // TODO(dsm): May require
}
#endif

/* Alignment macros */

#define CACHE_LINE_BYTES (64)
#define ATTR_LINE_ALIGNED __attribute__((aligned CACHE_LINE_BYTES ))

#endif  // PLSALLOC_INCLUDED_FROM_SIM

/* System allocator interface, used all over the place */
namespace plsalloc {
static std::tuple<char*, char*> sysAlloc(size_t chunkSize);
};
