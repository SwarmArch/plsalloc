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

#include <stddef.h>
#include "common.h"

/* Follows interface of STL allocator, allocates and frees from simulator-
 * managed untracked memory. This code lets us use STL containers inside the
 * allocator.
 *
 * NOTE: Based on zsim's sim/g_std/stl_galloc.h
 */

template <class T>
class StlUntrackedAlloc {
    public:
        typedef size_t size_type;
        typedef ptrdiff_t difference_type;
        typedef T* pointer;
        typedef const T* const_pointer;
        typedef T& reference;
        typedef const T& const_reference;
        typedef T value_type;

        StlUntrackedAlloc() {}
        StlUntrackedAlloc(const StlUntrackedAlloc&) {}

        pointer allocate(size_type n, const void * = 0) {
            T* t = (T*) sim_zero_cycle_untracked_malloc(sizeof(T) * n);
            return t;
        }

        void deallocate(void* p, size_type) {
            if (p) sim_zero_cycle_free(p);
        }

        pointer address(reference x) const { return &x; }
        const_pointer address(const_reference x) const { return &x; }
        StlUntrackedAlloc<T>& operator=(const StlUntrackedAlloc&) { return *this; }


        // Construct/destroy
        // gcc keeps changing these interfaces. See /usr/include/c++/4.8/ext/new_allocator.h
#if __cplusplus >= 201103L // >= 4.8
        template<typename _Up, typename... _Args>
        void construct(_Up* __p, _Args&&... __args) { ::new((void *)__p) _Up(std::forward<_Args>(__args)...); }

        template<typename _Up> void destroy(_Up* __p) { __p->~_Up(); }
#else // < 4.8
        void construct(pointer p, const T& val) { new (static_cast<T*>(p)) T(val); }
        void construct(pointer p) { construct(p, value_type()); } //required by gcc 4.6
        void destroy(pointer p) { p->~T(); }
#endif

        size_type max_size() const { return size_t(-1); }

        template <class U> struct rebind { typedef StlUntrackedAlloc<U> other; };

        template <class U> StlUntrackedAlloc(const StlUntrackedAlloc<U>&) {}

        template <class U> StlUntrackedAlloc& operator=(const StlUntrackedAlloc<U>&) { return *this; }

        /* dsm: The == (and !=) operator in an allocator must be defined and,
         * per http://download.oracle.com/docs/cd/E19422-01/819-3703/15_3.htm :
         *
         *   Returns true if allocators b and a can be safely interchanged. Safely
         *   interchanged means that b could be used to deallocate storage obtained
         *   through a, and vice versa.
         *
         * We can ALWAYS do this, as deallocate just calls gm_free()
         */
        template <class U> bool operator==(const StlUntrackedAlloc<U>&) const { return true; }

        template <class U> bool operator!=(const StlUntrackedAlloc<U>&) const { return false; }
};
