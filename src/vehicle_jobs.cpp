
#include "game.h"
#include "load_heap.h"
#include "hooks.h"
#include "log.h"
#include "sdk.h"

#include <windows.h>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace {

constexpr uintptr_t kQueueLoop = 0x14017d800;   // mm job queue worker (queue*)
constexpr uintptr_t kMtxLock = 0x14095ab30;     // _Mtx_lock wrapper (mtx) -> 0 ok
constexpr uintptr_t kMtxUnlock = 0x14095ab54;   // _Mtx_unlock (mtx)
constexpr uintptr_t kCndWait = 0x14095ae98;     // _Cnd_wait (cnd, mtx)
constexpr uintptr_t kCndBroadcast = 0x14095ae24; // _Cnd_broadcast (cnd)
constexpr uintptr_t kSetThreadName = 0x14017cca0; // (const char*)
constexpr uintptr_t kFree = 0x1409b2d50;        // the game's _aligned_free
constexpr uintptr_t kResize = 0x1408de0b0;      // ring resize (ring, new capacity): copies, resets head
constexpr uintptr_t kLoadTaskVftable = 0x140b03188; // c_vehicle_local_thread_task_load

using Loop_t = void (*)(uint8_t*);
using Mtx_t = int (*)(void*);
using Unlock_t = void (*)(void*);
using CndWait_t = int (*)(void*, void*);
using Cnd_t = void (*)(void*);
using Name_t = void (*)(const char*);
using Free_t = void (*)(void*);
using Resize_t = void (*)(void*, uint32_t);

Loop_t g_loop_orig;
Mtx_t g_lock;
Unlock_t g_unlock;
CndWait_t g_wait;
Cnd_t g_broadcast;
Name_t g_set_name;
Free_t g_free;
Resize_t g_resize;
uintptr_t g_load_vftable;
int g_threads = 4;
LARGE_INTEGER g_freq{};

double ms_between(int64_t a, int64_t b) { return double(b - a) * 1000.0 / double(g_freq.QuadPart); }
int64_t qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void run_task(uint8_t* task)
{
    auto vt = *reinterpret_cast<void (***)(uint8_t*)>(task);
    vt[1](task);
    task[0xb0] = 0;
    g_lock(task + 0x10);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(task + 8), 1);
    g_broadcast(task + 0x68);
    g_unlock(task + 0x10);
}

bool is_load(const uint8_t* task) { return *reinterpret_cast<const uintptr_t*>(task) == g_load_vftable; }

// --- pool --------------------------------------------------------------------------------------------------------
std::mutex g_pm;
std::condition_variable g_pcv;
std::deque<uint8_t*> g_work;
int g_in_flight = 0, g_started = 0;

struct Batch { // loads since the pool was last idle
    int loads = 0, others = 0, peak = 0;
    int64_t first = 0, last_end = 0;
    double sum_ms = 0, max_ms = 0, other_ms = 0;
} g_batch;

void log_batch_locked()
{
    if (!g_batch.loads) return;
    swlog("vehicle loads: %d in %.2f s on %d thread(s) (each: avg %.0f ms, longest %.0f ms; up to %d at once), %d other "
          "task(s) %.0f ms",
          g_batch.loads, ms_between(g_batch.first, g_batch.last_end) / 1000.0, g_threads, g_batch.sum_ms / g_batch.loads,
          g_batch.max_ms, g_batch.peak, g_batch.others, g_batch.other_ms);
    if (load_heap_on()) {
        size_t in_use, committed;
        unsigned long long fallbacks;
        load_heap_stats(in_use, committed, fallbacks);
        swlog("vehicle loads: arenas %.1f MB in use of %.1f MB committed, %llu allocation(s) fell back to the game's heap",
              in_use / 1048576.0, committed / 1048576.0, fallbacks);
    }
    g_batch = Batch{};
}

void pool_thread(int index)
{
    using SetDesc_t = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (auto set = reinterpret_cast<SetDesc_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))) {
        wchar_t name[32];
        swprintf(name, 32, L"vehicle_load_%d", index);
        set(GetCurrentThread(), name);
    }
    load_heap_enter_thread(index);
    for (;;) {
        uint8_t* task;
        {
            std::unique_lock<std::mutex> l(g_pm);
            g_pcv.wait(l, [] { return !g_work.empty(); });
            task = g_work.front();
            g_work.pop_front();
        }
        int64_t t0 = qpc();
        run_task(task);
        int64_t t1 = qpc();
        std::lock_guard<std::mutex> l(g_pm);
        double d = ms_between(t0, t1);
        g_batch.sum_ms += d;
        g_batch.max_ms = std::max(g_batch.max_ms, d);
        g_batch.last_end = t1;
        if (--g_in_flight == 0 && g_work.empty()) log_batch_locked();
        g_pcv.notify_all();
    }
}

