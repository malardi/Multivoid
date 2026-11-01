// coop/dev/lab_window.cpp -- see coop/dev/lab_window.h.

#include "coop/dev/lab_window.h"

#include "ue_wrap/core/log.h"

#include <windows.h>
#include <shobjidl.h>

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

namespace coop::dev::lab_window {
namespace {

std::atomic<bool> g_started{false};
std::atomic<bool> g_hidden{false};
std::mutex g_mu;
// Where the engine had each window before it was parked, so `window show` can put it back.
std::map<HWND, POINT> g_home;

std::vector<HWND> ProcessWindows() {
    struct Ctx { DWORD pid; std::vector<HWND> out; } ctx{::GetCurrentProcessId(), {}};
    ::EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            ::GetWindowThreadProcessId(h, &pid);
            if (pid == c->pid && ::IsWindowVisible(h)) c->out.push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

// Just past the right edge of every monitor, at the top of the desktop.
POINT ParkSpot() {
    return POINT{::GetSystemMetrics(SM_XVIRTUALSCREEN) + ::GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64,
                 ::GetSystemMetrics(SM_YVIRTUALSCREEN)};
}

ITaskbarList* Taskbar() {
    static ITaskbarList* s = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (SUCCEEDED(::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList,
                                         reinterpret_cast<void**>(&s))) &&
            FAILED(s->HrInit())) {
            s->Release();
            s = nullptr;
        }
    }
    return s;
}

// The parking is done by the window itself: its procedure is subclassed, and every WM_WINDOWPOSCHANGING
// while hidden has its position rewritten to the parking spot and its activation dropped. The engine's
// own moves (the restore from the minimised start, the resolution's centring) then land off-screen in
// the same call that makes them, on the game thread, with no frame drawn where the owner would see it.
// A first try moved the window from the watcher instead, and lost: a move sent to a game thread busy
// loading waited ten seconds, while the engine centred its window on the screen.
const wchar_t* const kPrevProp = L"MultivoidLabPrevProc";

void RememberHome(HWND h, int x, int y) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_home.count(h)) g_home[h] = POINT{x, y};
}

LRESULT CALLBACK ParkProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const auto prev = reinterpret_cast<WNDPROC>(::GetPropW(h, kPrevProp));
    if (msg == WM_WINDOWPOSCHANGING && g_hidden.load(std::memory_order_acquire)) {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        const POINT spot = ParkSpot();
        // -32000 is a minimise's own placement: left alone (the watcher restores it).
        if (!(pos->flags & SWP_NOMOVE) && pos->x > -30000 && pos->x != spot.x) {
            RememberHome(h, pos->x, pos->y);
            pos->x = spot.x;
            pos->y = spot.y;
        }
        pos->flags |= SWP_NOACTIVATE;
    }
    return prev ? ::CallWindowProcW(prev, h, msg, wp, lp) : ::DefWindowProcW(h, msg, wp, lp);
}

// One window taken in hand: subclassed once, restored if minimised (the launcher starts the process
// so; ShowWindowAsync, so the watcher never waits on the game thread), moved to the spot if it stands
// anywhere else, and its taskbar button removed.
void Park(HWND h, POINT spot) {
    if (!::GetPropW(h, kPrevProp)) {
        const LONG_PTR old = ::GetWindowLongPtrW(h, GWLP_WNDPROC);
        if (!old || !::SetPropW(h, kPrevProp, reinterpret_cast<HANDLE>(old))) return;
        ::SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ParkProc));
        if (ITaskbarList* tb = Taskbar()) tb->DeleteTab(h);
        UE_LOGI("[LAB] window %p taken in hand: kept off-screen at (%ld,%ld)", static_cast<void*>(h), spot.x, spot.y);
    }
    // An asynchronous request waits in the queue of a game thread that is busy loading: one a second
    // per window, not one a pass (only this thread calls Park).
    static std::map<HWND, ULONGLONG> lastAsk;
    const ULONGLONG now = ::GetTickCount64();
    if (now - lastAsk[h] < 1000) return;
    if (::IsIconic(h)) {
        ::ShowWindowAsync(h, SW_SHOWNOACTIVATE);
        lastAsk[h] = now;
        return;
    }
    RECT rc{};
    if (::GetWindowRect(h, &rc) && rc.left != spot.x) {
        lastAsk[h] = now;
        RememberHome(h, rc.left, rc.top);
        ::SetWindowPos(h, nullptr, spot.x, spot.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        if (ITaskbarList* tb = Taskbar()) tb->DeleteTab(h);
    }
}

DWORD WINAPI Watcher(LPVOID) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // the taskbar list is an STA object
    for (;;) {
        if (g_hidden.load(std::memory_order_acquire)) {
            const POINT spot = ParkSpot();
            for (HWND h : ProcessWindows()) Park(h, spot);
            ::Sleep(15);
        } else {
            ::Sleep(250);
        }
    }
}

