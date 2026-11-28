// `qilin identify <folder> --json [--build <title>]`: what the player's game
// data is, for the downloads' launcher (tools/player-kit/launcher), as the
// contract every Qilin project's recompiler keeps (ADOPTING.md there). One
// JSON object on standard output:
//
//   {"ok": true|false, "title": "...", "id": "...", "version": "...", "message": "..."}
//
// and the exit code: 0 when it is the supported build, 3 when it can be read
// but isn't (another game, another release: the message names what it is
// and what's needed), 2 when it can't be read at all (missing, not an
// extracted disc, damaged, incomplete). The message is a plain sentence for
// the player, shown as it is.
//
// Here the data is an extracted Xbox disc: a folder holding default.xbe and
// the game's own folders. The XBE's certificate names the game (its title
// ID) and its header's timestamp the build; the folders xwavebank, bundles
// and demand must be there too. Nothing is written, and only default.xbe is
// read.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace qilin {

struct Identity {
    int code = 2;  // 0, 3 or 2, as above
    bool ok = false;
    std::string title, id, version, message;
};

// The build a download supports.
struct SupportedBuild {
    std::string_view title;    // "Grabbed by the Ghoulies (USA)", game.txt's `build`
    std::string_view game;     // "Grabbed by the Ghoulies": the game's name, for another release's message
    std::string_view product;  // "MS-083", as the certificate's title ID reads
    uint32_t title_id = 0;     // the certificate's
    uint32_t timestamp = 0;    // the XBE header's (seconds since 1970, UTC)
};

// The one this port runs: the USA release (docs/xbe-analysis.md).
inline constexpr SupportedBuild kGhoulies{"Grabbed by the Ghoulies (USA)", "Grabbed by the Ghoulies", "MS-083", 0x4D530053u, 1064426049u};

// `input` is the extracted disc's folder (or its default.xbe); `want` a
// build's title (empty: the supported one).
Identity identify(const std::filesystem::path& input, std::string_view want = {}, const SupportedBuild& build = kGhoulies);

// The same for an XBE's bytes, with whether the disc's folders were found
// (identify() looks for them): the tests' entry point.
Identity identify_xbe(std::span<const uint8_t> xbe, std::string_view missing_folder, const SupportedBuild& build = kGhoulies);

std::string to_json(const Identity& identity);

// "24 September 2003", the UTC date of an XBE timestamp.
std::string xbe_date(uint32_t timestamp);

}  // namespace qilin