void pool_submit(uint8_t* task)
{
    std::unique_lock<std::mutex> l(g_pm);
    while (g_started < g_threads) std::thread(pool_thread, g_started++).detach();
    g_pcv.wait(l, [] { return g_in_flight < g_threads; });
    if (!g_batch.loads) g_batch.first = qpc();
    g_batch.loads++;
    g_in_flight++;
    g_batch.peak = std::max(g_batch.peak, g_in_flight);
    g_work.push_back(task);
    g_pcv.notify_all();
}

void pool_wait_idle()
{
    std::unique_lock<std::mutex> l(g_pm);
    g_pcv.wait(l, [] { return g_in_flight == 0; });
}

// --- the vehicle queue's worker ----------------------------------------------------------------------------------
void vehicle_queue_loop(uint8_t* q)
{
    void* mtx = q + 0x38;
    void* cnd = q + 0x90;
    auto& data = *reinterpret_cast<uint8_t***>(q + 0x18);
    auto& cap = *reinterpret_cast<uint32_t*>(q + 0x20);
    auto& head = *reinterpret_cast<uint32_t*>(q + 0x24);
    auto& count = *reinterpret_cast<uint32_t*>(q + 0x28);
    volatile uint8_t& stop = q[0xd8];

    if (g_lock(mtx) != 0) return;
    for (;;) {
        while (count == 0 && !stop) g_wait(cnd, mtx);
        if (stop) break;
        uint8_t* task = data[head % cap];
        uint32_t old_cap = cap;
        head = head == cap - 1 ? 0 : head + 1;
        if (--count == 0) { // as the game's loop: an empty ring gives its buffer back
            if (old_cap) {
                g_free(data);
                data = nullptr;
                cap = 0;
            }
            // The game leaves head as it was here; the next push regrows to 32 entries without resetting it, and a
            // head past 31 then never wraps and the following growth copies from past the buffer (2026-10-21 21:27
            // crash). The game only gets away with it because its shrink below keeps cap at 32 by the time the ring
            // is empty. Nothing is queued and there is no buffer, so 0 is the only meaningful head.
            head = 0;
        } else if (count < old_cap / 2 && old_cap > 32) { // as the game's loop: shrink (resets head)
            g_resize(q + 0x18, old_cap / 2);
        }
        *reinterpret_cast<uint8_t**>(q + 0x30) = task;
        g_unlock(mtx);

        if (is_load(task)) {
            pool_submit(task);
        } else { // a save or anything else: after every load before it, alone
            pool_wait_idle();
            int64_t t0 = qpc();
            run_task(task);
            std::lock_guard<std::mutex> l(g_pm);
            g_batch.others++;
            g_batch.other_ms += ms_between(t0, qpc());
        }
        *reinterpret_cast<uint8_t**>(q + 0x30) = nullptr;
        if (g_lock(mtx) != 0) return;
    }
    g_unlock(mtx);
    pool_wait_idle(); // the owner joins this thread and then frees the queue and its tasks
}

void loop_detour(uint8_t* q)
{
    const char* name = nullptr;
    __try {
        name = *reinterpret_cast<const char* const*>(q + 0xe0);
        if (!name || strcmp(name, "vehicle_save") != 0) name = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        name = nullptr;
    }
    if (!name) return g_loop_orig(q);
    g_set_name(name);
    swlog("vehicle loads: queue %p (\"vehicle_save\") worker started, loads on %d thread(s)", q, g_threads);
    vehicle_queue_loop(q);
}

bool install()
{
    QueryPerformanceFrequency(&g_freq);
    g_threads = std::clamp(int(GetPrivateProfileIntW(L"spawn", L"vehicle_load_threads", 4, settings_ini())), 0, 16);
    if (g_threads == 0) {
        swlog("vehicle loads: vehicle_load_threads=0, game's own loop");
        return true;
    }
    g_lock = game_ptr<Mtx_t>(kMtxLock);
    g_unlock = game_ptr<Unlock_t>(kMtxUnlock);
    g_wait = game_ptr<CndWait_t>(kCndWait);
    g_broadcast = game_ptr<Cnd_t>(kCndBroadcast);
    g_set_name = game_ptr<Name_t>(kSetThreadName);
    g_free = game_ptr<Free_t>(kFree);
    g_resize = game_ptr<Resize_t>(kResize);
    g_load_vftable = reinterpret_cast<uintptr_t>(game_ptr<void*>(kLoadTaskVftable));
    if (!hook_game(kQueueLoop, reinterpret_cast<void*>(&loop_detour), reinterpret_cast<void**>(&g_loop_orig))) return false;
    swlog("vehicle loads: up to %d at once on the vehicle queue (\"vehicle loads:\" lines per batch)", g_threads);
    return true;
}

HookRegistrar reg{{"parallel vehicle loads", /*needs_known_build=*/true, &install}};

} // namespace