// A console with no window, for the console programs the game's mods start. The game is a GUI process
// without a console, so a child like the Fusion mod's `cmd /c Fusion.exe` (run at every launch) got a
// console of its own, and with Windows Terminal as the default terminal that is a Terminal window
// that takes the focus for half a second: the owner's video lost it at every lab launch. A child
// inherits its parent's console instead of making one, so the copy holds a windowless one.
// AllocConsoleWithOptions is Windows 11 24H2's (kernel32, looked up: older systems lack it, and there
// the copy goes without, as before). Declared here, the SDK this builds with may predate it.
void HoldWindowlessConsole() {
    struct AllocOptions { int mode; BOOL useShowWindow; WORD showWindow; };  // ALLOC_CONSOLE_OPTIONS
    using AllocFn = HRESULT(WINAPI*)(AllocOptions*, int*);
    const auto alloc = reinterpret_cast<AllocFn>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "AllocConsoleWithOptions")));
    if (!alloc) {
        UE_LOGW("[LAB] lab_window: no AllocConsoleWithOptions here; a mod's console program may show a window");
        return;
    }
    AllocOptions opt{2 /*ALLOC_CONSOLE_MODE_NO_WINDOW*/, FALSE, 0};
    int result = 0;  // ALLOC_CONSOLE_RESULT: 0 none, 1 new, 2 an existing one
    const HRESULT hr = alloc(&opt, &result);
    UE_LOGI("[LAB] lab_window: windowless console for child programs (hr=0x%08lX result=%d)",
            static_cast<unsigned long>(hr), result);
}

}  // namespace

void Start() {
    char v[8] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_LAB_HIDE", v, sizeof(v)) == 0 || v[0] != '1') return;
    if (g_started.exchange(true)) return;
    HoldWindowlessConsole();
    g_hidden.store(true, std::memory_order_release);
    if (HANDLE t = ::CreateThread(nullptr, 0, &Watcher, nullptr, 0, nullptr)) {
        ::CloseHandle(t);
        UE_LOGI("[LAB] lab_window: VOTVCOOP_LAB_HIDE=1 -- this copy's windows stay off-screen");
    } else {
        g_started.store(false);
        g_hidden.store(false);
    }
}

bool Hidden() { return g_hidden.load(std::memory_order_acquire); }

bool SetHidden(bool hidden) {
    if (!g_started.load()) return false;
    g_hidden.store(hidden, std::memory_order_release);
    if (hidden) return true;  // the watcher parks them on its next pass
    std::map<HWND, POINT> home;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        home.swap(g_home);
    }
    for (const auto& [h, at] : home) {
        if (!::IsWindow(h)) continue;
        ::SetWindowPos(h, nullptr, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        // AddTab is a COM call from this (game) thread: an apartment of its own for the one call.
        const HRESULT co = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ITaskbarList* tb = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList,
                                         reinterpret_cast<void**>(&tb)))) {
            if (SUCCEEDED(tb->HrInit())) tb->AddTab(h);
            tb->Release();
        }
        if (SUCCEEDED(co)) ::CoUninitialize();
    }
    return true;
}

void* MainWindow() {
    HWND best = nullptr;
    LONG bestArea = 0;
    for (HWND h : ProcessWindows()) {
        RECT rc{};
        if (!::GetClientRect(h, &rc)) continue;
        const LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
        if (area > bestArea) { bestArea = area; best = h; }
    }
    return best;
}

int PostText(const std::wstring& text) {
    HWND h = static_cast<HWND>(MainWindow());
    if (!h) return 0;
    struct Named { const wchar_t* name; UINT vk; };
    static const Named kNamed[] = {
        {L"{enter}", VK_RETURN}, {L"{tab}", VK_TAB},   {L"{back}", VK_BACK},   {L"{esc}", VK_ESCAPE},
        {L"{up}", VK_UP},        {L"{down}", VK_DOWN}, {L"{left}", VK_LEFT},   {L"{right}", VK_RIGHT},
        {L"{space}", VK_SPACE},  {L"{del}", VK_DELETE},
    };
    int posted = 0;
    for (size_t i = 0; i < text.size();) {
        bool named = false;
        if (text[i] == L'{') {
            for (const Named& n : kNamed) {
                const size_t len = wcslen(n.name);
                if (text.compare(i, len, n.name) != 0) continue;
                // A key, as the keyboard sends it: down (repeat 1, the scan code) and up (previous-state
                // and transition bits); the edit box's commit and the menus' navigation read these.
                const LPARAM scan = static_cast<LPARAM>(::MapVirtualKeyW(n.vk, MAPVK_VK_TO_VSC)) << 16;
                posted += ::PostMessageW(h, WM_KEYDOWN, n.vk, 1 | scan) ? 1 : 0;
                if (n.vk == VK_RETURN || n.vk == VK_TAB || n.vk == VK_BACK || n.vk == VK_ESCAPE || n.vk == VK_SPACE)
                    posted += ::PostMessageW(h, WM_CHAR, n.vk == VK_SPACE ? L' ' : static_cast<WPARAM>(n.vk), 1 | scan) ? 1 : 0;
                posted += ::PostMessageW(h, WM_KEYUP, n.vk, 1 | scan | (1LL << 30) | (1LL << 31)) ? 1 : 0;
                i += len;
                named = true;
                break;
            }
        }
        if (named) continue;
        posted += ::PostMessageW(h, WM_CHAR, text[i], 1) ? 1 : 0;
        ++i;
    }
    return posted;
}

}  // namespace coop::dev::lab_window
