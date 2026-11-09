
#include "load_heap.h"

#include "game.h"
#include "hooks.h"
#include "log.h"
#include "sdk.h"
#include "thread_arenas.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

constexpr uintptr_t kMallocBase = 0x1409be108;
constexpr uintptr_t kCallocBase = 0x1409bd228;
constexpr uintptr_t kFreeBase = 0x1409bd2a0;
constexpr uintptr_t kReallocBase = 0x1409bf0d0;
constexpr uintptr_t kMsizeBase = 0x1409c880c;
constexpr uintptr_t kExpandBase = 0x1409c871c;

constexpr int kLoadArenas = 16;    // vehicle_load_threads is at most 16
constexpr int kGeneralArenas = 32; // every other thread ([spawn] all_threads)
constexpr int kArenas = kLoadArenas + kGeneralArenas;
constexpr size_t kMaxRequest = ThreadArenas::kMaxRequest;

using Malloc_t = void* (*)(size_t);
using Calloc_t = void* (*)(size_t, size_t);
using Free_t = void (*)(void*);
using Realloc_t = void* (*)(void*, size_t);
using Msize_t = size_t (*)(void*);
using Expand_t = void* (*)(void*, size_t);

Malloc_t g_malloc;
Calloc_t g_calloc;
Free_t g_free;
Realloc_t g_realloc;
Msize_t g_msize;
Expand_t g_expand;

ThreadArenas g_heap;
bool g_on = false;
std::atomic<unsigned long long> g_fallbacks{0};
thread_local int t_arena = -1;
bool g_all_threads = false;
std::atomic<unsigned> g_next_general{0};
std::atomic<unsigned long long> g_general_threads{0};

// This thread's arena: a load thread's own, else (all_threads) one of the general ones, picked at the first call.
inline int arena_of_thread()
{
    if (t_arena >= 0 || !g_all_threads) return t_arena;
    t_arena = kLoadArenas + int(g_next_general.fetch_add(1, std::memory_order_relaxed) % kGeneralArenas);
    g_general_threads.fetch_add(1, std::memory_order_relaxed);
    return t_arena;
}

bool ours(const void* p) { return g_on && g_heap.owns(p); }

void* thread_alloc(size_t n)
{
    if (void* p = g_heap.alloc(arena_of_thread(), n)) return p;
    g_fallbacks.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void* malloc_detour(size_t n)
{
    if (arena_of_thread() >= 0)
        if (void* p = thread_alloc(n)) return p;
    return g_malloc(n);
}

void* calloc_detour(size_t count, size_t size)
{
    if (arena_of_thread() >= 0 && size && count <= kMaxRequest / size) {
        size_t n = count * size;
        if (void* p = thread_alloc(n)) {
            memset(p, 0, n); // free-list blocks hold old data
            return p;
        }
    }
    return g_calloc(count, size);
}

void free_detour(void* p)
{
    if (ours(p)) return g_heap.free(p);
    g_free(p);
}

void* realloc_detour(void* p, size_t n)
{
    if (!ours(p)) return p ? g_realloc(p, n) : malloc_detour(n);
    if (n == 0) { // as _realloc_base: frees and returns null
        g_heap.free(p);
        return nullptr;
    }
    if (g_heap.resize_in_place(p, n)) return p;
    void* q = malloc_detour(n);
    if (!q) return nullptr; // the old block stays valid, as with the CRT
    memcpy(q, p, g_heap.size(p));
    g_heap.free(p);
    return q;
}

size_t msize_detour(void* p)
{
    if (ours(p)) return g_heap.size(p);
    return g_msize(p);
}

void* expand_detour(void* p, size_t n)
{
    if (!ours(p)) return g_expand(p, n);
    return g_heap.resize_in_place(p, n) ? p : nullptr; // can't grow past its class
}

DWORD WINAPI log_loop(void*)
{
    unsigned long long last_allocs = 0, last_fallbacks = 0;
    for (;;) {
        Sleep(60000);
        size_t in_use, committed;
        unsigned long long allocs;
        g_heap.stats(in_use, committed, allocs);
        unsigned long long fb = g_fallbacks.load(std::memory_order_relaxed);
        swlog("game heap 60s: %.0f allocations/s from the arenas (%llu threads so far), %.0f MB in use of %.0f MB committed, "
              "%.1f/s fell back to the game's heap (over 1 MB, or an arena full)",
              (allocs - last_allocs) / 60.0, g_general_threads.load(), in_use / 1048576.0, committed / 1048576.0,
              (fb - last_fallbacks) / 60.0);
        last_allocs = allocs, last_fallbacks = fb;
    }
}

bool install()
{
    int mode = int(GetPrivateProfileIntW(L"spawn", L"load_heap", 1, settings_ini()));
    bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    if (mode == 0 || (mode == 1 && !wine)) {
        swlog("load heap: off (%s)", mode == 0 ? "load_heap=0" : "not Wine; Windows' heap doesn't serialize the loads");
        return true;
    }
    // Top-down (thread_arenas.h): bottom-up, Wine put the reservation right above the game's image and code that
    // needs memory within reach of the exe (physics_fixes' radar stub, hook trampolines) found none (2026-10-22
    // 02:35 / 02:52: "radar msgs: no memory within 4 GB above the game; off").
    if (!g_heap.reserve(kArenas)) {
        swlog("load heap: couldn't reserve address space for the arenas, off");
        return true;
    }
    g_on = true;
    bool ok = hook_game(kFreeBase, reinterpret_cast<void*>(&free_detour), reinterpret_cast<void**>(&g_free)) &&
              hook_game(kReallocBase, reinterpret_cast<void*>(&realloc_detour), reinterpret_cast<void**>(&g_realloc)) &&
              hook_game(kMsizeBase, reinterpret_cast<void*>(&msize_detour), reinterpret_cast<void**>(&g_msize)) &&
              hook_game(kExpandBase, reinterpret_cast<void*>(&expand_detour), reinterpret_cast<void**>(&g_expand)) &&
              hook_game(kCallocBase, reinterpret_cast<void*>(&calloc_detour), reinterpret_cast<void**>(&g_calloc)) &&
              hook_game(kMallocBase, reinterpret_cast<void*>(&malloc_detour), reinterpret_cast<void**>(&g_malloc));
    if (!ok) { // nothing allocates from the arenas until a pool thread enters, so this is still safe
        g_on = false; // ours() false for everything
        return false;
    }
    // every thread only once all six are hooked: a block handed out earlier would reach an unhooked free
    g_all_threads = GetPrivateProfileIntW(L"spawn", L"all_threads", 1, settings_ini()) != 0;
    swlog("load heap: on (%s): each vehicle load thread allocates from its own arena%s (%d x %zu MB reserved at %p)",
          wine ? "Wine" : "load_heap=2", g_all_threads ? ", every other thread from one of 32 more (\"game heap 60s\" lines)" : "",
          kArenas, g_heap.slice() >> 20, g_heap.base());
    if (g_all_threads) CloseHandle(CreateThread(nullptr, 0, log_loop, nullptr, 0, nullptr));
    return true;
}

HookRegistrar reg{{"vehicle load arenas", /*needs_known_build=*/true, &install}};

} // namespace

bool load_heap_on() { return g_on; }

void load_heap_enter_thread(int index)
{
    if (g_on && index >= 0 && index < kLoadArenas) t_arena = index;
}

void load_heap_stats(size_t& in_use, size_t& committed, unsigned long long& fallbacks)
{
    unsigned long long allocs;
    g_heap.stats(in_use, committed, allocs);
    fallbacks = g_fallbacks.load(std::memory_order_relaxed);
}
