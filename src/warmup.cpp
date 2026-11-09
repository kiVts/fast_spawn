// Background warm-up: read the game's data files once at start-up, in parallel, so the first load doesn't pay for them.
//
// Straight after a reboot a game load took 30.9 s (2026-10-21 13:15), 24 s of it opening the ~29,000 files that exist
// (0.82 ms each; 0.01-0.02 ms once they have been opened since boot: first-open scans and cold reads). The game opens
// them one by one on its loading thread. This lists rom\ and the Stormworks workshop folder when the game starts and
// opens + reads every file on a few background threads (Windows background priority, so the game's own disk and CPU
// use come first), the folders the game loads first first. Read only: GENERIC_READ, OPEN_EXISTING, sharing everything,
// nothing is written or kept open. Settings: [load] warmup_threads (0 = off). Logs "warm-up:" when done.

#include "hooks.h"
#include "log.h"
#include "sdk.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_threads = 6;
std::vector<std::wstring> g_files;
std::atomic<size_t> g_next{0};
std::atomic<uint64_t> g_bytes{0};
std::atomic<int> g_done_threads{0}, g_failed{0};
ULONGLONG g_start = 0;

void list(const std::wstring& dir, std::vector<std::wstring>& out, int depth)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 12 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) list(dir + L"\\" + n, out, depth + 1);
        } else {
            out.push_back(dir + L"\\" + n);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

void reader()
{
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN); // low CPU and disk priority
    std::vector<char> buf(1 << 20);
    for (size_t i; (i = g_next.fetch_add(1)) < g_files.size();) {
        HANDLE h = CreateFileW(g_files[i].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            g_failed++;
            continue;
        }
        DWORD got;
        while (ReadFile(h, buf.data(), DWORD(buf.size()), &got, nullptr) && got) g_bytes += got;
        CloseHandle(h);
    }
    if (g_done_threads.fetch_add(1) + 1 == g_threads)
        swlog("warm-up: %zu files, %.0f MB read in %.1f s on %d threads (%d couldn't be opened)", g_files.size(),
              g_bytes / 1048576.0, (GetTickCount64() - g_start) / 1000.0, g_threads, int(g_failed));
}

void start()
{
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    g_start = GetTickCount64();
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    dir.resize(dir.find_last_of(L'\\'));
    std::wstring rom = dir + L"\\rom";
    // What a load opens first, first: definitions, meshes, audio, tiles, then the rest of rom\, then the workshop items.
    for (const wchar_t* sub : {L"\\data\\definitions", L"\\meshes", L"\\audio", L"\\data\\tiles"}) list(rom + sub, g_files, 0);
    std::vector<std::wstring> rest;
    list(rom, rest, 0);
    std::sort(g_files.begin(), g_files.end());
    for (auto& f : rest)
        if (!std::binary_search(g_files.begin(), g_files.end(), f)) g_files.push_back(f);
    std::wstring lower = dir;
    for (auto& c : lower) c = towlower(c);
    size_t common = lower.rfind(L"\\steamapps\\common\\");
    if (common != std::wstring::npos) list(dir.substr(0, common) + L"\\steamapps\\workshop\\content\\573090", g_files, 0);
    for (int i = 0; i < g_threads; i++) std::thread(reader).detach();
}

bool install()
{
    g_threads = std::clamp(int(GetPrivateProfileIntW(L"load", L"warmup_threads", 6, settings_ini())), 0, 32);
    if (!g_threads) {
        swlog("warm-up: off in settings.ini");
        return true;
    }
    std::thread(start).detach();
    swlog("warm-up: reading rom\\ and the workshop folder in the background on %d threads", g_threads);
    return true;
}

HookRegistrar reg{{"start-up file warm-up", /*needs_known_build=*/false, &install}};

} // namespace
