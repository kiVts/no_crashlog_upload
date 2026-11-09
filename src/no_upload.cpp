
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "sdk.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

constexpr char kHost[] = "geometa.co.uk";

std::atomic<int> g_blocked{0};
void* g_orig = nullptr;

// Runs on the crash handler's upload thread while the game is crashing: no logging, no allocation.
void upload_detour() { g_blocked.fetch_add(1, std::memory_order_relaxed); }

struct Section {
    uintptr_t lo = 0, hi = 0;
};

bool sections(Section& text, Section& rdata)
{
    uintptr_t base = game_base();
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        Section s{base + sec[i].VirtualAddress, base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize};
        if (!memcmp(sec[i].Name, ".text", 6)) text = s;
        if (!memcmp(sec[i].Name, ".rdata", 7)) rdata = s;
    }
    return text.lo && rdata.lo;
}

// The exact string (with its terminator and a terminator before it, so "https://geometa.co.uk/..." doesn't count).
uintptr_t find_host(const Section& rdata)
{
    const size_t n = sizeof kHost; // includes the terminator
    uintptr_t found = 0;
    for (uintptr_t p = rdata.lo + 1; p + n <= rdata.hi; p++) {
        if (*reinterpret_cast<const char*>(p - 1) != 0 || memcmp(reinterpret_cast<const void*>(p), kHost, n)) continue;
        if (found) return 0; // ambiguous
        found = p;
    }
    return found;
}

// lea r64, [rip + disp32] (REX.W 8D /r with mod 00, rm 101) whose target is the string.
int find_lea(const Section& text, uintptr_t target, uintptr_t& at)
{
    int hits = 0;
    for (uintptr_t p = text.lo; p + 7 <= text.hi; p++) {
        auto b = reinterpret_cast<const uint8_t*>(p);
        if ((b[0] != 0x48 && b[0] != 0x4c) || b[1] != 0x8d || (b[2] & 0xc7) != 0x05) continue;
        int32_t disp;
        memcpy(&disp, b + 3, 4);
        if (p + 7 + disp == target) at = p, hits++;
    }
    return hits;
}

// Function start of the code at a, following chained unwind entries (a code chunk) to the primary one.
uintptr_t function_start(uintptr_t a)
{
    DWORD64 image = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(a, &image, nullptr);
    for (int depth = 0; rf && depth < 8; depth++) {
        auto info = reinterpret_cast<const uint8_t*>(image + rf->UnwindData);
        if (!((info[0] >> 3) & 4)) return static_cast<uintptr_t>(image + rf->BeginAddress); // no UNW_FLAG_CHAININFO
        uint8_t codes = info[2];
        auto chained = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + ((codes + 1) & ~1) * 2);
        rf = const_cast<PRUNTIME_FUNCTION>(chained);
    }
    return 0;
}

bool install()
{
    if (!GetPrivateProfileIntW(L"crash", L"block_upload", 1, settings_ini())) {
        swlog("crash upload: allowed in settings.ini (block_upload=0)");
        return true;
    }
    Section text, rdata;
    if (!sections(text, rdata)) return false;
    uintptr_t host = find_host(rdata);
    if (!host) {
        swlog("crash upload: no \"%s\" string in this build: nothing to block", kHost);
        mod_status("nothing to block in this build");
        return true;
    }
    uintptr_t lea = 0;
    int hits = find_lea(text, host, lea);
    uintptr_t fn = hits == 1 ? function_start(lea) : 0;
    if (!fn) {
        swlog("crash upload: the uploader isn't recognised (%d references to the host string); NOT blocked", hits);
        mod_status("NOT blocking: crash uploader not recognised (see no_crash_upload.log)");
        return false;
    }
    if (!hook_address(fn, reinterpret_cast<void*>(&upload_detour), &g_orig)) {
        mod_status("NOT blocking: hook failed (see no_crash_upload.log)");
        return false;
    }
    swlog("crash upload: blocked (uploader at game+%llx returns at once; reports stay in the crashes folder only)",
          static_cast<unsigned long long>(fn - game_base()));
    return true;
}

HookRegistrar reg{{"block crash report upload", /*needs_known_build=*/false, &install}};

} // namespace
