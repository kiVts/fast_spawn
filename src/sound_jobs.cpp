
#include "game.h"
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
#include <string>
#include <thread>

namespace {

constexpr uintptr_t kSoundList = 0x14027c610;  // (sound manager, ?): registers every built-in sound
constexpr uintptr_t kSoundLoad = 0x1401a96a0;  // (buffer, fs, mode, game string* path, consumed)
constexpr uintptr_t kFileRead = 0x1401837a0;   // (fs, mode, game string* path, consumed, game string* data) -> 0 ok
constexpr uintptr_t kOggDecode = 0x1400cb470;  // stb_vorbis_decode_memory
constexpr uintptr_t kStrReserve = 0x14017cbb0; // (game string*, size)
constexpr uintptr_t kAlignedFree = 0x1409b2d50;
constexpr uintptr_t kCrtFree = 0x1409a6b34;

struct GStr { // the game's string / byte buffer
    char* p;
    uint32_t len, cap;
};

using List_t = void (*)(void*, void*);
using Load_t = void (*)(uint8_t*, void*, uint64_t, GStr*, void*);
using Read_t = uint8_t (*)(void*, uint64_t, GStr*, GStr*);
using Decode_t = int (*)(const void*, int, int*, int*, int16_t**);
using Reserve_t = void (*)(GStr*, uint32_t);
using Free_t = void (*)(void*);
using alGenBuffers_t = void(__cdecl*)(int, uint32_t*);
using alDeleteBuffers_t = void(__cdecl*)(int, const uint32_t*);
using alGetError_t = int(__cdecl*)();
using alBufferData_t = void(__cdecl*)(uint32_t, int, const void*, int, int);

List_t g_list_orig;
Load_t g_load_orig;
Read_t g_read;
Decode_t g_decode;
Reserve_t g_reserve;
Free_t g_aligned_free, g_crt_free;
alGenBuffers_t al_gen;
alDeleteBuffers_t al_delete;
alGetError_t al_error;
alBufferData_t al_data;
int g_threads = 8;
LARGE_INTEGER g_freq{};

int64_t qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double ms_between(int64_t a, int64_t b) { return double(b - a) * 1000.0 / double(g_freq.QuadPart); }

struct Job {
    uint8_t* buf;
    void* fs;
    uint64_t mode;
    std::string path;
    bool done = false, read_ok = false;
    int samples = 0, channels = 0, rate = 0;
    int16_t* pcm = nullptr;
    double ms = 0;
};

// --- workers -----------------------------------------------------------------------------------------------------
std::mutex g_m;
std::condition_variable g_work_cv, g_done_cv;
std::deque<Job*> g_work;
int g_started = 0;

void run(Job& j)
{
    int64_t t0 = qpc();
    GStr path{};
    path.len = uint32_t(j.path.size());
    g_reserve(&path, path.len + 1);
    memcpy(path.p, j.path.c_str(), path.len + 1);
    GStr data{};
    uint8_t failed = g_read(j.fs, j.mode, &path, &data); // takes the path string
    if (!failed) j.samples = g_decode(data.p, int(data.len), &j.channels, &j.rate, &j.pcm);
    j.read_ok = !failed;
    if (data.len != 0 && data.p) g_aligned_free(data.p);
    j.ms = ms_between(t0, qpc());
}

void worker(int index)
{
    using SetDesc_t = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (auto set = reinterpret_cast<SetDesc_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))) {
        wchar_t name[32];
        swprintf(name, 32, L"sound_decode_%d", index);
        set(GetCurrentThread(), name);
    }
    for (;;) {
        Job* j;
        {
            std::unique_lock<std::mutex> l(g_m);
            g_work_cv.wait(l, [] { return !g_work.empty(); });
            j = g_work.front();
            g_work.pop_front();
        }
        run(*j);
        std::lock_guard<std::mutex> l(g_m);
        j->done = true;
        g_done_cv.notify_all();
    }
}

// --- the game's thread -------------------------------------------------------------------------------------------
thread_local int t_depth = 0;
thread_local std::deque<Job*>* t_pending; // in call order

struct Stats {
    int count = 0, failed = 0, other = 0;
    double sum_ms = 0, max_ms = 0;
    int64_t start = 0;
};
thread_local Stats t_stats;

