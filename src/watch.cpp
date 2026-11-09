
#include "watch.h"

#include "game.h"
#include "log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kSites = 64, kDepth = 6, kEvents = 96;
constexpr DWORD64 kDr7Mask = 0xf0003; // L0/G0 + RW0/LEN0
constexpr DWORD64 kDr7Watch = 0xd0001; // L0, RW0 = 01 (write), LEN0 = 11 (4 bytes)

struct Site {
    uint64_t chain[kDepth]; // [0] = instruction after the write, then callers
    int count, up, down, other, first_value, last_value;
    DWORD tid;
    bool reported;
};
struct Event {
    int64_t qpc;
    int site, value;
};

Site g_sites[kSites];
std::atomic<int> g_nsites{0}, g_dropped{0}, g_nevents{0};
Event g_events[kEvents];
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
std::atomic<int> g_last_value{0};

std::atomic<uintptr_t> g_addr{0};     // watching now
std::atomic<uintptr_t> g_retired[4]{}; // stopped watching: still ours if a trap comes in
int g_retired_next = 0;
const char* g_name = "?";
int64_t g_armed_at = 0;
LARGE_INTEGER g_freq{};
PVOID g_veh = nullptr;

bool ours(uintptr_t a)
{
    if (a && a == g_addr.load()) return true;
    for (auto& r : g_retired)
        if (a && a == r.load()) return true;
    return false;
}

void unwind(const CONTEXT* trap, uint64_t* chain)
{
    CONTEXT c = *trap;
    chain[0] = c.Rip;
    __try {
        for (int i = 1; i < kDepth; i++) {
            DWORD64 image = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
            if (fe) {
                void* handler_data;
                DWORD64 frame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fe, &c, &handler_data, &frame, nullptr);
            } else { // leaf: return address on top of the stack
                c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                c.Rsp += 8;
            }
            if (!c.Rip) break;
            chain[i] = c.Rip;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void record(const CONTEXT* c)
{
    uint64_t chain[kDepth]{};
    unwind(c, chain);
    int value = *reinterpret_cast<volatile int*>(g_addr.load());
    int prev = g_last_value.exchange(value);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    while (g_lock.test_and_set(std::memory_order_acquire)) YieldProcessor();
    int n = g_nsites.load(), idx = -1;
    for (int i = 0; i < n; i++)
        if (memcmp(g_sites[i].chain, chain, sizeof(chain)) == 0) idx = i;
    if (idx < 0 && n < kSites) {
        idx = n;
        Site& s = g_sites[idx];
        memset(&s, 0, sizeof(s));
        memcpy(s.chain, chain, sizeof(chain));
        s.first_value = value;
        g_nsites.store(n + 1);
    }
    if (idx >= 0) {
        Site& s = g_sites[idx];
        s.count++;
        if (value == prev + 1) s.up++;
        else if (value == prev - 1) s.down++;
        else s.other++;
        s.last_value = value;
        s.tid = GetCurrentThreadId();
        int e = g_nevents.load();
        if (e < kEvents) {
            g_events[e] = {now.QuadPart, idx, value};
            g_nevents.store(e + 1);
        }
    } else {
        g_dropped.fetch_add(1);
    }
    g_lock.clear(std::memory_order_release);
}

LONG CALLBACK veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 1) || !ours(static_cast<uintptr_t>(c->Dr0))) return EXCEPTION_CONTINUE_SEARCH;
    if (c->Dr0 == g_addr.load()) record(c);
    else c->Dr7 &= ~kDr7Mask; // a retired watch this thread still had: switch it off
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct Pass {
    uintptr_t addr; // 0 = switch off
    int done = 0, skipped = 0, failed = 0;
};

DWORD WINAPI apply_all(void* p)
{
    Pass& pass = *static_cast<Pass*>(p);
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) {
            pass.failed++;
            continue;
        }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        bool set = false, busy = false;
        if (SuspendThread(t) != DWORD(-1)) {
            if (GetThreadContext(t, &c)) {
                bool dr0_used = (c.Dr7 & 3) && !ours(static_cast<uintptr_t>(c.Dr0));
                if (dr0_used) {
                    busy = true; // someone else's breakpoint (a debugger): leave it
                } else {
                    c.Dr7 &= ~kDr7Mask;
                    if (pass.addr) {
                        c.Dr0 = pass.addr;
                        c.Dr7 |= kDr7Watch;
                    }
                    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    set = SetThreadContext(t, &c) != 0;
                }
            }
            ResumeThread(t);
        }
        CloseHandle(t);
        if (set) pass.done++;
        else if (busy) pass.skipped++;
        else pass.failed++;
    }
    CloseHandle(snap);
    return 0;
}

