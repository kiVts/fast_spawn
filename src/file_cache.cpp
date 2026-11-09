
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "sdk.h"

#include <windows.h>
#include <atomic>
#include <cwctype>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr uintptr_t kStateCreateRun = 0x1408ea910;

using Run_t = void (*)(void*);
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
Run_t g_run_orig;
CreateFileW_t g_create_orig;

std::wstring g_roots[3]; // rom, workshop, mods_temp; lower case, no trailing backslash
std::atomic<int> g_loading{0};
std::atomic<bool> g_on{true};
std::atomic<int> g_verify_left{500};
SRWLOCK g_lock = SRWLOCK_INIT;

struct Dir {
    bool exists = false;
    std::unordered_set<std::wstring> names; // lower case, long and short names
    FILETIME written{};                     // the folder's last-write time when listed (watched folders only)
};
// A CreateFileW call can still be reading a listing when the last state-load task
// clears the cache. The loading counter gates new lookups; it does not pin readers
// already in flight. Keep immutable entries alive until their last reader returns.
using DirPtr = std::shared_ptr<const Dir>;
std::unordered_map<std::wstring, DirPtr> g_dirs;     // workshop folders, cleared after each load
std::unordered_map<std::wstring, DirPtr> g_rom_dirs; // rom\ folders, kept for the session
std::atomic<int> g_play_hits{0}, g_play_passed{0};   // rom\ opens outside loads
std::atomic<unsigned long long> g_play_logged_at{0};
std::atomic<int> g_hits{0}, g_passed{0}, g_listed{0}, g_verified{0}, g_relisted{0};

bool ascii_lower(std::wstring& s)
{
    for (auto& c : s) {
        if (c >= 0x80) return false;
        if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
        if (c == L'/') c = L'\\';
    }
    return true;
}

FILETIME folder_written(const std::wstring& dir)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &a)) return FILETIME{};
    return a.ftLastWriteTime;
}

