// `qilin identify` (identify.hpp).
#include "identify.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <vector>

#include "path.hpp"
#include "xbe.hpp"

namespace qilin {

namespace fs = std::filesystem;

namespace {

// The folders an extracted disc of the game holds beside default.xbe (the
// runtime reads its sound, levels and streamed data from them).
constexpr std::array<std::string_view, 3> kDiscFolders = {"xwavebank", "bundles", "demand"};

constexpr std::string_view kExtract =
    "Extract the whole disc image (with extract-xiso or xdvdfs, for example) and choose the folder that holds default.xbe.";

Identity answer(int code, std::string message) {
    Identity i;
    i.code = code;
    i.ok = code == 0;
    i.message = std::move(message);
    return i;
}

// "MS-083": the title ID as the box and the certificate show it.
std::string product_id(uint32_t title_id) {
    const char a = char(title_id >> 24), b = char((title_id >> 16) & 0xFF);
    if (std::isupper(static_cast<unsigned char>(a)) && std::isupper(static_cast<unsigned char>(b)))
        return std::format("{}{}-{:03}", a, b, title_id & 0xFFFF);
    return std::format("{:08X}", title_id);
}

// The certificate's game regions, in words: "USA", "Japan", "Europe".
std::string region_text(uint32_t region) {
    std::string out;
    auto add = [&](std::string_view s) {
        if (!out.empty()) out += ", ";
        out += s;
    };
    if (region & 1) add("USA");
    if (region & 2) add("Japan");
    if (region & 4) add("Europe");
    return out;
}

std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20) {
            out += std::format("\\u{:04x}", unsigned(u));
        } else {
            out += c;  // UTF-8 as it is
        }
    }
    return out + "\"";
}

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// The entry of `dir` named `name`, whatever its case (an extraction may
// keep the disc's capitals, and Linux and macOS may tell them apart).
std::optional<fs::directory_entry> find_entry(const fs::path& dir, std::string_view name) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (lower(utf8_path(it->path().filename())) == name) return *it;
    return std::nullopt;
}

std::string describe(const SupportedBuild& b) { return std::format("{}, {} (built {})", b.title, b.product, xbe_date(b.timestamp)); }

}  // namespace

std::string xbe_date(uint32_t timestamp) {
    // Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm).
    const int64_t z = int64_t(timestamp / 86400) + 719468;
    const int64_t era = z / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    static constexpr std::array<std::string_view, 12> kMonths = {"January", "February", "March",     "April",   "May",      "June",
                                                                 "July",    "August",   "September", "October", "November", "December"};
    return std::format("{} {} {}", day, kMonths[size_t(month - 1)], year);
}

Identity identify_xbe(std::span<const uint8_t> bytes, std::string_view missing_folder, const SupportedBuild& build) {
    const std::string wanted = " This download is for " + describe(build) + ".";
    auto xbe = Xbe::parse(bytes);
    if (!xbe)
        return answer(2, "Its default.xbe isn't a readable Xbox program: the disc may not have been extracted completely. " + std::string(kExtract));
    const XbeCertificate& cert = xbe->cert;
    Identity i = answer(0, "");
    i.id = product_id(cert.title_id);
    i.version = std::to_string(cert.version);
    const std::string region = region_text(cert.region);
    const std::string name = cert.title_name.empty() ? "an Xbox game" : cert.title_name;
    i.title = cert.title_name.empty() ? std::string() : region.empty() ? cert.title_name : cert.title_name + " (" + region + ")";
    if (cert.title_id != build.title_id) {
        i.code = 3;
        i.ok = false;
        i.message = std::format("This is {} ({}), not {}.{}", name, region.empty() ? i.id : region + ", " + i.id, build.game, wanted);
        return i;
    }
    if (xbe->timestamp != build.timestamp) {
        i.code = 3;
        i.ok = false;
        i.message = std::format("This is {}{}, but another release of it: its default.xbe was built {}, version {}.{}", build.game,
                                region.empty() ? std::string() : " (" + region + ")", xbe_date(xbe->timestamp), i.version, wanted);
        return i;
    }
    i.title = std::string(build.title);
    if (!missing_folder.empty()) {
        i.code = 2;
        i.ok = false;
        i.message = std::format("This default.xbe is the right one, {}, but the folder has no {} folder beside it: extract the whole disc, not "
                                "just default.xbe, and choose the folder that holds both.",
                                build.title, missing_folder);
        return i;
    }
    i.message = std::format("This is {}, {} (built {}): the right disc.", build.title, i.id, xbe_date(xbe->timestamp));
    return i;
}

Identity identify(const fs::path& input, std::string_view want, const SupportedBuild& build) {
    if (!want.empty() && want != build.title) return answer(2, "This recompiler knows no build called " + std::string(want) + ".");
    std::error_code ec;
    const fs::file_status status = fs::status(input, ec);
    if (ec || !fs::exists(status)) return answer(2, "Can't find " + utf8_path(input) + ".");

    fs::path folder, xbe_path;
    if (fs::is_directory(status)) {
        folder = input;
        // By this name, as the launcher's steps and the game's start read it
        // (the disc's own folders may have any case: the runtime finds them).
        xbe_path = folder / "default.xbe";
        if (!fs::is_regular_file(xbe_path, ec)) return answer(2, "There's no default.xbe in this folder. " + std::string(kExtract));
    } else {
        // A file: default.xbe itself (its folder is the disc's), else something
        // that isn't an extracted disc, such as the disc image.
        if (lower(utf8_path(input.filename())) != "default.xbe")
            return answer(2, "This is a file, not a folder. If it's the disc image (an .iso), extract it first (with extract-xiso or xdvdfs, for "
                             "example), then choose the folder that holds default.xbe.");
        xbe_path = input;
        folder = input.parent_path();
    }

    std::vector<uint8_t> bytes;
    {
        std::ifstream f(xbe_path, std::ios::binary);
        if (!f) return answer(2, "Can't read this folder's default.xbe: it may be in use, or not yours to read.");
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (f.bad()) return answer(2, "Can't read this folder's default.xbe to the end.");
    }
    std::string_view missing;
    for (const std::string_view needed : kDiscFolders) {
        const auto entry = find_entry(folder, needed);
        if (!entry || !entry->is_directory(ec)) {
            missing = needed;
            break;
        }
    }
    return identify_xbe(bytes, missing, build);
}

std::string to_json(const Identity& i) {
    return std::string("{\"ok\": ") + (i.ok ? "true" : "false") + ", \"title\": " + json_string(i.title) + ", \"id\": " + json_string(i.id) +
           ", \"version\": " + json_string(i.version) + ", \"message\": " + json_string(i.message) + "}\n";
}

}  // namespace qilin
