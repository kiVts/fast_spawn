
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "sdk.h"
#include "watch.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace {

constexpr uintptr_t kSpawnEnter = 0x1407b70c0;
constexpr uintptr_t kSpawnUpdate = 0x1407b7160;
constexpr uintptr_t kTileStep = 0x140571580;    // host: one tile's state machine, 24 args, arg 22 = cooldown is 0
constexpr uintptr_t kTileManager = 0x140586e20; // tiles wanted per player + ready-to-spawn replies (ring, ?, players, ?)
constexpr uintptr_t kTileLookup = 0x140576690;  // (grid, x | z << 32) -> tile or null
constexpr uintptr_t kJobDone = 0x14017d240;     // (mm_thread_task) -> finished
constexpr uintptr_t kIsHost = 0x140ceb6ff;      // byte: 1 host / single player, 0 client
size_t tile_ring() { return per_build<size_t>(0x6470, 0x6478); }

using u64 = uint64_t;
using SpawnEnter_t = u64 (*)(u64, u64, u64, u64);
using SpawnUpdate_t = u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64);
using TileStep_t = u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64,
                           u64, u64, u64, u64, u64);
using TileManager_t = u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);
using TileLookup_t = int* (*)(void*, u64);
using JobDone_t = bool (*)(void*);

SpawnEnter_t g_enter_orig;
SpawnUpdate_t g_update_orig;
TileStep_t g_tile_orig;
TileManager_t g_mgr_orig;
TileLookup_t g_lookup;
JobDone_t g_job_done;
const volatile uint8_t* g_is_host;

bool g_boost = true;
bool g_watch88 = false;
double g_budget_ms = 12, g_budget_alone_ms = 60, g_max_boost_s = 90, g_log_every_s = 2;
double g_min_wait_ms = 500; // after the host's reply, instead of the game's 30 frames; 0 = the game's frames
LARGE_INTEGER g_freq{};

int64_t qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double ms(int64_t ticks) { return double(ticks) * 1000.0 / double(g_freq.QuadPart); }
int64_t ticks_of_s(double s) { return static_cast<int64_t>(s * double(g_freq.QuadPart)); }

struct TileCounts {
    int pending = 0;  // state != 1
    int by_state[8]{}; // 0..6, 7 = other
};

