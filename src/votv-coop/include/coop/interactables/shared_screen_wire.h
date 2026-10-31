// coop/interactables/shared_screen_wire.h -- the SharedScreen blob: the base computer's desktop rows and the
// envelope that says whose screen they are.
//
// envelope [u8 canonical][u32 lease][u32 revision][u8 owner] then the rows:
//   [u16 n]{ [u8 pathLen][path ascii][u8 visibility][u8 flags][i32 tab][f32 scroll][u16 chars]{u16 utf16} }
// `canonical` is 1 on the host's broadcast and 0 on a client operator's report. The lease names one operator's
// tenure (the host mints a new one whenever the laptop's holder changes) and the revision orders snapshots
// within it. `owner` is the operator's slot, 0xFF when nobody holds the laptop and the screen is the host's own.
// A decode reads the whole blob or refuses it: a duplicate path, a path outside the desktop's grammar, a member
// out of range or a trailing byte refuses all of it.
//
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/include/coop/interactables/shared_screen_wire.h (MIT);
// the ceiling is this tree's blob transport's.

#pragma once

#include "coop/net/blob_chunks.h"
#include "ue_wrap/desk/shared_screen.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace coop::shared_screen_wire {

using ue_wrap::shared_screen::Row;

inline constexpr size_t kEnvelopeBytes = 10;
// The whole blob rides one chunked send, so the rows are held below the transport's ceiling.
inline constexpr size_t kMaxBytes = coop::blob_chunks::MaxBlobBytes() - kEnvelopeBytes;
inline constexpr size_t kMaxRows = 512;
inline constexpr size_t kMaxPath = 128;
inline constexpr size_t kMaxChars = 4096;

template <class T>
void Put(std::vector<uint8_t>& bytes, T value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(T));
}

// Empty when the rows cannot be represented (none, too many, a member out of range, past the ceiling).
inline std::vector<uint8_t> Encode(const std::vector<Row>& rows) {
    if (rows.empty() || rows.size() > kMaxRows) return {};
    std::vector<uint8_t> bytes;
    Put<uint16_t>(bytes, static_cast<uint16_t>(rows.size()));
    for (const Row& row : rows) {
        if (row.path.empty() || row.path.size() > kMaxPath || row.text.size() > kMaxChars ||
            row.visibility > 4 || row.flags > 7 || !std::isfinite(row.scroll)) return {};
        Put<uint8_t>(bytes, static_cast<uint8_t>(row.path.size()));
        bytes.insert(bytes.end(), row.path.begin(), row.path.end());
        Put<uint8_t>(bytes, row.visibility);
        Put<uint8_t>(bytes, row.flags);
        Put<int32_t>(bytes, row.tab);
        Put<float>(bytes, row.scroll);
        Put<uint16_t>(bytes, static_cast<uint16_t>(row.text.size()));
        for (wchar_t ch : row.text) Put<uint16_t>(bytes, static_cast<uint16_t>(ch));
        if (bytes.size() > kMaxBytes) return {};
    }
    return bytes;
}

struct Cursor {
    const uint8_t* p;
    size_t n;
    size_t at = 0;
    template <class T>
    bool Get(T& value) {
        if (sizeof(T) > n - at) return false;
        std::memcpy(&value, p + at, sizeof(T));
        at += sizeof(T);
        return true;
    }
};

inline bool Decode(const uint8_t* data, size_t size, std::vector<Row>& rows) {
    rows.clear();
    if (size > kMaxBytes) return false;
    Cursor c{data, size};
    uint16_t count = 0;
    if (!c.Get(count) || count == 0 || count > kMaxRows) return false;
    std::set<std::string> paths;
    std::vector<Row> result;
    result.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        Row row;
        uint8_t length = 0;
        uint16_t chars = 0;
        if (!c.Get(length) || length == 0 || length > kMaxPath || length > c.n - c.at) return false;
        row.path.assign(reinterpret_cast<const char*>(c.p + c.at), length);
        c.at += length;
        if (!paths.insert(row.path).second || row.path.rfind("pc", 0) != 0 ||
            row.path.find_first_not_of("pc/r0123456789") != std::string::npos) return false;
        if (!c.Get(row.visibility) || !c.Get(row.flags) || !c.Get(row.tab) || !c.Get(row.scroll) ||
            row.visibility > 4 || row.flags > 7 || row.tab < -1 || row.tab > 512 ||
            !std::isfinite(row.scroll) || row.scroll < 0 || row.scroll > 1000000 ||
            !c.Get(chars) || chars > kMaxChars) return false;
        row.text.reserve(chars);
        for (uint16_t k = 0; k < chars; ++k) {
            uint16_t ch = 0;
            if (!c.Get(ch)) return false;
            row.text.push_back(static_cast<wchar_t>(ch));
        }
        result.push_back(std::move(row));
    }
    if (c.at != size) return false;
    rows = std::move(result);
    return true;
}

struct Snapshot {
    bool     canonical = false;
    uint32_t lease = 0;
    uint32_t revision = 0;
    uint8_t  owner = 0xFF;
    std::vector<Row> rows;
};

inline std::vector<uint8_t> EncodeSnapshot(bool canonical, uint32_t lease, uint32_t revision, uint8_t owner,
                                           const std::vector<uint8_t>& rows) {
    if (!lease || !revision || rows.empty() || rows.size() > kMaxBytes) return {};
    std::vector<uint8_t> bytes;
    bytes.reserve(kEnvelopeBytes + rows.size());
    Put<uint8_t>(bytes, canonical ? 1 : 0);
    Put<uint32_t>(bytes, lease);
    Put<uint32_t>(bytes, revision);
    Put<uint8_t>(bytes, owner);
    bytes.insert(bytes.end(), rows.begin(), rows.end());
    return bytes;
}

inline bool DecodeSnapshot(const std::vector<uint8_t>& bytes, Snapshot& out) {
    if (bytes.size() <= kEnvelopeBytes || bytes.size() > kMaxBytes + kEnvelopeBytes || bytes[0] > 1) return false;
    Snapshot result;
    result.canonical = bytes[0] != 0;
    std::memcpy(&result.lease, bytes.data() + 1, 4);
    std::memcpy(&result.revision, bytes.data() + 5, 4);
    result.owner = bytes[9];
    if (!result.lease || !result.revision ||
        !Decode(bytes.data() + kEnvelopeBytes, bytes.size() - kEnvelopeBytes, result.rows)) return false;
    out = std::move(result);
    return true;
}

}  // namespace coop::shared_screen_wire
