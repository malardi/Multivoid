// ue_wrap/desk/shared_screen.cpp -- see ue_wrap/desk/shared_screen.h.
//
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/ue_wrap/desk/shared_screen.cpp (MIT): the widget
// is found through this tree's laptop_widget, the per-class offsets are cached, and a text already written is
// not written again (each write mints an engine FText, see ue_wrap/core/ftext_utils.h).

#include "ue_wrap/desk/shared_screen.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/devices/laptop_widget.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace ue_wrap::shared_screen {
namespace {

namespace R = reflection;

constexpr size_t  kMaxWidgets = 512;
constexpr int     kMaxDepth   = 20;
constexpr size_t  kMaxText    = 4096;

// ESlateVisibility: Collapsed and Hidden draw nothing, so nothing under them is walked.
constexpr uint8_t kCollapsed = 1;
constexpr uint8_t kHidden    = 2;
constexpr uint8_t kMaxVisibility = 4;

// A member's offset, per (class object, member): the text block's `Text`, a user widget's `WidgetTree`,
// the tree's `RootWidget`.
std::map<std::pair<void*, const wchar_t*>, int32_t> g_offsets;
// What Apply last wrote to a text block, by path.
std::map<std::string, std::wstring> g_written;

void* Fn(void* object, const wchar_t* name) {
    return R::FindDispatchFunctionCached(R::ClassOf(object), name);
}

template <class T>
bool Get(void* object, const wchar_t* name, T& value) {
    ParamFrame frame(Fn(object, name));
    return frame.valid() && Call(object, frame) &&
           frame.GetRaw(L"ReturnValue", &value, static_cast<int32_t>(sizeof(value)));
}

template <class T>
bool Set(void* object, const wchar_t* name, const wchar_t* param, T value) {
    ParamFrame frame(Fn(object, name));
    return frame.valid() && frame.Set<T>(param, value) && Call(object, frame);
}

// Is `object`'s class `name` or derived from it.
bool Is(void* object, const wchar_t* name) {
    void* cls = R::ClassOf(object);
    for (int guard = 0; cls && guard < 32; ++guard) {
        if (R::NameEquals(R::NameOf(cls), name)) return true;
        cls = R::SuperStructOf(cls);
    }
    return false;
}

void* PointerAt(void* object, size_t off) {
    void* p = nullptr;
    std::memcpy(&p, static_cast<const uint8_t*>(object) + off, sizeof(p));
    return p && R::IsLive(p) ? p : nullptr;
}

// `name`'s offset on `object`'s class, resolved once per class; `fallback` when the class does not name it
// (the engine members are UPROPERTYs in this build, and the profile constants are the same build's).
int32_t MemberOffset(void* object, const wchar_t* name, int32_t fallback) {
    const auto key = std::make_pair(R::ClassOf(object), name);
    auto it = g_offsets.find(key);
    if (it != g_offsets.end()) return it->second;
    int32_t off = R::FindPropertyOffset(key.first, name);
    if (off < 0) off = fallback;
    g_offsets.emplace(key, off);
    return off;
}

void* PointerAtMember(void* object, int32_t off) {
    return off >= 0 ? PointerAt(object, static_cast<size_t>(off)) : nullptr;
}

void Walk(void* widget, const std::string& path, int depth, std::map<std::string, void*>& out) {
    if (!widget || !R::IsLive(widget) || depth > kMaxDepth || out.size() >= kMaxWidgets) return;
    out.emplace(path, widget);
    uint8_t visibility = 0;
    if (Get(widget, L"GetVisibility", visibility) && (visibility == kCollapsed || visibility == kHidden)) return;
    if (Is(widget, L"UserWidget")) {
        const auto treeOff = static_cast<int32_t>(profile::off::UUserWidget_WidgetTree);
        const auto rootOff = static_cast<int32_t>(profile::off::UWidgetTree_RootWidget);
        if (void* tree = PointerAtMember(widget, MemberOffset(widget, L"WidgetTree", treeOff)))
            Walk(PointerAtMember(tree, MemberOffset(tree, L"RootWidget", rootOff)), path + "/r", depth + 1, out);
    }
    int32_t count = 0;
    if (!Get(widget, L"GetChildrenCount", count) || count < 0 || count > static_cast<int32_t>(kMaxWidgets)) return;
    int32_t active = -1;
    const bool switcher = Is(widget, L"WidgetSwitcher") && Get(widget, L"GetActiveWidgetIndex", active);
    for (int32_t i = 0; i < count; ++i) {
        if (switcher && i != active) continue;
        ParamFrame frame(Fn(widget, L"GetChildAt"));
        void* child = nullptr;
        if (frame.valid() && frame.Set<int32_t>(L"Index", i) && Call(widget, frame) &&
            frame.GetRaw(L"ReturnValue", &child, static_cast<int32_t>(sizeof(child))))
            Walk(child, path + "/" + std::to_string(i), depth + 1, out);
    }
}

// The desktop's widgets by path. rootswitch's page 0 is the boot UI the laptop's own startup drives; page 1 is
// the desktop, and only it is mirrored -- neither the switcher nor the boot page is read or written.
std::map<std::string, void*> Widgets() {
    std::map<std::string, void*> widgets;
    void* ui = laptop_widget::Widget();
    if (!ui) return widgets;
    void* root = PointerAtMember(ui, MemberOffset(ui, L"rootswitch", -1));
    if (!root) return widgets;
    ParamFrame frame(Fn(root, L"GetChildAt"));
    void* desktop = nullptr;
    if (frame.valid() && frame.Set<int32_t>(L"Index", 1) && Call(root, frame) &&
        frame.GetRaw(L"ReturnValue", &desktop, static_cast<int32_t>(sizeof(desktop))))
        Walk(desktop, "pc", 0, widgets);
    return widgets;
}

bool ReadRow(void* widget, Row& row) {
    if (!Get(widget, L"GetVisibility", row.visibility)) return false;
    if (Is(widget, L"WidgetSwitcher") && Get(widget, L"GetActiveWidgetIndex", row.tab)) row.flags |= kHasTab;
    if (Is(widget, L"ScrollBox") && Get(widget, L"GetScrollOffset", row.scroll)) row.flags |= kHasScroll;
    if (Is(widget, L"TextBlock")) {
        const int32_t off = MemberOffset(widget, L"Text", -1);
        if (off >= 0) {
            row.text = ftext_utils::FTextToString(static_cast<const uint8_t*>(widget) + off);
            if (row.text.size() <= kMaxText) row.flags |= kHasText;
            else row.text.clear();
        }
    }
    return row.visibility <= kMaxVisibility && (!(row.flags & kHasScroll) || std::isfinite(row.scroll));
}

}  // namespace