bool count_pending(const uint8_t* ring, TileCounts& out)
{
    __try {
        const uint8_t* grids = *reinterpret_cast<const uint8_t* const*>(ring + 0x40);
        uint32_t cap = *reinterpret_cast<const uint32_t*>(ring + 0x48), head = *reinterpret_cast<const uint32_t*>(ring + 0x4c);
        if (!grids || !cap) return false;
        for (uint32_t g = 0; g < 2; g++) {
            const uint8_t* grid = grids + ((head + g) % cap) * 0x270;
            const int* const* list = *reinterpret_cast<const int* const* const*>(grid + 0x58);
            uint32_t lcap = *reinterpret_cast<const uint32_t*>(grid + 0x60), lhead = *reinterpret_cast<const uint32_t*>(grid + 0x64);
            uint32_t n = *reinterpret_cast<const uint32_t*>(grid + 0x68);
            if (!n) continue;
            if (!list || !lcap || n > 4096) return false;
            for (uint32_t i = 0; i < n; i++) {
                int st = *list[(lhead + i) % lcap];
                if (st != 1) out.pending++;
                out.by_state[(st >= 0 && st < 7) ? st : 7]++;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool scan_players(const uint8_t* players, int& count, int& waiting)
{
    __try {
        const uint8_t* const* data = *reinterpret_cast<const uint8_t* const* const*>(players + 0x1c0);
        uint32_t cap = *reinterpret_cast<const uint32_t*>(players + 0x1c8), head = *reinterpret_cast<const uint32_t*>(players + 0x1cc);
        uint32_t n = *reinterpret_cast<const uint32_t*>(players + 0x1d0);
        if (n > 256 || (n && (!data || !cap))) return false;
        count = int(n);
        waiting = 0;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t* p = data[(head + i) % cap];
            if (p && p[0x16a]) waiting++;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Client only: is the tile under the player loaded (the check in spawning state 1)? -1 = no tile / unknown.
int tile_under_player(const uint8_t* world)
{
    __try {
        const uint8_t* ch = *reinterpret_cast<const uint8_t* const*>(world + 0xbb9f8);
        if (!ch) return -1;
        int x = int(std::floor(std::floor(*reinterpret_cast<const double*>(ch + 0x70) * 0.001 + 0.5) + 0.5));
        int z = int(std::floor(std::floor(*reinterpret_cast<const double*>(ch + 0x80) * 0.001 + 0.5) + 0.5));
        const uint8_t* ring = world + tile_ring();
        const uint8_t* grids = *reinterpret_cast<const uint8_t* const*>(ring + 0x40);
        uint32_t cap = *reinterpret_cast<const uint32_t*>(ring + 0x48), head = *reinterpret_cast<const uint32_t*>(ring + 0x4c);
        if (!grids || !cap) return -1;
        uint32_t idx = (head + (156000.0f < float(x * 1000) ? 1u : 0u)) % cap;
        int* tile = g_lookup(const_cast<uint8_t*>(grids + idx * 0x270), u64(uint32_t(x)) | (u64(uint32_t(z)) << 32));
        return tile ? *tile : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// --- host: boost window ------------------------------------------------------------------------------------------
// Not one thread: the host's tile code runs on the game-state thread while the world loads (2026-10-21 01:55: the
// spawn's tile manager calls were all on it) and on the update thread afterwards, and the tile manager is also called
// from vehicle-logic threads (physics_fixes runs those in parallel). g_mu guards all of the state below; it is never
// held across a call into the game.
SRWLOCK g_mu = SRWLOCK_INIT;
struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_mu); }
    ~Lock() { ReleaseSRWLockExclusive(&g_mu); }
};
std::atomic<int64_t> g_boost_start{0}; // 0 = off
int64_t g_last_pending = 0, g_last_request = 0;
int g_players = 0;
double g_iter_spent = 0;     // ms of forced finishing in this tick's tile pass
int64_t g_last_step_end = 0; // a gap of > 2 ms since the last tile step = the next tick's pass

struct BoostStats {
    int forced = 0, finished = 0, requests = 0, max_pending = 0;
    double forced_ms = 0, forced_max_ms = 0, iter_max_ms = 0;
    double gate_wait_ms = 0; // time tiles sat with their job done waiting for the cooldown (not forced)
    int gate_waits = 0;
} g_bs;

// Tiles whose job is done but that are not finished yet: when they first became ready.
std::unordered_map<const void*, int64_t> g_ready_since;

bool boost_on() { return g_boost_start.load(std::memory_order_relaxed) != 0; }

void boost_begin(int64_t now, int waiting)
{
    if (!boost_on()) {
        g_bs = BoostStats{};
        g_boost_start.store(now, std::memory_order_relaxed);
        swlog("boost: %d player(s) waiting to spawn (%d connected) - finishing loaded tiles without the 5-tick gap%s",
              waiting, g_players, g_boost ? "" : " [boost=0: timing only]");
    }
    g_bs.requests++;
    g_last_request = now;
    g_last_pending = now;
}

void boost_end(int64_t now, const char* why)
{
    int64_t start = g_boost_start.exchange(0, std::memory_order_relaxed);
    if (!start) return;
    swlog("boost: done after %.2f s (%s): %d tiles finished, %d early (%.0f ms, longest %.1f ms, most per iteration "
          "%.1f ms), %d waited %.0f ms in total for the cooldown, most pending %d",
          ms(now - start) / 1000.0, why, g_bs.finished, g_bs.forced, g_bs.forced_ms, g_bs.forced_max_ms,
          g_bs.iter_max_ms, g_bs.gate_waits, g_bs.gate_wait_ms, g_bs.max_pending);
}

u64 mgr_detour(u64 ring, u64 a2, u64 players, u64 a4, u64 a5, u64 a6, u64 a7, u64 a8, u64 a9, u64 a10, u64 a11,
               u64 a12, u64 a13)
{
    int64_t now = qpc();
    int count = 0, waiting = 0;
    if (players && scan_players(reinterpret_cast<const uint8_t*>(players), count, waiting)) {
        Lock lock;
        g_players = count;
        if (waiting && *g_is_host) boost_begin(now, waiting);
    }
    u64 r = g_mgr_orig(ring, a2, players, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
    Lock lock;
    if (boost_on() && ring) {
        TileCounts tc;
        if (count_pending(reinterpret_cast<const uint8_t*>(ring), tc)) {
            g_bs.max_pending = std::max(g_bs.max_pending, tc.pending);
            if (tc.pending) g_last_pending = now;
        }
        int64_t start = g_boost_start.load(std::memory_order_relaxed);
        if (now - g_last_pending > ticks_of_s(1.0) && now - g_last_request > ticks_of_s(2.0))
            boost_end(now, "no tiles left to load");
        else if (now - start > ticks_of_s(g_max_boost_s))
            boost_end(now, "time limit");
    }
    if (g_ready_since.size() > 512) g_ready_since.clear(); // stale entries (tiles removed while loading)
    return r;
}

u64 tile_detour(u64 tile_, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7, u64 a8, u64 a9, u64 a10, u64 a11, u64 a12,
                u64 a13, u64 a14, u64 a15, u64 a16, u64 a17, u64 a18, u64 a19, u64 a20, u64 a21, u64 gate, u64 a23,
                u64 a24)
{
    int* tile = reinterpret_cast<int*>(tile_);
    int before = tile[0];
    if (before != 2 && before != 3) // not loading / saving: nothing the cooldown holds back
        return g_tile_orig(tile_, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20,
                           a21, gate, a23, a24);

    void* job = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(tile) + (before == 2 ? 0x28 : 0x30));
    bool ready = job && g_job_done(job);
    int64_t t0 = qpc(), ready_at = t0;
    bool force;
    {
        Lock lock;
        if (ms(t0 - g_last_step_end) > 2.0) g_iter_spent = 0;
        if (ready) ready_at = g_ready_since.try_emplace(tile, t0).first->second;
        bool open = (gate & 0xff) != 0;
        double budget = g_players <= 1 ? g_budget_alone_ms : g_budget_ms;
        force = ready && !open && g_boost && boost_on() && g_iter_spent < budget;
    }

    u64 r = g_tile_orig(tile_, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21,
                        force ? 1 : gate, a23, a24);
    int64_t t1 = qpc();
    Lock lock;
    g_last_step_end = t1;
    if (tile[0] != before) {
        g_ready_since.erase(tile);
        if (boost_on()) {
            g_bs.finished++;
            if (force) {
                double d = ms(t1 - t0);
                g_bs.forced++;
                g_bs.forced_ms += d;
                g_bs.forced_max_ms = std::max(g_bs.forced_max_ms, d);
                g_iter_spent += d;
                g_bs.iter_max_ms = std::max(g_bs.iter_max_ms, g_iter_spent);
            } else {
                g_bs.gate_waits++;
                g_bs.gate_wait_ms += ms(t0 - ready_at);
            }
        }
    }
    return r;
}

// --- every Spawning screen: what it waited for (the thread that runs the game state) ----------------------------
enum Wait { kHostReply, kTileUnderPlayer, kWorld88, kTiles, kMinFrames, kVehicles, kWaitCount };
const char* const kWaitName[kWaitCount] = {"host reply", "tile under player", "world+0x88", "tiles", "min frames", "vehicle setup"};

struct SpawnTrack {
    bool active = false, done = false; // done: summary written, ignore the frames until the screen closes
    int64_t start = 0, last = 0, last_log = 0;
    double wait_ms[kWaitCount]{};
    int frames = 0, max_pending = 0;
    int last_state = -1;
    bool watching = false;
    int64_t replied_at = 0;
    int frames_skipped = -1; // frames of the 30-frame minimum ended early by min_wait_ms (-1 = not needed)
} g_sp;

void spawn_begin(int64_t now)
{
    if (g_sp.watching) { // the last screen ended without a summary (left the game, reloaded)
        watch_report(true);
        watch_disarm();
    }
    g_sp = SpawnTrack{};
    g_sp.active = true;
    g_sp.start = g_sp.last = g_sp.last_log = now;
}

void spawn_summary(int64_t now)
{
    double total = ms(now - g_sp.start);
    swlog("spawn: ready in %.2f s (%s, %d frames, %.0f fps): host reply %.2f, tile under player %.2f, tiles %.2f "
          "(most pending %d), world+0x88 %.2f, min frames %.2f, vehicle setup %.2f s",
          total / 1000.0, *g_is_host ? "host" : "client", g_sp.frames, g_sp.frames * 1000.0 / std::max(total, 1.0),
          g_sp.wait_ms[kHostReply] / 1000, g_sp.wait_ms[kTileUnderPlayer] / 1000, g_sp.wait_ms[kTiles] / 1000,
          g_sp.max_pending, g_sp.wait_ms[kWorld88] / 1000, g_sp.wait_ms[kMinFrames] / 1000, g_sp.wait_ms[kVehicles] / 1000);
    if (g_sp.frames_skipped >= 0)
        swlog("spawn: 30-frame minimum ended %.1f s after the host reply with %d frames still to go", g_min_wait_ms / 1000,
              g_sp.frames_skipped);
    if (g_sp.watching) {
        watch_report(true);
        watch_disarm();
    }
    g_sp.active = false;
    g_sp.done = true;
}

u64 enter_detour(u64 self, u64 a2, u64 a3, u64 a4)
{
    spawn_begin(qpc());
    return g_enter_orig(self, a2, a3, a4);
}

u64 update_detour(u64 self, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7, u64 a8)
{
    int64_t now = qpc();
    const uint8_t* s = reinterpret_cast<const uint8_t*>(self);
    bool gap = ms(now - g_sp.last) > 3000.0;
    if (g_sp.done && !gap) {
        g_sp.last = now;
        return g_update_orig(self, a2, a3, a4, a5, a6, a7, a8);
    }
    if (!g_sp.active || gap) spawn_begin(now);

    int state = *reinterpret_cast<const int*>(s + 0x4f8);
    int frames_left = *reinterpret_cast<const int*>(s + 0x4f4);
    if (g_min_wait_ms > 0 && state == 1 && frames_left > 0 && g_sp.replied_at &&
        ms(now - g_sp.replied_at) >= g_min_wait_ms) {
        *reinterpret_cast<int*>(self + 0x4f4) = 0; // the game counts it down only while it is not 0
        g_sp.frames_skipped = frames_left;
        frames_left = 0;
    }
    const uint8_t* world = *reinterpret_cast<const uint8_t* const*>(s + 0x18);
    TileCounts tc;
    int w88 = 0, under = -1;
    Wait w = kVehicles;
    if (state == 0) {
        w = kHostReply;
    } else if (state == 1 && world) {
        count_pending(world + tile_ring(), tc);
        w88 = *reinterpret_cast<const int*>(world + 0x88);
        under = *g_is_host ? -1 : tile_under_player(world);
        if (under >= 0 && under != 1) w = kTileUnderPlayer;
        else if (w88) w = kWorld88;
        else if (tc.pending) w = kTiles;
        else w = kMinFrames;
        g_sp.max_pending = std::max(g_sp.max_pending, tc.pending);
    }
    if (g_watch88 && world && !g_sp.watching) {
        watch_arm(const_cast<uint8_t*>(world + 0x88), "world+0x88");
        g_sp.watching = true;
    }
    g_sp.wait_ms[w] += ms(now - g_sp.last);
    g_sp.last = now;
    g_sp.frames++;

    if (ms(now - g_sp.last_log) >= g_log_every_s * 1000.0 && state <= 1) {
        g_sp.last_log = now;
        swlog("spawn: %.1f s, waiting for %s: %d tiles pending (state 0:%d 2:%d 3:%d 4:%d 5:%d), tile under player %d, "
              "world+0x88 %d, frames left %d",
              ms(now - g_sp.start) / 1000.0, kWaitName[w], tc.pending, tc.by_state[0], tc.by_state[2], tc.by_state[3],
              tc.by_state[4], tc.by_state[5], under, w88, frames_left);
        if (g_sp.watching) {
            watch_arm(const_cast<uint8_t*>(world + 0x88), "world+0x88"); // threads started since
            watch_report(false);
        }
    }

    u64 r = g_update_orig(self, a2, a3, a4, a5, a6, a7, a8);
    int after = *reinterpret_cast<const int*>(s + 0x4f8);
    if (g_sp.active && after != g_sp.last_state) {
        if (after == 1) {
            g_sp.replied_at = qpc();
            swlog("spawn: host replied after %.2f s", ms(g_sp.replied_at - g_sp.start) / 1000.0);
        }
        if (after == 3) spawn_summary(qpc());
        g_sp.last_state = after;
    }
    return r;
}

bool install()
{
    QueryPerformanceFrequency(&g_freq);
    const wchar_t* ini = settings_ini();
    g_boost = GetPrivateProfileIntW(L"spawn", L"boost", 1, ini) != 0;
    g_watch88 = GetPrivateProfileIntW(L"spawn", L"watch_world88", 0, ini) != 0;
    g_budget_ms = GetPrivateProfileIntW(L"spawn", L"budget_ms", 12, ini);
    g_budget_alone_ms = GetPrivateProfileIntW(L"spawn", L"budget_ms_alone", 60, ini);
    g_max_boost_s = GetPrivateProfileIntW(L"spawn", L"max_boost_s", 90, ini);
    g_log_every_s = std::max(1, int(GetPrivateProfileIntW(L"spawn", L"log_every_s", 2, ini)));
    g_min_wait_ms = GetPrivateProfileIntW(L"spawn", L"min_wait_ms", 500, ini);

    g_lookup = game_ptr<TileLookup_t>(kTileLookup);
    g_job_done = game_ptr<JobDone_t>(kJobDone);
    g_is_host = game_ptr<const volatile uint8_t*>(kIsHost);

    bool ok = hook_game(kSpawnEnter, reinterpret_cast<void*>(&enter_detour), reinterpret_cast<void**>(&g_enter_orig)) &&
              hook_game(kSpawnUpdate, reinterpret_cast<void*>(&update_detour), reinterpret_cast<void**>(&g_update_orig)) &&
              hook_game(kTileStep, reinterpret_cast<void*>(&tile_detour), reinterpret_cast<void**>(&g_tile_orig)) &&
              hook_game(kTileManager, reinterpret_cast<void*>(&mgr_detour), reinterpret_cast<void**>(&g_mgr_orig));
    if (!ok) return false;
    swlog("fast spawn: %s, budget %.0f ms per tick (%.0f ms alone), logging every %.0f s%s",
          g_boost ? "tile boost on while players spawn" : "boost off, timing only", g_budget_ms, g_budget_alone_ms,
          g_log_every_s, g_watch88 ? ", watching world+0x88" : "");
    return true;
}

HookRegistrar reg{{"fast spawn", /*needs_known_build=*/true, &install}};

} // namespace