Pass run_pass(uintptr_t addr)
{
    Pass pass{addr};
    HANDLE h = CreateThread(nullptr, 0, &apply_all, &pass, 0, nullptr);
    if (h) {
        WaitForSingleObject(h, 5000);
        CloseHandle(h);
    }
    return pass;
}

void describe(uint64_t a, char* out, size_t n)
{
    uintptr_t base = game_base();
    HMODULE game = reinterpret_cast<HMODULE>(base), mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(a), &mod);
    if (mod && mod == game) {
        snprintf(out, n, "%llx", static_cast<unsigned long long>(a - base + kGhidraImageBase));
    } else if (mod) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* file = wcsrchr(path, L'\\');
        snprintf(out, n, "%ls+%llx", file ? file + 1 : path, static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(mod)));
    } else {
        snprintf(out, n, "?%llx", static_cast<unsigned long long>(a));
    }
}

void log_site(int i, const Site& s, bool counts)
{
    char line[512];
    int len = snprintf(line, sizeof(line), "watch %s: writer #%d at", g_name, i);
    for (int d = 0; d < kDepth && s.chain[d] && len < int(sizeof(line)) - 48; d++) {
        char a[96];
        describe(s.chain[d], a, sizeof(a));
        len += snprintf(line + len, sizeof(line) - len, d ? " <- %s" : " %s", a);
    }
    if (counts)
        swlog("%s: %d writes (+1 x%d, -1 x%d, other x%d), values %d .. %d, thread %lu", line, s.count, s.up, s.down,
              s.other, s.first_value, s.last_value, s.tid);
    else
        swlog("%s (first write: value %d, thread %lu)", line, s.first_value, s.tid);
}

} // namespace

void watch_arm(void* address, const char* name)
{
    uintptr_t a = reinterpret_cast<uintptr_t>(address);
    if (!a || (a & 3)) {
        swlog("watch %s: address %p not 4-byte aligned, not watched", name, address);
        return;
    }
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &veh);
    bool fresh = g_addr.load() != a;
    if (fresh) {
        if (g_addr.load()) watch_disarm();
        g_name = name;
        g_nsites.store(0);
        g_nevents.store(0);
        g_dropped.store(0);
        g_last_value.store(*reinterpret_cast<volatile int*>(a));
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_armed_at = now.QuadPart;
        g_addr.store(a);
    }
    Pass p = run_pass(a);
    if (fresh)
        swlog("watch %s: watching writes to %p (value %d) on %d threads (%d with a debugger's breakpoint, %d failed)", name,
              address, g_last_value.load(), p.done, p.skipped, p.failed);
}

void watch_report(bool final)
{
    if (!g_addr.load() && !final) return;
    int n = g_nsites.load();
    for (int i = 0; i < n; i++) {
        if (final) {
            log_site(i, g_sites[i], true);
        } else if (!g_sites[i].reported) {
            g_sites[i].reported = true;
            log_site(i, g_sites[i], false);
        }
    }
    if (!final) return;
    // The order of the writes: "time ms:writer#=value", up to kEvents of them.
    int e = g_nevents.load();
    char line[1024];
    int len = 0;
    for (int i = 0; i < e; i++) {
        len += snprintf(line + len, sizeof(line) - len, " %.0f:#%d=%d",
                        double(g_events[i].qpc - g_armed_at) * 1000.0 / double(g_freq.QuadPart), g_events[i].site,
                        g_events[i].value);
        if (len > int(sizeof(line)) - 40 || i == e - 1) {
            swlog("watch %s: sequence%s", g_name, line);
            len = 0;
        }
    }
    swlog("watch %s: %d writers, %d writes in the sequence%s%s", g_name, n, e, e >= kEvents ? " (full)" : "",
          g_dropped.load() ? " - writer table full, some writes not recorded" : "");
}

void watch_disarm()
{
    uintptr_t a = g_addr.exchange(0);
    if (!a) return;
    g_retired[g_retired_next++ % 4].store(a);
    Pass p = run_pass(0);
    swlog("watch %s: stopped (%d threads)", g_name, p.done);
}
