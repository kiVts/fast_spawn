#include "game.h"
#include "hooks.h"
#include "log.h"
#include "resolve.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

int g_mode = 1;
LARGE_INTEGER g_freq{};

struct CallState {
    const uint8_t* owner = nullptr;
    uint64_t sum = 0;
    uint32_t bodies = 0, links_first = 0;
    uint64_t run = 0, skipped = 0, changed_skippable = 0;
    LONGLONG start = 0;
    bool prev_skippable = false;
    uint64_t prev_hash = 0;
    bool logged = false;
};
thread_local CallState t_call;

struct Ring {
    uintptr_t data;
    uint32_t cap, head, count;
};
Ring ring_at(const uint8_t* p)
{
    Ring r;
    memcpy(&r.data, p, 8);
    memcpy(&r.cap, p + 8, 4);
    memcpy(&r.head, p + 0xc, 4);
    memcpy(&r.count, p + 0x10, 4);
    return r;
}

uint64_t component_sum(const uint8_t* owner)
{
    Ring b = ring_at(owner);
    if (!b.data || !b.cap) return 0;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < b.count; i++) {
        auto body = *reinterpret_cast<const uint8_t* const*>(b.data + uint64_t((b.head + i) % b.cap) * 8);
        if (body) sum += *reinterpret_cast<const uint32_t*>(body + 0x400);
    }
    return sum;
}

uint64_t link_hash(const uint8_t* owner)
{
    Ring l = ring_at(owner + 0x50);
    uint64_t h = 1469598103934665603ull ^ l.count;
    if (!l.data || !l.cap) return h;
    for (uint32_t i = 0; i < l.count; i++) {
        auto e = reinterpret_cast<const uint8_t*>(l.data + uint64_t((l.head + i) % l.cap) * 0x30);
        for (int k = 0; k < 0x30; k++) h = (h ^ e[k]) * 1099511628211ull;
    }
    return h;
}

void finish(CallState& s)
{
    if (!s.owner || s.logged || s.bodies < 8) return;
    s.logged = true;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double ms = double(now.QuadPart - s.start) * 1000.0 / double(g_freq.QuadPart);
    if (g_mode == 2) {
        swlog("link pass (verify): %u bodies, %u links, %llu passes run, %llu would have been skipped; %llu of those "
              "changed the links%s; %.0f ms to the last pass",
              s.bodies, s.links_first, static_cast<unsigned long long>(s.run),
              static_cast<unsigned long long>(s.skipped), static_cast<unsigned long long>(s.changed_skippable),
              s.changed_skippable ? " (NOT SAFE TO SKIP)" : "", ms);
    } else {
        swlog("link pass: %u bodies, %u links, %llu passes run, %llu repeats skipped; %.0f ms to the last pass",
              s.bodies, s.links_first, static_cast<unsigned long long>(s.run),
              static_cast<unsigned long long>(s.skipped), ms);
    }
}

// Called from the stub at the start of every link pass (links present). true = run it.
extern "C" bool __cdecl link_pass_decide(const uint8_t* owner, const uint8_t* site_rsp)
{
    CallState& s = t_call;
    uint32_t outer = *reinterpret_cast<const uint32_t*>(site_rsp + 0x28);
    uint64_t sum = component_sum(owner);
    Ring b = ring_at(owner);
    if (outer == 0 || s.owner != owner) {
        finish(s);
        s = CallState{};
        s.owner = owner;
        s.bodies = b.count;
        s.links_first = ring_at(owner + 0x50).count;
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&s.start));
        s.sum = sum;
        s.run = 1;
        if (g_mode == 2) {
            s.prev_hash = link_hash(owner);
            s.prev_skippable = false;
        }
        if (b.count == 1) finish(s);
        return true;
    }
    bool skippable = sum == s.sum;
    bool last = outer + 1 >= b.count;
    bool run = true;
    if (g_mode == 2) {
        uint64_t h = link_hash(owner);
        if (s.prev_skippable && h != s.prev_hash) s.changed_skippable++;
        s.prev_skippable = skippable;
        s.prev_hash = h;
        if (skippable) s.skipped++;
        s.run++;
    } else if (skippable) {
        s.skipped++;
        run = false;
    } else {
        s.run++;
    }
    s.sum = sum;
    if (last) finish(s);
    return run;
}

bool bytes_at(const uint8_t* p, const char* hex)
{
    for (; *hex; hex++) {
        if (*hex == ' ') continue;
        auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        if (*p++ != uint8_t(nib(hex[0]) << 4 | nib(hex[1]))) return false;
        hex++;
    }
    return true;
}

const uint8_t* find_in(const uint8_t* lo, const uint8_t* hi, const char* hex, size_t len)
{
    const uint8_t* found = nullptr;
    for (const uint8_t* p = lo; p + len <= hi; p++) {
        if (!bytes_at(p, hex)) continue;
        if (found) return nullptr;
        found = p;
    }
    return found;
}