bool Read(std::vector<Row>& rows) {
    rows.clear();
    for (const auto& [path, widget] : Widgets()) {
        Row row;
        row.path = path;
        if (!ReadRow(widget, row)) { rows.clear(); return false; }
        rows.push_back(std::move(row));
    }
    return !rows.empty();
}

bool Apply(const std::vector<Row>& rows) {
    auto widgets = Widgets();
    if (widgets.empty()) return false;
    bool ok = true;
    for (const Row& row : rows) {
        const auto it = widgets.find(row.path);
        if (it == widgets.end()) { ok = false; continue; }
        void* widget = it->second;
        Row current;
        if (!R::IsLive(widget) || !ReadRow(widget, current)) { ok = false; continue; }
        if (row.visibility != current.visibility)
            ok = Set<uint8_t>(widget, L"SetVisibility", L"InVisibility", row.visibility) && ok;
        // The page is the engine's UWidgetSwitcher::SetActiveWidgetIndex alone: the Blueprint's own
        // switcherSwitched would createRC and bind this viewer's keys to a remote camera.
        if ((row.flags & current.flags & kHasTab) && row.tab != current.tab) {
            int32_t count = 0;
            if (Get(widget, L"GetChildrenCount", count) && row.tab >= 0 && row.tab < count)
                ok = Set<int32_t>(widget, L"SetActiveWidgetIndex", L"Index", row.tab) && ok;
            else
                ok = false;
        }
        if ((row.flags & current.flags & kHasScroll) && std::fabs(row.scroll - current.scroll) > 0.5f)
            ok = Set<float>(widget, L"SetScrollOffset", L"NewScrollOffset", row.scroll) && ok;
        if ((row.flags & current.flags & kHasText) && row.text != current.text) {
            auto written = g_written.find(row.path);
            if (written != g_written.end() && written->second == row.text) continue;  // ours already; a binding holds it
            uint8_t text[ftext_utils::kFTextSize]{};
            ParamFrame frame(Fn(widget, L"SetText"));
            const bool set = frame.valid() && ftext_utils::MintFText(row.text.c_str(), text) &&
                             frame.SetRaw(L"InText", text, static_cast<int32_t>(sizeof(text))) && Call(widget, frame);
            if (set) g_written[row.path] = row.text;
            ok = set && ok;
        }
    }
    return ok;
}

void ResetWritten() {
    g_written.clear();
    g_offsets.clear();   // class objects are world-scoped like the widget
}

}  // namespace ue_wrap::shared_screen
