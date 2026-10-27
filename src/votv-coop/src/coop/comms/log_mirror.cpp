// coop/comms/log_mirror.cpp -- see coop/comms/log_mirror.h.

#include "coop/comms/log_mirror.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace coop::log_mirror {
namespace {

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_threadStarted{false};

constexpr int kFiles = 2;               // 0: multivoid.log, 1: UE4SS.log
constexpr uint64_t kJoinTail = 512 * 1024;
constexpr int kChunksPerTick = 9;       // ~2 KB per 100 ms tick: about 20 KB/s

std::wstring PathOf(int file) {
    if (file == 0) return ue_wrap::log::CurrentPath();
    const std::wstring dir = ue_wrap::paths::Ue4ssDir();
    return dir.empty() ? std::wstring() : dir + L"\\UE4SS.log";
}

// ---- the client's side: its own thread ------------------------------------------------------
struct Stream { bool started = false; uint64_t offset = 0; };

bool ReadAt(const std::wstring& path, uint64_t offset, void* buf, DWORD want, DWORD& got, uint64_t& size) {
    got = 0;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    ::GetFileSizeEx(h, &sz);
    size = static_cast<uint64_t>(sz.QuadPart);
    bool ok = true;
    if (want && offset < size) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        ok = ::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && ::ReadFile(h, buf, want, &got, nullptr);
    }
    ::CloseHandle(h);
    return ok;
}

void ClientLoop() {
    Stream streams[kFiles];
    std::wstring paths[kFiles];
    bool wasLive = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        coop::net::Session* s = g_session.load(std::memory_order_acquire);
        const bool live = s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
        if (!live) {
            if (wasLive) for (auto& st : streams) st = Stream{};
            wasLive = false;
            continue;
        }
        wasLive = true;
        int budget = kChunksPerTick;
        for (int f = 0; f < kFiles && budget > 0; ++f) {
            if (paths[f].empty()) paths[f] = PathOf(f);
            if (paths[f].empty()) continue;
            Stream& st = streams[f];
            uint64_t size = 0;
            DWORD got = 0;
            char probe[1];
            if (!ReadAt(paths[f], 0, probe, 0, got, size)) continue;
            if (!st.started) {
                st.started = true;
                st.offset = (f == 0 && size > kJoinTail) ? size - kJoinTail : 0;
            }
            if (size < st.offset) st.offset = 0;  // the file began again
            while (budget > 0 && st.offset < size) {
                coop::net::LogChunkPayload p{};
                p.offset = static_cast<uint32_t>(st.offset);
                p.file = static_cast<uint8_t>(f);
                const DWORD want = static_cast<DWORD>(
                    (size - st.offset) < sizeof(p.data) ? (size - st.offset) : sizeof(p.data));
                if (!ReadAt(paths[f], st.offset, p.data, want, got, size) || got == 0) break;
                p.len = static_cast<uint8_t>(got);
                const int len = static_cast<int>(offsetof(coop::net::LogChunkPayload, data) + got);
                if (!s->SendReliable(coop::net::ReliableKind::LogChunk, &p, len)) break;
                st.offset += got;
                --budget;
            }
        }
    }
}

// ---- the host's side: the game thread -----------------------------------------------------------
struct Sink { FILE* f = nullptr; uint64_t expect = 0; bool begun = false; uint32_t sinceFlush = 0; };
Sink g_sinks[coop::net::kMaxPeers][kFiles];

void CloseAll() {
    for (auto& row : g_sinks)
        for (auto& k : row) {
            if (k.f) std::fclose(k.f);
            k = Sink{};
        }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_threadStarted.exchange(true)) return;
    // Never joined: it only sleeps and reads while no session runs, and the process ends it.
    std::thread(&ClientLoop).detach();
}

void OnChunk(int slot, const void* payload, size_t len) {
    if (slot < 1 || slot >= coop::net::kMaxPeers) return;
    constexpr size_t kHead = offsetof(coop::net::LogChunkPayload, data);
    if (len < kHead) return;
    coop::net::LogChunkPayload p{};
    std::memcpy(&p, payload, len < sizeof(p) ? len : sizeof(p));
    if (p.file >= kFiles || p.len > sizeof(p.data) || kHead + p.len > len) return;
    Sink& k = g_sinks[slot][p.file];
    if (!k.f) {
        const std::wstring dir = ue_wrap::paths::ExeDir() + L"\\coop_logs";
        ::CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring path = dir + L"\\peer" + std::to_wstring(slot) + (p.file == 0 ? L"_multivoid.log" : L"_UE4SS.log");
        k.f = _wfopen(path.c_str(), L"wb");
        if (!k.f) return;
        UE_LOGI("log_mirror: peer slot %d's %ls arrives in coop_logs", slot, p.file == 0 ? L"multivoid.log" : L"UE4SS.log");
    }
    if (k.begun && p.offset != k.expect) {
        char note[96];
        const int n = std::snprintf(note, sizeof(note), "\n[log_mirror: the stream jumped from %llu to %u]\n",
                                    static_cast<unsigned long long>(k.expect), p.offset);
        std::fwrite(note, 1, static_cast<size_t>(n), k.f);
    }
    k.begun = true;
    std::fwrite(p.data, 1, p.len, k.f);
    k.expect = static_cast<uint64_t>(p.offset) + p.len;
    if (++k.sinceFlush >= 16) { std::fflush(k.f); k.sinceFlush = 0; }
}

void OnDisconnect() { CloseAll(); }

}  // namespace coop::log_mirror