uint8_t* alloc_near(uintptr_t target, size_t size)
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    for (uintptr_t d = si.dwAllocationGranularity; d < 0x70000000; d += si.dwAllocationGranularity) {
        for (uintptr_t a : {target - d, target + d}) {
            a &= ~(uintptr_t(si.dwAllocationGranularity) - 1);
            if (auto* m = static_cast<uint8_t*>(
                    VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
                return m;
        }
    }
    return nullptr;
}

// Writes the stub into mem (a page within +-2 GB of site, PAGE_READWRITE) and the jump over the 10 site bytes.
bool link_pass_patch(uint8_t* site, const uint8_t* after, uint8_t* mem)
{
    uint8_t c[160];
    size_t n = 0;
    auto put = [&](const char* hex) {
        for (; *hex; hex++) {
            if (*hex == ' ') continue;
            auto nib = [](char ch) { return ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10; };
            c[n++] = uint8_t(nib(hex[0]) << 4 | nib(hex[1]));
            hex++;
        }
    };
    bool range = true;
    auto rel_to = [&](const uint8_t* target) {
        intptr_t d = reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(mem + n + 4);
        range = range && d == int32_t(d);
        int32_t r = int32_t(d);
        memcpy(c + n, &r, 4);
        n += 4;
    };
    put("83 7e 60 00");
    put("0f 86");            // jbe after_links
    rel_to(after);
    put("55 48 89 e5");      // push rbp; mov rbp,rsp
    put("50 51 52 41 50 41 51 41 52 41 53");
    put("48 83 e4 f0 48 81 ec 80 00 00 00"); // and rsp,-16; sub rsp,0x80
    put("f3 0f 7f 44 24 20 f3 0f 7f 4c 24 30 f3 0f 7f 54 24 40"); // movdqu [rsp+20..40],xmm0-2
    put("f3 0f 7f 5c 24 50 f3 0f 7f 64 24 60 f3 0f 7f 6c 24 70"); // xmm3-5
    put("48 89 f1 48 8d 55 08");
    put("48 b8");                // mov rax, decide
    uint64_t fn = reinterpret_cast<uint64_t>(&link_pass_decide);
    memcpy(c + n, &fn, 8);
    n += 8;
    put("ff d0 84 c0");          // call rax; test al,al
    put("f3 0f 6f 44 24 20 f3 0f 6f 4c 24 30 f3 0f 6f 54 24 40");
    put("f3 0f 6f 5c 24 50 f3 0f 6f 64 24 60 f3 0f 6f 6c 24 70");
    put("48 8d 65 c8");          // lea rsp,[rbp-0x38]
    put("41 5b 41 5a 41 59 41 58 5a 59 58 5d");
    put("0f 84");                // jz after_links (skip this pass)
    rel_to(after);
    put("e9");                   // jmp link loop
    rel_to(site + 10);
    intptr_t back = reinterpret_cast<intptr_t>(mem) - reinterpret_cast<intptr_t>(site + 5);
    if (!range || back != int32_t(back)) return false;
    memcpy(mem, c, n);
    DWORD old = 0;
    if (!VirtualProtect(mem, 0x1000, PAGE_EXECUTE_READ, &old)) return false;
    uint8_t jump[10] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    int32_t r = int32_t(back);
    memcpy(jump + 1, &r, 4);
    if (!VirtualProtect(site, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site, jump, sizeof(jump)); // at start-up, before any vehicle loads
    DWORD ignored;
    VirtualProtect(site, sizeof(jump), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), mem, n);
    return true;
}

bool install()
{
    g_mode = static_cast<int>(GetPrivateProfileIntW(L"load", L"link_pass", 1, settings_ini()));
    if (g_mode <= 0) {
        swlog("link pass: off in settings.ini");
        return true;
    }
    if (g_mode > 2) g_mode = 1;
    QueryPerformanceFrequency(&g_freq);

    uintptr_t hit = native_find_pattern("c7 44 24 20 00 00 00 00 83 7e 60 00 0f 86 ?? ?? ?? ?? 4c 8d 46 50");
    if (!hit) {
        swlog("link pass: the game's link section wasn't found (game update?), left as it is");
        return true;
    }
    auto* site = reinterpret_cast<uint8_t*>(hit + 8);
    uintptr_t fb = 0, fe = 0;
    if (!native_function_bounds(hit, &fb, &fe) || fe - fb > 0x4000) {
        swlog("link pass: no function bounds for the link section, left as it is");
        return true;
    }
    auto* lo = reinterpret_cast<const uint8_t*>(fb);
    auto* hi = reinterpret_cast<const uint8_t*>(fe);
    int32_t jbe_rel;
    memcpy(&jbe_rel, site + 6, 4);
    const uint8_t* after = site + 10 + jbe_rel;
    bool ok = find_in(lo, site, "89 4c 24 28 3b c8 0f 84", 8) &&
              find_in(lo, site, "8b 46 0c 03 c1 33 d2 f7 76 08 48 8b 06 4c 8b 04 d0", 17) &&
              find_in(lo, site, "41 39 88 00 04 00 00", 7) &&
              bytes_at(site + 10, "4c 8d 46 50") && bytes_at(site + 0x19, "41 8b 40 0c 03 44 24 20 33 d2 41 f7 70 08") &&
              bytes_at(site + 0x33, "44 8b 6e 10") && after > site && after < hi &&
              bytes_at(after, "8b 4c 24 28 ff c1 8b 44 24 38 e9");
    if (!ok) {
        swlog("link pass: the code around the link section differs from build 0x6ae31989, left as it is");
        return true;
    }

    uint8_t* mem = alloc_near(reinterpret_cast<uintptr_t>(site), 0x1000);
    if (!mem) {
        swlog("link pass: no memory near the game's code, left as it is");
        return true;
    }
    if (!link_pass_patch(site, after, mem)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        swlog("link pass: couldn't write the jump (memory out of range or protection), left as it is");
        return true;
    }
    swlog("link pass: %s (vehicle bodies' link list resolved once per change instead of once per body; game+%#llx)",
          g_mode == 2 ? "verify mode, every pass still runs" : "on",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(site) - game_base()));
    return true;
}

HookRegistrar reg{{"vehicle link pass", /*needs_known_build=*/false, &install}};

} // namespace