void finish(Job& j)
{
    uint8_t* b = j.buf;
    if (j.read_ok) {
        int channels = j.channels;
        uint32_t total = uint32_t(channels * j.samples);
        int bytes = int(total * 2);
        *reinterpret_cast<int*>(b + 0xc) = bytes;
        int format = 0x1101; // AL_FORMAT_MONO16
        if (channels == 1) {
            *reinterpret_cast<uint32_t*>(b + 4) = total & 0x7fffffff;
        } else if (channels == 2) {
            format = 0x1103; // AL_FORMAT_STEREO16
            *reinterpret_cast<uint32_t*>(b + 4) = (total & 0x7fffffff) >> 1;
        }
        *reinterpret_cast<int*>(b + 8) = j.rate;
        al_data(*reinterpret_cast<uint32_t*>(b), format, j.pcm, bytes, j.rate);
        if (j.pcm) g_crt_free(j.pcm);
    } else {
        t_stats.failed++;
    }
    b[0x10] = 1;
    if (al_error() != 0) {
        al_delete(1, reinterpret_cast<uint32_t*>(b));
        al_error();
        b[0x10] = 0;
    }
    t_stats.count++;
    t_stats.sum_ms += j.ms;
    t_stats.max_ms = std::max(t_stats.max_ms, j.ms);
    delete &j;
}

// Finishes jobs from the front while they are done; wait = also wait for them (all = until none are left).
void drain(bool wait, size_t keep)
{
    auto& q = *t_pending;
    while (q.size() > keep) {
        Job* j = q.front();
        {
            std::unique_lock<std::mutex> l(g_m);
            if (!j->done) {
                if (!wait) return;
                g_done_cv.wait(l, [j] { return j->done; });
            }
        }
        q.pop_front();
        finish(*j);
    }
}

void load_detour(uint8_t* buf, void* fs, uint64_t mode, GStr* path, void* a5)
{
    if (!t_depth || !path || !path->p || !strstr(path->p, ".ogg")) {
        if (t_depth) t_stats.other++;
        return g_load_orig(buf, fs, mode, path, a5);
    }
    al_gen(1, reinterpret_cast<uint32_t*>(buf));
    al_error();
    Job* j = new Job{buf, fs, mode & 0xff, std::string(path->p, path->len)};
    // As the game: the path string is the callee's to free (the pointer is left as it is, length and capacity 0).
    g_aligned_free(path->p);
    path->len = path->cap = 0;
    {
        std::lock_guard<std::mutex> l(g_m);
        while (g_started < g_threads) std::thread(worker, g_started++).detach();
        g_work.push_back(j);
        g_work_cv.notify_one();
    }
    t_pending->push_back(j);
    drain(false, 0);
    if (t_pending->size() > 64) drain(true, 64); // bound the decoded PCM waiting for its upload
}

void list_detour(void* self, void* a2)
{
    if (t_depth++ == 0) {
        if (!t_pending) t_pending = new std::deque<Job*>;
        t_stats = Stats{};
        t_stats.start = qpc();
    }
    g_list_orig(self, a2);
    if (--t_depth == 0) {
        drain(true, 0);
        double wall = ms_between(t_stats.start, qpc());
        swlog("sounds: %d decoded on %d thread(s) in %.2f s for the whole list (decode + read: %.2f s in total, longest "
              "%.0f ms), %d unreadable, %d not .ogg (game's own path)",
              t_stats.count, g_threads, wall / 1000.0, t_stats.sum_ms / 1000.0, t_stats.max_ms, t_stats.failed, t_stats.other);
    }
}

bool install()
{
    QueryPerformanceFrequency(&g_freq);
    unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    int def = int(std::min(8u, cores - 1));
    g_threads = std::clamp(int(GetPrivateProfileIntW(L"load", L"sound_threads", def, settings_ini())), 0, 32);
    if (g_threads == 0) {
        swlog("sounds: sound_threads=0, the game decodes them one by one");
        return true;
    }
    HMODULE al = GetModuleHandleW(L"OpenAL64.dll");
    if (al) {
        al_gen = reinterpret_cast<alGenBuffers_t>(GetProcAddress(al, "alGenBuffers"));
        al_delete = reinterpret_cast<alDeleteBuffers_t>(GetProcAddress(al, "alDeleteBuffers"));
        al_error = reinterpret_cast<alGetError_t>(GetProcAddress(al, "alGetError"));
        al_data = reinterpret_cast<alBufferData_t>(GetProcAddress(al, "alBufferData"));
    }
    if (!al_gen || !al_delete || !al_error || !al_data) {
        swlog("sounds: OpenAL64.dll functions not found, sound decoding left to the game");
        return true;
    }
    g_read = game_ptr<Read_t>(kFileRead);
    g_decode = game_ptr<Decode_t>(kOggDecode);
    g_reserve = game_ptr<Reserve_t>(kStrReserve);
    g_aligned_free = game_ptr<Free_t>(kAlignedFree);
    g_crt_free = game_ptr<Free_t>(kCrtFree);
    if (!hook_game(kSoundList, reinterpret_cast<void*>(&list_detour), reinterpret_cast<void**>(&g_list_orig)) ||
        !hook_game(kSoundLoad, reinterpret_cast<void*>(&load_detour), reinterpret_cast<void**>(&g_load_orig)))
        return false;
    swlog("sounds: the world's built-in sounds decode on %d threads (\"sounds:\" line per load)", g_threads);
    return true;
}

HookRegistrar reg{{"parallel sound decoding", /*needs_known_build=*/true, &install}};

} // namespace
