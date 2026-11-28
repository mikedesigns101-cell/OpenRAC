// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The levels as the program reads them: each level's program, for the runtime's relocation of the
// functions levels share (load_level_program), and --check-levels, which reads every extracted
// level as the window would, optionally with --mods overrides (check_levels).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "common/breadcrumbs.h"
#include "common/log.h"
#include "openrac/elf.h"
#include "openrac/game_host.h"
#include "openrac/guest.h"
#include "openrac/memory.h"
#include "program/program.h"

#ifdef OPENRAC_HAS_DISC
#include "assets/disc/disc.h"
#include "assets/disc/wad.h"
#endif
#ifdef OPENRAC_FRONTEND
#include "scene/level.h"
#endif

namespace openrac::program {
namespace {

#ifdef OPENRAC_HAS_DISC
// Going Commando's programs (games/rac2/decomp/docs/build/levels.md): G/LEVELn.WAD, which holds level n but
// for the museum, level 30, in G/LEVEL21.WAD. Its word pair at +0x10 gives the level data's sector,
// which opens with the (offset, size) of the program. The image is opened once.
// The image, opened once (nullptr when it cannot be), and where level `level`'s data starts in it
// (0 when it has no such level).
assets::disc::IsoImage* rac2_iso() {
    static std::optional<assets::disc::IsoImage> iso;
    static bool tried = false;
    if (!tried) {
        tried = true;
        // The disc's files, read through their sector map (assets/disc/sector_map.h).
        const fs::path image = g_options.data / "iso_data" / openrac_game.game;
        try {
            iso.emplace(assets::disc::IsoImage::open(image));
        } catch (const std::exception& e) {
            log::warn("the disc image {} cannot be read: {}", image.string(), e.what());
        }
    }
    return iso ? &*iso : nullptr;
}

std::uint32_t rac2_word(const std::vector<std::uint8_t>& from, std::size_t at) {
    std::uint32_t value = 0;
    if (at + sizeof(value) <= from.size()) {
        std::memcpy(&value, from.data() + at, sizeof(value));
    }
    return value;
}

std::uint64_t rac2_level_data(assets::disc::IsoImage& iso, int level) {
    constexpr int kMuseum = 30;
    constexpr int kMuseumFile = 21;
    constexpr std::uint64_t kSector = 2048;
    const std::string name = std::format("G/LEVEL{}.WAD", level == kMuseum ? kMuseumFile : level);
    const assets::disc::IsoEntry* file = iso.find(name);
    if (file == nullptr) {
        return 0;
    }
    const std::uint64_t start = file->lba * kSector;
    const std::vector<std::uint8_t> head = iso.read_bytes(start, 0x18);
    if (static_cast<int>(rac2_word(head, 0x08)) != level) {
        return 0;
    }
    return start + rac2_word(head, 0x10) * kSector;
}

bool rac2_level_program(int level, std::vector<std::uint8_t>* bytes) {
    assets::disc::IsoImage* iso = rac2_iso();
    if (iso == nullptr) {
        return false;
    }
    try {
        const std::uint64_t data = rac2_level_data(*iso, level);
        if (data == 0) {
            return false;
        }
        const std::vector<std::uint8_t> slot = iso->read_bytes(data, 8);
        *bytes = iso->read_bytes(data + rac2_word(slot, 0), rac2_word(slot, 4));
        return !bytes->empty();
    } catch (const std::exception& e) {
        log::warn("level {}'s program cannot be read from the disc image: {}", level, e.what());
        return false;
    }
}

// A moby class of a level as its core holds it on the disc, before the game relocates it
// (LoadLevelArchiveResources, func_L00_00303990: the level data opens with (offset, size) pairs,
// the core index second and the core data tenth, WAD-compressed; the index's moby class table,
// +0x18 its count and +0x1C its 0x20-byte records, each +0 the block's offset in the core, +4 the
// class, +0x10 its texture map). A block ends where the next block of the core starts (the bounds
// extractor/level.py core_boundaries takes, with the index's +0x24, PrintMobyClassSizes's end).
// The last level's core is kept: several classes of one level read it once.
struct Rac2Class {
    int level = -1, oclass = -1;
    std::vector<std::uint8_t> bytes;
    std::uint8_t map[16] = {};
};

// The last level's core index and core data (rac2_level_class, rac2_level_hero_seq), read once.
int g_rac2_core_level = -1;
std::vector<std::uint8_t> g_rac2_index, g_rac2_core;

bool rac2_level_core(int level) {
    int& core_level = g_rac2_core_level;
    std::vector<std::uint8_t>& index = g_rac2_index;
    std::vector<std::uint8_t>& core = g_rac2_core;
    assets::disc::IsoImage* iso = rac2_iso();
    if (iso == nullptr) {
        return false;
    }
    try {
        if (core_level != level) {
            core_level = -1;
            index.clear();
            core.clear();
            const std::uint64_t data = rac2_level_data(*iso, level);
            if (data == 0) {
                return false;
            }
            const std::vector<std::uint8_t> slots = iso->read_bytes(data, 0x50);
            const auto section = [&](std::size_t pair) {
                std::vector<std::uint8_t> raw =
                    iso->read_bytes(data + rac2_word(slots, pair * 8), rac2_word(slots, pair * 8 + 4));
                return assets::disc::is_wad(raw) ? assets::disc::wad_decompress(raw) : raw;
            };
            index = section(1);
            core = section(9);
            if (index.size() < 0x90 || core.size() != rac2_word(index, 0x8C)) {
                log::warn("level {}'s core cannot be read from the disc image", level);
                index.clear();
                core.clear();
                return false;
            }
            core_level = level;
        }
        return true;
    } catch (const std::exception& e) {
        log::warn("level {}'s core cannot be read from the disc image: {}", level, e.what());
        core_level = -1;
        index.clear();
        core.clear();
        return false;
    }
}

// Where a block of level `level`'s core that starts at `at` ends: the next block's start (the
// index's sections, its moby, tie and shrub classes, and the hero's seqs), or the core's end.
std::uint64_t rac2_core_block_end(std::uint32_t at) {
    const std::vector<std::uint8_t>& index = g_rac2_index;
    std::uint64_t end = g_rac2_core.size();
    const auto bound = [&](std::uint64_t b) {
        if (b > at && b < end) {
            end = b;
        }
    };
    for (std::size_t w : {0x08, 0x0C, 0x10, 0x14, 0x24, 0x60}) {
        bound(rac2_word(index, w));
    }
    const std::size_t tables[3][2] = {{0x18, 0x20}, {0x20, 0x20}, {0x28, 0x30}};
    for (const auto& t : tables) {
        const std::uint32_t n = rac2_word(index, t[0]), first = rac2_word(index, t[0] + 4);
        for (std::uint32_t i = 0; i < n && first + (i + 1) * t[1] <= index.size(); ++i) {
            bound(rac2_word(index, first + i * t[1]));
        }
    }
    const std::uint32_t seqs = rac2_word(index, 0x78);
    for (std::uint32_t s = 0; s < 256 && seqs != 0 && seqs + (s + 1) * 4ull <= index.size(); ++s) {
        bound(rac2_word(index, seqs + s * 4));
    }
    return end;
}

// Seq `seq` of the hero's class as level `level`'s core holds it (LoadLevelArchiveResources: the
// index's +0x78, a word a seq, each its block's offset in the core or 0 for none; the class's own
// count of seqs bounds it). The block's pointers are offsets from its start
// (relocate_asset_entry_pointers, func_00293260), so it works anywhere once relocated.
bool rac2_level_hero_seq(int level, int seq, std::vector<std::uint8_t>* out) {
    if (seq < 0 || seq > 255 || !rac2_level_core(level)) {
        return false;
    }
    const std::vector<std::uint8_t>& index = g_rac2_index;
    const std::vector<std::uint8_t>& core = g_rac2_core;
    const std::uint32_t seqs = rac2_word(index, 0x78);
    if (seqs == 0 || seqs + (seq + 1) * 4ull > index.size()) {
        return false;
    }
    const std::uint32_t at = rac2_word(index, seqs + seq * 4u);
    if (at == 0 || at >= core.size()) {
        return false;
    }
    const std::uint64_t end = rac2_core_block_end(at);
    if (end - at < 0x20) {
        return false;
    }
    out->assign(core.begin() + at, core.begin() + static_cast<std::ptrdiff_t>(end));
    return true;
}

bool rac2_level_class(int level, int oclass, Rac2Class* out) {
    if (!rac2_level_core(level)) {
        return false;
    }
    const std::vector<std::uint8_t>& index = g_rac2_index;
    const std::vector<std::uint8_t>& core = g_rac2_core;
    try {
        const std::uint32_t count = rac2_word(index, 0x18), table = rac2_word(index, 0x1C);
        std::size_t record = SIZE_MAX;
        for (std::uint32_t i = 0; i < count && table + (i + 1) * 0x20ull <= index.size(); ++i) {
            if (static_cast<int>(rac2_word(index, table + i * 0x20 + 4)) == oclass) {
                record = table + i * 0x20;
            }
        }
        if (record == SIZE_MAX) {
            return false;
        }
        const std::uint32_t at = rac2_word(index, record);
        std::uint64_t end = core.size();
        const auto bound = [&](std::uint64_t b) {
            if (b > at && b < end) {
                end = b;
            }
        };
        for (std::size_t w : {0x08, 0x0C, 0x10, 0x14, 0x24, 0x60}) {
            bound(rac2_word(index, w));
        }
        const std::size_t tables[3][2] = {{0x18, 0x20}, {0x20, 0x20}, {0x28, 0x30}};
        for (const auto& t : tables) {
            const std::uint32_t n = rac2_word(index, t[0]), first = rac2_word(index, t[0] + 4);
            for (std::uint32_t i = 0; i < n && first + (i + 1) * t[1] <= index.size(); ++i) {
                bound(rac2_word(index, first + i * t[1]));
            }
        }
        if (at == 0 || at >= core.size() || end - at < 0x50) {
            return false;
        }
        out->level = level;
        out->oclass = oclass;
        out->bytes.assign(core.begin() + at, core.begin() + static_cast<std::ptrdiff_t>(end));
        std::memcpy(out->map, index.data() + record + 0x10, sizeof(out->map));
        return true;
    } catch (const std::exception& e) {
        log::warn("level {}'s class {} cannot be read from the disc image: {}", level, oclass, e.what());
        return false;
    }
}

// The first moby of a class a level's gameplay file places, as LoadLevelData reads it
// (func_L00_00300618): the level header's +0x20 (the file's first sector, from the level's, and its
// sectors), WAD-compressed; section +0x4C the records (a count, then from +0x10 records of the
// size their first word gives: +0x28 the class, +0x68 its pvars block or -1), +0x5C the blocks'
// (offset, size) in section +0x60, +0x64 the pointers inside blocks (block, offset; -1 ends) that
// the load turns into addresses. The record, its block and those pointers' offsets in it.
struct Rac2Record {
    int level = -1, oclass = -1;
    std::vector<std::uint8_t> record, pvars;
    std::vector<std::uint32_t> inner;
};

bool rac2_level_record(int level, int oclass, Rac2Record* out) {
    assets::disc::IsoImage* iso = rac2_iso();
    if (iso == nullptr) {
        return false;
    }
    try {
        constexpr std::uint64_t kSector = 2048;
        const std::string name = std::format("G/LEVEL{}.WAD", level);
        const assets::disc::IsoEntry* file = iso->find(name);
        if (file == nullptr) {
            return false;
        }
        const std::vector<std::uint8_t> head = iso->read_bytes(file->lba * kSector, 0x60);
        if (static_cast<int>(rac2_word(head, 0x08)) != level) {
            return false;
        }
        std::vector<std::uint8_t> raw = iso->read_bytes(
            (file->lba + rac2_word(head, 0x20)) * kSector, rac2_word(head, 0x24) * kSector
        );
        const std::vector<std::uint8_t> g =
            assets::disc::is_wad(raw) ? assets::disc::wad_decompress(raw) : std::move(raw);
        const std::uint32_t mobys = rac2_word(g, 0x4C), blocks = rac2_word(g, 0x5C);
        const std::uint32_t data = rac2_word(g, 0x60), inner = rac2_word(g, 0x64);
        const std::uint32_t count = rac2_word(g, mobys);
        std::size_t p = mobys + 0x10;
        for (std::uint32_t i = 0; i < count && p + 0x88 <= g.size(); ++i) {
            const std::uint32_t size = rac2_word(g, p) & ~3u;
            const std::int32_t block = static_cast<std::int32_t>(rac2_word(g, p + 0x68));
            if (size < 0x88) {
                return false;
            }
            if (static_cast<int>(rac2_word(g, p + 0x28)) == oclass && block >= 0) {
                const std::uint32_t off = rac2_word(g, blocks + block * 8u);
                const std::uint32_t bytes = rac2_word(g, blocks + block * 8u + 4u);
                if (std::uint64_t(data) + off + bytes > g.size()) {
                    return false;
                }
                out->level = level;
                out->oclass = oclass;
                out->record.assign(g.begin() + p, g.begin() + p + 0x88);
                out->pvars.assign(g.begin() + data + off, g.begin() + data + off + bytes);
                out->inner.clear();
                for (std::size_t q = inner; inner != 0 && q + 8 <= g.size()
                                            && static_cast<std::int32_t>(rac2_word(g, q)) >= 0;
                     q += 8) {
                    if (static_cast<std::int32_t>(rac2_word(g, q)) == block
                        && rac2_word(g, q + 4) + 4u <= bytes) {
                        out->inner.push_back(rac2_word(g, q + 4));
                    }
                }
                return true;
            }
            p += size;
        }
        return false;
    } catch (const std::exception& e) {
        log::warn("level {}'s gameplay file cannot be read from the disc image: {}", level, e.what());
        return false;
    }
}
#endif

#ifdef OPENRAC_HAS_DISC
// Up Your Arsenal's programs (extractor/wads.py, WadDisc and level_ranges, the editor's reader of the
// disc): its table of contents from sector 1001, global headers (each sized by its first word,
// the last padded with zeros) and then the level table, three (header sector, sector count) pairs
// a level. A level's header is 0x60 bytes: +4 the file's first sector, +8 the level's number,
// +0x10 the level data's sector in the file, which opens with the (offset, size) of the program,
// WAD-compressed. The functions every level carries are written with Veldin's (level 1's)
// addresses, so any level other than Veldin needs Veldin's program to pair them.
bool rac3_level_program(int level, std::vector<std::uint8_t>* bytes) {
    constexpr std::uint32_t kTocSector = 1001;
    constexpr std::uint64_t kSector = 2048;
    constexpr std::size_t kTocLimit = 0x200000;
    constexpr std::size_t kPair = 24;
    constexpr std::uint32_t kLevelHeader = 0x60;
    static std::optional<assets::disc::IsoImage> iso;
    static std::vector<std::uint8_t> toc;
    static bool tried = false;
    // The disc's files, read through their sector map (assets/disc/sector_map.h).
    const fs::path image = g_options.data / "iso_data" / openrac_game.game;

    try {
        if (!tried) {
            tried = true;
            iso.emplace(assets::disc::IsoImage::open(image));
            const std::uint64_t sectors = iso->sector_count();
            const std::uint64_t size = sectors > kTocSector ? (sectors - kTocSector) * kSector : 0;
            toc = iso->read_bytes(kTocSector * kSector, std::min<std::uint64_t>(kTocLimit, size));
        }
        if (!iso || toc.empty()) {
            return false;
        }

        const auto word = [](const std::vector<std::uint8_t>& from, std::size_t at) {
            std::int32_t value = 0;
            if (at + 4 <= from.size()) {
                std::memcpy(&value, from.data() + at, sizeof(value));
            }
            return value;
        };
        // The size of the header at sector `lba` of the table, 0 when it is not a header the
        // games use (wads.HEADER_KINDS).
        const auto header_size = [&](std::int32_t lba) -> std::uint32_t {
            if (lba <= static_cast<std::int32_t>(kTocSector)) {
                return 0;
            }
            const std::size_t at = (static_cast<std::size_t>(lba) - kTocSector) * kSector;
            const auto size = static_cast<std::uint32_t>(word(toc, at));
            switch (size) {
                case 0x60:
                case 0xC68:
                case 0x1018:
                case 0x1818:
                case 0x2A0:
                case 0x137C:
                case 0x26F0:
                    return at + 8 <= toc.size() ? size : 0;
                default:
                    return 0;
            }
        };

        // The level table: the first entry whose first two pairs point at headers and whose third
        // does too or is empty.
        std::size_t table = 0;
        for (;;) {
            if (table + kPair > toc.size()) {
                return false;
            }
            const std::int32_t first = word(toc, table), second = word(toc, table + 8),
                               third = word(toc, table + 16);
            if (header_size(first) != 0 && header_size(second) != 0
                && (third == 0 || header_size(third) != 0)) {
                break;
            }
            const std::int32_t size = word(toc, table);
            if (size == 0) {
                table += 4;
                continue;
            }
            if (size < 8 || size > 0x10000 || size % 4 != 0) {
                return false;
            }
            table += static_cast<std::size_t>(size);
        }

        for (std::size_t at = table; at + kPair <= toc.size() && at < table + 100 * kPair;
             at += kPair) {  // wads.MAX_LEVELS
            for (std::size_t pair = 0; pair < 3; ++pair) {
                const std::int32_t lba = word(toc, at + pair * 8);
                if (header_size(lba) != kLevelHeader) {
                    continue;
                }
                const std::size_t header = (static_cast<std::size_t>(lba) - kTocSector) * kSector;
                if (word(toc, header + 8) != level) {
                    continue;
                }
                const std::uint64_t data = (static_cast<std::uint64_t>(word(toc, header + 4))
                                            + static_cast<std::uint64_t>(word(toc, header + 0x10)))
                                           * kSector;
                const std::vector<std::uint8_t> slot = iso->read_bytes(data, 8);
                const std::int32_t offset = word(slot, 0), size = word(slot, 4);
                if (size <= 0) {
                    return false;
                }
                std::vector<std::uint8_t> raw = iso->read_bytes(data + offset, size);
                *bytes =
                    assets::disc::is_wad(raw) ? assets::disc::wad_decompress(raw) : std::move(raw);
                return !bytes->empty();
            }
        }
    } catch (const std::exception& e) {
        log::warn("level {}'s program cannot be read from {}: {}", level, image.string(), e.what());
    }
    return false;
}
#endif

// A level's program straight from the player's image (<data>/iso_data/<game>/disc.iso, kept by the
// set-up), for the games whose disc layout is known. The image is opened once.
bool level_program_from_disc(int level, std::vector<std::uint8_t>* bytes) {
#ifdef OPENRAC_HAS_DISC
    if (std::string(openrac_game.game) == "rac2") {
        return rac2_level_program(level, bytes);
    }
    if (std::string(openrac_game.game) == "rac3") {
        return rac3_level_program(level, bytes);
    }

    static std::optional<assets::disc::Disc> disc;
    static bool tried = false;

    if (!tried) {
        tried = true;
        // The disc's files, read through their sector map (assets/disc/sector_map.h).
        const fs::path image = g_options.data / "iso_data" / openrac_game.game;

        if (std::string(openrac_game.game) == "rac1") {
            try {
                disc.emplace(assets::disc::Disc::open(image, assets::disc::rac1_layout()));
            } catch (const std::exception& e) {
                log::warn(
                    "the level programs cannot be read from {}: {}", image.string(), e.what()
                );
            }
        }
    }

    if (!disc) {
        return false;
    }

    try {
        assets::disc::LevelFiles files = disc->level(static_cast<std::uint32_t>(level));

        if (files.overlay) {
            *bytes = std::move(*files.overlay);
            return true;
        }
    } catch (const std::exception& e) {
        log::warn("level {}'s program cannot be read from the disc's files: {}", level, e.what());
    }
#else
    (void)level;
    (void)bytes;
#endif
    return false;
}

}  // namespace

void load_level_program(int level) {
    crash::level_breadcrumb("level program load", level);
    char name[8];
    std::snprintf(name, sizeof(name), "%02d", level);
    const fs::path path =
        g_options.data / "decompiler_out" / openrac_game.game / "levels" / name / "overlay.bin";
    std::vector<std::uint8_t> bytes;

    if (!runtime::read_file(path, &bytes) && !level_program_from_disc(level, &bytes)) {
        crash::level_breadcrumb("level program missing", level);
        log::error(
            "level {}'s program is in neither {} nor the disc's files: the functions levels share "
            "are not "
            "relocated, and the game will misbehave (the hero's heading ignores the camera)",
            level,
            path.string()
        );
        return;
    }

    log::verbose("level {}'s program: {} bytes", level, bytes.size());
    crash::level_breadcrumb("level program relocation", level);
    std::uint32_t entry = 0;

    for (std::size_t at = 0; at + 16 <= bytes.size();) {
        std::uint32_t head[4];
        std::memcpy(head, bytes.data() + at, sizeof(head));

        if (entry == 0) {
            entry = head[3];
        }

        if (head[3] != entry || at + 16 + head[1] > bytes.size()) {
            break;
        }

        openrac_guest_add_level_program(level, head[0], bytes.data() + at + 16, head[1]);
        at += 16 + head[1];
    }
    crash::level_breadcrumb("level program ready", level);
}

extern "C" void openrac_game_clear_level_programs(void) {
    openrac_guest_clear_level_programs();
}

extern "C" int openrac_game_level_class(
    int level, int oclass, const uint8_t** bytes, uint32_t* size, uint8_t map[16]
) {
#ifdef OPENRAC_HAS_DISC
    // Each class read once a run: the disc is not read again at each load of the level.
    static std::vector<Rac2Class> classes;
    if (std::string(openrac_game.game) != "rac2" || bytes == nullptr || size == nullptr
        || map == nullptr) {
        return -1;
    }
    const Rac2Class* found = nullptr;
    for (const Rac2Class& c : classes) {
        if (c.level == level && c.oclass == oclass) {
            found = &c;
        }
    }
    if (found == nullptr) {
        Rac2Class c;
        if (!rac2_level_class(level, oclass, &c)) {
            classes.push_back(Rac2Class{level, oclass, {}, {}});  // not read again either
            return -1;
        }
        log::info("level {}'s moby class {}: {} bytes read from the disc image", level, oclass, c.bytes.size());
        classes.push_back(std::move(c));
        found = &classes.back();
    }
    if (found->bytes.empty()) {
        return -1;
    }
    *bytes = found->bytes.data();
    *size = static_cast<uint32_t>(found->bytes.size());
    std::memcpy(map, found->map, sizeof(found->map));
    return 0;
#else
    (void)level;
    (void)oclass;
    (void)bytes;
    (void)size;
    (void)map;
    return -1;
#endif
}

extern "C" int openrac_game_level_hero_seq(int level, int seq, const uint8_t** bytes, uint32_t* size) {
#ifdef OPENRAC_HAS_DISC
    // Each seq read once a run.
    struct Seq {
        int level, seq;
        std::vector<std::uint8_t> bytes;
    };
    static std::vector<Seq> seqs;
    if (std::string(openrac_game.game) != "rac2" || bytes == nullptr || size == nullptr) {
        return -1;
    }
    const Seq* found = nullptr;
    for (const Seq& s : seqs) {
        if (s.level == level && s.seq == seq) {
            found = &s;
        }
    }
    if (found == nullptr) {
        Seq s{level, seq, {}};
        if (rac2_level_hero_seq(level, seq, &s.bytes)) {
            log::info("level {}'s hero seq {}: {} bytes read from the disc image", level, seq, s.bytes.size());
        }
        seqs.push_back(std::move(s));
        found = &seqs.back();
    }
    if (found->bytes.empty()) {
        return -1;
    }
    *bytes = found->bytes.data();
    *size = static_cast<uint32_t>(found->bytes.size());
    return 0;
#else
    (void)level;
    (void)seq;
    (void)bytes;
    (void)size;
    return -1;
#endif
}

extern "C" int openrac_game_level_record(
    int level, int oclass, uint8_t record[0x88], const uint8_t** pvars, uint32_t* pvars_size,
    const uint32_t** inner, uint32_t* inner_count
) {
#ifdef OPENRAC_HAS_DISC
    static std::vector<Rac2Record> records;
    if (std::string(openrac_game.game) != "rac2" || record == nullptr || pvars == nullptr
        || pvars_size == nullptr || inner == nullptr || inner_count == nullptr) {
        return -1;
    }
    const Rac2Record* found = nullptr;
    for (const Rac2Record& r : records) {
        if (r.level == level && r.oclass == oclass) {
            found = &r;
        }
    }
    if (found == nullptr) {
        Rac2Record r;
        if (!rac2_level_record(level, oclass, &r)) {
            records.push_back(Rac2Record{level, oclass, {}, {}, {}});
            return -1;
        }
        log::info(
            "level {}'s first moby of class {}: {} bytes of pvars, {} pointers in them",
            level,
            oclass,
            r.pvars.size(),
            r.inner.size()
        );
        records.push_back(std::move(r));
        found = &records.back();
    }
    if (found->record.empty()) {
        return -1;
    }
    std::memcpy(record, found->record.data(), 0x88);
    *pvars = found->pvars.data();
    *pvars_size = static_cast<uint32_t>(found->pvars.size());
    *inner = found->inner.data();
    *inner_count = static_cast<uint32_t>(found->inner.size());
    return 0;
#else
    (void)level;
    (void)oclass;
    (void)record;
    (void)pvars;
    (void)pvars_size;
    (void)inner;
    (void)inner_count;
    return -1;
#endif
}

#ifdef OPENRAC_FRONTEND
int check_levels(const fs::path& levels, const fs::path& mods) {
    const fs::path stamp = levels / std::format("verified-{}.json", scene::kManifestFormat);
    std::error_code ignored;
    if (mods.empty()) {
        fs::remove(stamp, ignored);
        fs::remove(levels / "verified.json", ignored);  // the stamp before formats were named
    }
    std::vector<int> ids;

    if (std::string(openrac_game.game) == "rac1") {
        for (int i = 0; i < 19; ++i) {
            ids.push_back(i);
        }
        ids.push_back(98);
        ids.push_back(99);
    } else {
        // A later game's levels are what its disc has (extractor/extract.py port): every level
        // folder there. The export writes its stamp last, so a folder that is there is whole.
        for (const auto& entry : fs::directory_iterator(levels, ignored)) {
            int id = 0;
            const std::string name = entry.path().filename().string();
            // level_NN exactly: not a copy left beside it (level_05-old) or a hidden one.
            if (entry.is_directory() && std::sscanf(name.c_str(), "level_%d", &id) == 1
                && name == std::format("level_{:02d}", id)) {
                ids.push_back(id);
            }
        }
        std::sort(ids.begin(), ids.end());
        if (ids.empty()) {
            log::error("{} has no levels: read them from the disc first", levels.string());
            return 1;
        }
    }
    int bad = 0;
    int newer = 0;  // levels of a newer format than this game reads: the game is out of date

    for (const int id : ids) {
        const fs::path dir = levels / std::format("level_{:02d}", id);
        scene::LevelData level;
        std::string error;

        const fs::path edited =
            mods.empty() ? fs::path{} : mods / std::format("level_{:02d}", id) / "placements.json";
        if (!scene::load_level(dir, level, error, edited)) {
            log::error("level {:02d}: {}", id, error);
            ++bad;
            newer += error.find("newer than this game") != std::string::npos;
            continue;
        }

        // The flight world has only its ships' textures; every other one has ground to stand on.
        if (id != 98 && (level.vertices.empty() || level.models.empty())) {
            log::error(
                "level {:02d}: it has nothing to draw ({} models, {} vertices)",
                id,
                level.models.size(),
                level.vertices.size()
            );
            ++bad;
            continue;
        }

        if (level.missing_images > 0) {
            log::error("level {:02d}: {} of its textures are missing", id, level.missing_images);
            ++bad;
            continue;
        }

        log::info(
            "level {:02d}: {} models, {} vertices, {} textures",
            id,
            level.models.size(),
            level.vertices.size(),
            level.images.size()
        );
        if (level.overridden_models > 0 || level.overridden_images > 0) {
            log::info(
                "level {:02d}: loaded {} modded model(s) and {} modded texture(s)",
                id,
                level.overridden_models,
                level.overridden_images
            );
        }
    }

    if (newer > 0) {
        log::error(
            "{} of {} levels in {} are of a newer format than this game reads: build the game "
            "again (the "
            "launcher's \"Build the game\"; tools/build_port.py)",
            newer,
            ids.size(),
            levels.string()
        );
        return 1;
    }

    if (bad > 0) {
        if (mods.empty()) {
            log::error(
                "{} of {} levels in {} cannot be drawn: read them from the disc again (the "
                "launcher's "
                "\"Get the levels ready\"; extractor/extract.py port --replace)",
                bad,
                ids.size(),
                levels.string()
            );
        } else {
            log::error(
                "{} of {} modded levels cannot be drawn from {}: repair or remove the named mod "
                "files",
                bad,
                ids.size(),
                mods.string()
            );
        }
        return 1;
    }

    if (std::FILE* f = std::fopen(stamp.string().c_str(), "wb")) {
        const std::string text = std::format(
            "{{\"format\": {}, \"levels\": {}, \"program\": \"{}\"}}\n",
            scene::kManifestFormat,
            ids.size(),
            program_name()
        );
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    } else {
        log::error("cannot write {}", stamp.string());
        return 1;
    }

    if (mods.empty()) {
        log::info("all {} levels can be drawn: {}", ids.size(), stamp.string());
    } else {
        log::info(
            "all {} levels can be drawn with mod overrides from {}", ids.size(), mods.string()
        );
    }
    return 0;
}
#endif

}  // namespace openrac::program
