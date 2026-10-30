// coop/dev/lab_cmd.cpp -- see coop/dev/lab_cmd.h.

#include "coop/dev/lab_cmd.h"

#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/session/teleport_client.h"
#include "coop/world/dimension_travel.h"
#include "harness/screenshot.h"
#include "harness/session_runtime.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace coop::dev::lab_cmd {
namespace {

namespace R = ue_wrap::reflection;

bool g_init = false, g_on = false;
std::wstring g_dir, g_tag;
long long g_lastId = 0;
FILETIME g_lastWrite{};
std::chrono::steady_clock::time_point g_nextPoll{}, g_nextState{};

std::wstring Env(const wchar_t* name) {
    wchar_t buf[512] = {};
    const DWORD n = ::GetEnvironmentVariableW(name, buf, 512);
    return (n > 0 && n < 512) ? std::wstring(buf, n) : std::wstring();
}

std::wstring PathOf(const wchar_t* suffix) { return g_dir + L"\\" + g_tag + suffix; }

void Answer(long long id, bool ok, const std::string& text) {
    if (FILE* f = _wfopen(PathOf(L"_out.txt").c_str(), L"ab")) {
        std::fprintf(f, "%lld %s %s\r\n", id, ok ? "ok" : "fail", text.c_str());
        std::fclose(f);
    }
    UE_LOGI("[LAB] #%lld %s %s", id, ok ? "ok" : "FAIL", text.c_str());
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c > 0 && c < 0x80 ? static_cast<char>(c) : '?');
    return s;
}

bool CallNoParams(void* obj, const wchar_t* fnName) {
    if (!obj) return false;
    void* fn = R::FindFunction(R::ClassOf(obj), fnName);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(obj, f);
}

void Run(long long id, const std::string& verb, const std::string& rest) {
    std::istringstream in(rest);
    void* local = coop::players::Registry::Get().Local();
    if (verb == "tp") {
        float x = 0, y = 0, z = 0, yaw = 0;
        in >> x >> y >> z >> yaw;
        const bool ok = coop::teleport_client::ApplyLocally({x, y, z, 0.f, yaw, 0.f});
        Answer(id, ok, "tp " + rest);
    } else if (verb == "travel") {
        std::string level;
        in >> level;
        const bool ok = coop::dimension_travel::TravelAsGameEntry(std::wstring(level.begin(), level.end()));
        Answer(id, ok, "travel " + level);
    } else if (verb == "exec") {
        const std::wstring cmd(rest.begin(), rest.end());
        Answer(id, ue_wrap::engine::ExecuteConsoleCommand(cmd.c_str()), "exec " + rest);
    } else if (verb == "shot") {
        const std::wstring label(rest.begin(), rest.end());
        Answer(id, harness::screenshot::Capture(label.c_str()), "shot " + rest);
    } else if (verb == "callp" || verb == "callgm") {
        std::string fn;
        in >> fn;
        void* obj = verb == "callp" ? local : ue_wrap::world_singleton::Gamemode();
        Answer(id, CallNoParams(obj, std::wstring(fn.begin(), fn.end()).c_str()), verb + " " + fn);
    } else if (verb == "log") {
        UE_LOGI("[LAB] ---- %s ----", rest.c_str());
        Answer(id, true, "log");
    } else {
        Answer(id, false, "unknown verb '" + verb + "'");
    }
}

void Poll() {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!::GetFileAttributesExW(PathOf(L"_cmd.txt").c_str(), GetFileExInfoStandard, &a)) return;
    if (::CompareFileTime(&a.ftLastWriteTime, &g_lastWrite) == 0) return;
    g_lastWrite = a.ftLastWriteTime;
    std::ifstream f(PathOf(L"_cmd.txt"));
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        long long id = 0;
        std::string verb;
        if (!(ls >> id >> verb) || id <= g_lastId) continue;
        g_lastId = id;
        std::string rest;
        std::getline(ls, rest);
        const size_t b = rest.find_first_not_of(' ');
        rest = b == std::string::npos ? std::string() : rest.substr(b);
        Run(id, verb, rest);
    }
}

void WriteState() {
    std::ostringstream o;
    void* world = ue_wrap::world_identity::CurrentWorld();
    o << "time " << ::GetTickCount64() << "\r\n";
    o << "world " << (world ? Narrow(R::ToString(R::NameOf(world))) : std::string("-")) << " gen "
      << ue_wrap::world_identity::Generation() << "\r\n";
    o << "dimension " << (coop::dimension_travel::InDimension() ? 1 : 0) << "\r\n";
    coop::net::Session& s = harness::session_runtime::Session();
    o << "session running=" << (s.running() ? 1 : 0) << " connected=" << (s.connected() ? 1 : 0)
      << " role=" << (s.role() == coop::net::Role::Host ? "host" : "client") << "\r\n";
    if (void* local = coop::players::Registry::Get().Local()) {
        ue_wrap::FVector l{};
        ue_wrap::engine::TryGetActorLocation(local, l);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "local %.0f %.0f %.0f\r\n", l.X, l.Y, l.Z);
        o << buf;
    } else {
        o << "local -\r\n";
    }
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        auto& p = coop::puppet_drive::Puppet(slot);
        if (!p.valid()) continue;
        ue_wrap::FVector l{};
        p.TryGetLocation(l);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "puppet %d %.0f %.0f %.0f\r\n", slot, l.X, l.Y, l.Z);
        o << buf;
    }
    const std::wstring tmp = PathOf(L"_state.tmp");
    if (FILE* f = _wfopen(tmp.c_str(), L"wb")) {
        const std::string t = o.str();
        std::fwrite(t.data(), 1, t.size(), f);
        std::fclose(f);
        ::MoveFileExW(tmp.c_str(), PathOf(L"_state.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

}  // namespace

void Tick() {
    if (!g_init) {
        g_init = true;
        g_dir = Env(L"VOTVCOOP_LAB_DIR");
        g_tag = Env(L"VOTVCOOP_LAB_TAG");
        g_on = !g_dir.empty() && !g_tag.empty();
        if (g_on) {
            // Commands already in the file belong to an earlier launch.
            std::ifstream f(PathOf(L"_cmd.txt"));
            std::string line;
            while (std::getline(f, line)) {
                const long long id = std::atoll(line.c_str());
                if (id > g_lastId) g_lastId = id;
            }
            UE_LOGI("[LAB] the lab channel is on: %ls\\%ls_cmd.txt (commands after #%lld)", g_dir.c_str(),
                    g_tag.c_str(), g_lastId);
        }
    }
    if (!g_on) return;
    const auto now = std::chrono::steady_clock::now();
    if (now >= g_nextPoll) { g_nextPoll = now + std::chrono::milliseconds(200); Poll(); }
    if (now >= g_nextState) { g_nextState = now + std::chrono::seconds(1); WriteState(); }
}

}  // namespace coop::dev::lab_cmd