bool same(const FILETIME& a, const FILETIME& b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

// watch: the folder may change during the load (mods_temp); its listing is used only while its last-write time is
// the one it was listed at.
DirPtr listing(const std::wstring& dir, bool rom = false, bool watch = false)
{
    auto& dirs = rom ? g_rom_dirs : g_dirs;
    FILETIME now_written = watch ? folder_written(dir) : FILETIME{};
    {
        AcquireSRWLockShared(&g_lock);
        auto it = dirs.find(dir);
        bool found = it != dirs.end();
        DirPtr d = found ? it->second : nullptr; // acquire ownership while the map is locked
        ReleaseSRWLockShared(&g_lock);
        if (found && (!watch || same(d->written, now_written))) return d;
        if (found) g_relisted++;
    }
    Dir d;
    d.written = now_written;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoStandard, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (f != INVALID_HANDLE_VALUE) {
        d.exists = true;
        do {
            std::wstring n = fd.cFileName, a = fd.cAlternateFileName;
            for (auto& c : n) c = towlower(c);
            for (auto& c : a) c = towlower(c);
            d.names.insert(n);
            if (!a.empty()) d.names.insert(a);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    } else {
        DWORD e = GetLastError();
        d.exists = e != ERROR_PATH_NOT_FOUND && e != ERROR_FILE_NOT_FOUND;
        if (d.exists) d.names.insert(L"*"); // couldn't list it (access?): never answer for this folder
    }
    g_listed++;
    DirPtr candidate = std::make_shared<const Dir>(std::move(d));
    AcquireSRWLockExclusive(&g_lock);
    DirPtr slot = watch ? dirs.insert_or_assign(dir, std::move(candidate)).first->second
                        : dirs.try_emplace(dir, std::move(candidate)).first->second;
    ReleaseSRWLockExclusive(&g_lock);
    return slot;
}

// 0 = ask Windows, else the error to answer with. Outside loads only rom\ paths are answered.
DWORD known_missing(LPCWSTR name, DWORD access, DWORD disp, bool loading, bool& rom)
{
    if (!name || disp != OPEN_EXISTING) return 0;
    if (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER)) return 0;
    if (wcschr(name, L'*') || wcschr(name, L'?') || wcsncmp(name, L"\\\\", 2) == 0) return 0;
    wchar_t full[MAX_PATH];
    DWORD n = GetFullPathNameW(name, MAX_PATH, full, nullptr);
    if (!n || n >= MAX_PATH) return 0;
    std::wstring p(full, n);
    if (!ascii_lower(p) || p.find(L':', 2) != std::wstring::npos) return 0; // streams, odd names
    int root = -1;
    for (int i = 0; i < 3; i++) {
        const std::wstring& r = g_roots[i];
        if (!r.empty() && p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == L'\\') root = i;
    }
    rom = root == 0;
    if (root < 0 || (!rom && !loading)) return 0;
    size_t cut = p.find_last_of(L'\\');
    std::wstring dir = p.substr(0, cut), file = p.substr(cut + 1);
    if (file.empty() || file.back() == L'.' || file.back() == L' ') return 0;
    DirPtr d = listing(dir, rom, root == 2);
    if (!d->exists) return ERROR_PATH_NOT_FOUND;
    if (d->names.count(L"*") || d->names.count(file)) return 0;
    return ERROR_FILE_NOT_FOUND;
}

// rom\ lookups between loads: counted, logged at most once a minute while they happen.
void log_play_stats(bool hit)
{
    (hit ? g_play_hits : g_play_passed)++;
    unsigned long long now = GetTickCount64(), last = g_play_logged_at.load(std::memory_order_relaxed);
    if (now - last < 60000 || !g_play_logged_at.compare_exchange_strong(last, now)) return;
    AcquireSRWLockShared(&g_lock);
    size_t folders = g_rom_dirs.size();
    ReleaseSRWLockShared(&g_lock);
    swlog("file cache: between loads, %d missing rom files answered from listings, %d rom opens passed to Windows "
          "(%zu rom folders listed)", int(g_play_hits.exchange(0)), int(g_play_passed.exchange(0)), folders);
}

HANDLE WINAPI create_detour(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                            HANDLE tmpl)
{
    if (!g_on.load(std::memory_order_relaxed)) return g_create_orig(name, access, share, sa, disp, flags, tmpl);
    bool loading = g_loading.load(std::memory_order_relaxed) != 0, rom = false;
    DWORD err = known_missing(name, access, disp, loading, rom);
    if (!loading && rom) log_play_stats(err != 0);
    if (!err) {
        if (loading) g_passed++;
        return g_create_orig(name, access, share, sa, disp, flags, tmpl);
    }
    if (g_verify_left.fetch_sub(1) > 0) {
        HANDLE h = g_create_orig(name, access, share, sa, disp, flags, tmpl);
        if (h != INVALID_HANDLE_VALUE) {
            g_on = false;
            swlog("file cache: MISMATCH, %ls exists but wasn't in its folder's listing - cache off for this session", name);
            return h;
        }
        g_verified++;
    }
    if (loading) g_hits++;
    SetLastError(err);
    return INVALID_HANDLE_VALUE;
}

void run_detour(void* task)
{
    if (g_loading.fetch_add(1) == 0) {
        g_hits = g_passed = g_listed = g_verified = g_relisted = 0;
    }
    g_run_orig(task);
    if (g_loading.fetch_sub(1) == 1) {
        AcquireSRWLockExclusive(&g_lock);
        g_dirs.clear();
        ReleaseSRWLockExclusive(&g_lock);
        if (g_hits || g_passed)
            swlog("file cache: %d missing files answered from %d folder listings (%d of them checked with Windows, all "
                  "agreed; %d mods_temp folders listed again after a change), %d opens passed to Windows%s",
                  int(g_hits), int(g_listed), int(g_verified), int(g_relisted), int(g_passed), g_on ? "" : " [off after a mismatch]");
    }
}

bool install()
{
    const wchar_t* ini = settings_ini();
    if (!GetPrivateProfileIntW(L"load", L"missing_file_cache", 1, ini)) {
        swlog("file cache: off in settings.ini");
        return true;
    }
    g_verify_left = int(GetPrivateProfileIntW(L"load", L"verify_misses", 500, ini));
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    dir.resize(dir.find_last_of(L'\\'));
    if (!ascii_lower(dir)) {
        swlog("file cache: game folder has non-ASCII characters, off");
        return true;
    }
    g_roots[0] = dir + L"\\rom";
    size_t common = dir.rfind(L"\\steamapps\\common\\");
    if (common != std::wstring::npos) g_roots[1] = dir.substr(0, common) + L"\\steamapps\\workshop\\content\\573090";
    wchar_t appdata[MAX_PATH];
    DWORD an = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (an && an < MAX_PATH) {
        std::wstring m = std::wstring(appdata, an) + L"\\stormworks\\working\\mods_temp";
        if (ascii_lower(m)) g_roots[2] = m;
    }
    if (!hook_api(L"kernelbase.dll", "CreateFileW", reinterpret_cast<void*>(&create_detour), reinterpret_cast<void**>(&g_create_orig)) ||
        !hook_game(kStateCreateRun, reinterpret_cast<void*>(&run_detour), reinterpret_cast<void**>(&g_run_orig)))
        return false;
    swlog("file cache: missing files under %ls, %ls and %ls answered from folder listings (rom: always, workshop and "
          "mods_temp: during loads; first %d checked)",
          g_roots[0].c_str(), g_roots[1].empty() ? L"(no workshop folder)" : g_roots[1].c_str(),
          g_roots[2].empty() ? L"(no mods_temp folder)" : g_roots[2].c_str(), int(g_verify_left));
    return true;
}

HookRegistrar reg{{"missing-file cache", /*needs_known_build=*/true, &install}};

} // namespace
