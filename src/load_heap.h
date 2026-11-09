#pragma once
#include <cstddef>

// Per-thread arenas for the parallel vehicle loads (load_heap.cpp).
void load_heap_enter_thread(int index); // this pool thread's allocations go to arena `index` (if the arenas are on)
bool load_heap_on();
void load_heap_stats(size_t& in_use, size_t& committed, unsigned long long& fallbacks);
