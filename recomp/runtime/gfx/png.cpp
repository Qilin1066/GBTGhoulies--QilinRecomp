#include "path.hpp"
// A small PNG writer: 8-bit RGBA, per-row adaptive filters, and its own
// deflate (LZ77 with hash chains, then dynamic or fixed Huffman codes, or a
// stored block when neither saves anything), for saving frames in tests and
// from qilin_run. No dependencies.
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <queue>
#include <utility>

#include "gfx.hpp"

namespace qilin::gfx {
namespace {

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

uint32_t adler32(const std::vector<uint8_t>& data) {
    uint32_t a = 1, b = 0;
    for (size_t at = 0; at < data.size();) {
        const size_t end = std::min(data.size(), at + 5552);  // the most bytes before b can overflow
        for (; at < end; ++at) {
            a += data[at];
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

void be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, uint32_t(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, crc32(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
}

// ---------------------------------------------------------------- filters
uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

// One row through filter type `Type` (0 none, 1 sub, 2 up, 3 average, 4 Paeth);
// returns the output's size read as signed bytes.
template <int Type>
uint64_t filter_row(const uint8_t* cur, const uint8_t* up, size_t row, uint8_t* out) {
    uint64_t cost = 0;
    for (size_t i = 0; i < row; ++i) {
        const int a = i >= 4 ? cur[i - 4] : 0, b = up[i], c = i >= 4 ? up[i - 4] : 0;
        uint8_t pred = 0;
        if constexpr (Type == 1) pred = uint8_t(a);
        if constexpr (Type == 2) pred = uint8_t(b);
        if constexpr (Type == 3) pred = uint8_t((a + b) / 2);
        if constexpr (Type == 4) pred = paeth(a, b, c);
        out[i] = uint8_t(cur[i] - pred);
        cost += uint64_t(std::abs(int(int8_t(out[i]))));
    }
    return cost;
}

// Each row gets the filter whose output, read as signed bytes, sums smallest
// (the PNG specification's suggested heuristic).
std::vector<uint8_t> filter_rows(const Image& image) {
    using Filter = uint64_t (*)(const uint8_t*, const uint8_t*, size_t, uint8_t*);
    static constexpr Filter kFilters[5] = {filter_row<0>, filter_row<1>, filter_row<2>, filter_row<3>, filter_row<4>};
    const size_t row = size_t(image.width) * 4;
    std::vector<uint8_t> raw((row + 1) * image.height);
    std::vector<uint8_t> zero(row, 0), trial(row);
    for (uint32_t y = 0; y < image.height; ++y) {
        const uint8_t* cur = image.rgba.data() + y * row;
        const uint8_t* up = y ? cur - row : zero.data();
        uint8_t* best = raw.data() + y * (row + 1);
        uint64_t best_cost = ~uint64_t(0);
        for (uint8_t type = 0; type < 5; ++type) {
            const uint64_t cost = kFilters[type](cur, up, row, trial.data());
            if (cost < best_cost) {
                best_cost = cost;
                best[0] = type;
                std::copy(trial.begin(), trial.end(), best + 1);
            }
        }
    }
    return raw;
}

// ---------------------------------------------------------------- deflate
class Bits {
public:
    explicit Bits(std::vector<uint8_t>* out) : out_(out) {}
    void put(uint32_t value, int count) {  // least significant bit first
        count_ += uint64_t(count);
        if (!out_) return;
        acc_ |= uint64_t(value) << fill_;
        fill_ += count;
        for (; fill_ >= 8; fill_ -= 8, acc_ >>= 8) out_->push_back(uint8_t(acc_));
    }
    void align() {
        if (fill_ && out_) out_->push_back(uint8_t(acc_));
        count_ = (count_ + 7) & ~uint64_t(7);
        acc_ = 0;
        fill_ = 0;
    }
    uint64_t count() const { return count_; }

private:
    std::vector<uint8_t>* out_;  // null: only count
    uint64_t acc_ = 0, count_ = 0;
    int fill_ = 0;
};

constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

template <size_t N>
int code_of(const uint16_t (&base)[N], uint32_t v) {
    return int(std::upper_bound(base, base + N, v) - base) - 1;
}

struct Token {
    uint16_t len;  // a literal byte when dist is 0, else the match length
    uint16_t dist;
};

// Huffman code lengths no longer than `limit` for these frequencies; unused
// symbols get 0. At least two symbols always get codes, so every code is complete.
std::vector<uint8_t> code_lengths(std::vector<uint32_t> freq, int limit) {
    size_t used = 0;
    for (uint32_t f : freq) used += f != 0;
    for (size_t s = 0; used < 2; ++s) {
        if (!freq[s]) {
            freq[s] = 1;
            ++used;
        }
    }
    struct Node {
        uint64_t weight;
        int left, right;  // children; for a leaf, -1 and the symbol
    };
    using Entry = std::pair<uint64_t, int>;
    for (int shift = 0;; ++shift) {  // too deep: flatten the weights and build again
        std::vector<Node> nodes;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;
        for (size_t s = 0; s < freq.size(); ++s) {
            if (!freq[s]) continue;
            nodes.push_back({std::max<uint64_t>(1, freq[s] >> shift), -1, int(s)});
            heap.push({nodes.back().weight, int(nodes.size() - 1)});
        }
        while (heap.size() > 1) {
            const Entry a = heap.top();
            heap.pop();
            const Entry b = heap.top();
            heap.pop();
            nodes.push_back({a.first + b.first, a.second, b.second});
            heap.push({nodes.back().weight, int(nodes.size() - 1)});
        }
        std::vector<uint8_t> lengths(freq.size(), 0);
        bool fits = true;
        std::vector<std::pair<int, int>> stack{{heap.top().second, 0}};
        while (!stack.empty()) {
            const auto [at, depth] = stack.back();
            stack.pop_back();
            const Node& n = nodes[size_t(at)];
            if (n.left < 0) {
                fits = fits && depth <= limit;
                lengths[size_t(n.right)] = uint8_t(std::min(depth, 255));
            } else {
                stack.push_back({n.left, depth + 1});
                stack.push_back({n.right, depth + 1});
            }
        }
        if (fits) return lengths;
    }
}

// Canonical codes for these lengths, bit-reversed for writing least significant bit first.
std::vector<uint16_t> canonical(const std::vector<uint8_t>& lengths) {
    uint32_t count[16] = {}, next[16] = {};
    for (uint8_t l : lengths) ++count[l];
    count[0] = 0;
    for (uint32_t bits = 1, code = 0; bits < 16; ++bits) {
        code = (code + count[bits - 1]) << 1;
        next[bits] = code;
    }
    std::vector<uint16_t> codes(lengths.size(), 0);
    for (size_t s = 0; s < lengths.size(); ++s) {
        const int l = lengths[s];
        if (!l) continue;
        const uint32_t c = next[l]++;
        uint32_t r = 0;
        for (int b = 0; b < l; ++b) r |= ((c >> b) & 1u) << (l - 1 - b);
        codes[s] = uint16_t(r);
    }
    return codes;
}

struct Tree {
    std::vector<uint8_t> lengths;
    std::vector<uint16_t> codes;
    explicit Tree(std::vector<uint8_t> l) : lengths(std::move(l)), codes(canonical(lengths)) {}
    void put(Bits& bits, size_t symbol) const { bits.put(codes[symbol], lengths[symbol]); }
};

void put_tokens(Bits& bits, const std::vector<Token>& tokens, const Tree& lit, const Tree& dist) {
    for (const Token& t : tokens) {
        if (!t.dist) {
            lit.put(bits, t.len);
            continue;
        }
        const int lc = code_of(kLenBase, t.len), dc = code_of(kDistBase, t.dist);
        lit.put(bits, size_t(257 + lc));
        bits.put(uint32_t(t.len - kLenBase[lc]), kLenExtra[lc]);
        dist.put(bits, size_t(dc));
        bits.put(uint32_t(t.dist - kDistBase[dc]), kDistExtra[dc]);
    }
    lit.put(bits, 256);
}

// Code lengths as code-length symbols, runs folded into 16 (repeat the last
// length 3-6 times), 17 (3-10 zeros) and 18 (11-138 zeros), each with its extra bits.
std::vector<std::pair<uint8_t, uint8_t>> fold_lengths(const std::vector<uint8_t>& seq) {
    std::vector<std::pair<uint8_t, uint8_t>> out;
    for (size_t i = 0; i < seq.size();) {
        const uint8_t v = seq[i];
        size_t run = 1;
        while (i + run < seq.size() && seq[i + run] == v) ++run;
        i += run;
        if (!v) {
            while (run >= 11) {
                const size_t n = std::min<size_t>(run, 138);
                out.push_back({uint8_t(18), uint8_t(n - 11)});
                run -= n;
            }
            if (run >= 3) {
                out.push_back({uint8_t(17), uint8_t(run - 3)});
                run = 0;
            }
        } else {
            out.push_back({v, uint8_t(0)});
            --run;
            while (run >= 3) {
                const size_t n = std::min<size_t>(run, 6);
                out.push_back({uint8_t(16), uint8_t(n - 3)});
                run -= n;
            }
        }
        for (; run; --run) out.push_back({v, uint8_t(0)});
    }
    return out;
}

// A dynamic block's header: its code-length code, then the literal/length and distance codes.
void put_dynamic_header(Bits& bits, bool last, const Tree& lit, const Tree& dist) {
    size_t hlit = lit.lengths.size(), hdist = dist.lengths.size();
    while (hlit > 257 && !lit.lengths[hlit - 1]) --hlit;
    while (hdist > 1 && !dist.lengths[hdist - 1]) --hdist;
    std::vector<uint8_t> seq(lit.lengths.begin(), lit.lengths.begin() + std::ptrdiff_t(hlit));
    seq.insert(seq.end(), dist.lengths.begin(), dist.lengths.begin() + std::ptrdiff_t(hdist));
    const auto folded = fold_lengths(seq);
    std::vector<uint32_t> cl_freq(19, 0);
    for (const auto& f : folded) ++cl_freq[f.first];
    const Tree cl(code_lengths(cl_freq, 7));
    size_t hclen = 19;
    while (hclen > 4 && !cl.lengths[kClOrder[hclen - 1]]) --hclen;

    bits.put(last ? 1 : 0, 1);
    bits.put(2, 2);
    bits.put(uint32_t(hlit - 257), 5);
    bits.put(uint32_t(hdist - 1), 5);
    bits.put(uint32_t(hclen - 4), 4);
    for (size_t i = 0; i < hclen; ++i) bits.put(cl.lengths[kClOrder[i]], 3);
    for (const auto& [symbol, extra] : folded) {
        cl.put(bits, symbol);
        if (symbol >= 16) bits.put(extra, symbol == 16 ? 2 : symbol == 17 ? 3 : 7);
    }
}

// How many bits the tokens take under these codes, from how often each symbol occurs.
uint64_t token_bits(const std::vector<uint32_t>& lit_freq, const std::vector<uint32_t>& dist_freq, const Tree& lit, const Tree& dist) {
    uint64_t n = 0;
    for (size_t s = 0; s < lit_freq.size(); ++s) n += uint64_t(lit_freq[s]) * (lit.lengths[s] + (s > 256 ? kLenExtra[s - 257] : 0u));
    for (size_t s = 0; s < dist_freq.size(); ++s) n += uint64_t(dist_freq[s]) * (dist.lengths[s] + kDistExtra[s]);
    return n;
}

const Tree& fixed_lit() {
    static const Tree tree([] {
        std::vector<uint8_t> l(288, 8);
        std::fill(l.begin() + 144, l.begin() + 256, uint8_t(9));
        std::fill(l.begin() + 256, l.begin() + 280, uint8_t(7));
        return l;
    }());
    return tree;
}

const Tree& fixed_dist() {
    static const Tree tree(std::vector<uint8_t>(30, 5));
    return tree;
}

void put_stored(Bits& bits, bool last, const uint8_t* data, size_t size) {
    do {
        const size_t n = std::min<size_t>(size, 65535);
        bits.put(last && n == size ? 1 : 0, 1);
        bits.put(0, 2);
        bits.align();
        bits.put(uint32_t(n), 16);
        bits.put(uint32_t(~n & 0xFFFF), 16);
        for (size_t i = 0; i < n; ++i) bits.put(data[i], 8);
        data += n;
        size -= n;
    } while (size);
}

// One block's tokens, written whichever way comes out shortest.
void put_block(Bits& bits, bool last, const std::vector<Token>& tokens, const uint8_t* data, size_t size) {
    std::vector<uint32_t> lit_freq(286, 0), dist_freq(30, 0);
    for (const Token& t : tokens) {
        if (!t.dist) {
            ++lit_freq[t.len];
        } else {
            ++lit_freq[size_t(257 + code_of(kLenBase, t.len))];
            ++dist_freq[size_t(code_of(kDistBase, t.dist))];
        }
    }
    ++lit_freq[256];
    const Tree lit(code_lengths(lit_freq, 15)), dist(code_lengths(dist_freq, 15));
    Bits header(nullptr);
    put_dynamic_header(header, last, lit, dist);
    const uint64_t dynamic = header.count() + token_bits(lit_freq, dist_freq, lit, dist);
    const uint64_t fixed = 3 + token_bits(lit_freq, dist_freq, fixed_lit(), fixed_dist());
    const uint64_t stored = 8 * (size + 5 * ((size + 65534) / 65535)) + 7;  // headers and padding at most
    if (stored < dynamic && stored < fixed) {
        put_stored(bits, last, data, size);
    } else if (dynamic <= fixed) {
        put_dynamic_header(bits, last, lit, dist);
        put_tokens(bits, tokens, lit, dist);
    } else {
        bits.put(last ? 1 : 0, 1);
        bits.put(1, 2);
        put_tokens(bits, tokens, fixed_lit(), fixed_dist());
    }
}

// A zlib stream (RFC 1950/1951) of `data`: greedy LZ77 over a 32 KiB window,
// trying the 32 most recent places each 3-byte prefix was seen.
std::vector<uint8_t> zlib(const std::vector<uint8_t>& data) {
    constexpr size_t kWindow = 32768, kChain = 32, kBlockTokens = size_t(1) << 16;
    constexpr int kHashBits = 15;
    constexpr uint32_t kNone = ~0u;
    std::vector<uint8_t> out{0x78, 0x9C};
    out.reserve(data.size() / 2);
    Bits bits(&out);
    std::vector<uint32_t> head(size_t(1) << kHashBits, kNone), prev(kWindow, kNone);
    const auto hash_at = [&](size_t i) {
        const uint32_t v = uint32_t(data[i]) | uint32_t(data[i + 1]) << 8 | uint32_t(data[i + 2]) << 16;
        return (v * 2654435761u) >> (32 - kHashBits);
    };
    const auto load8 = [&](size_t i) {
        uint64_t v;
        std::memcpy(&v, data.data() + i, 8);
        return v;
    };
    const auto insert = [&](size_t i) {
        if (i + 3 > data.size()) return;
        const uint32_t h = hash_at(i);
        prev[i % kWindow] = head[h];
        head[h] = uint32_t(i);
    };
    std::vector<Token> tokens;
    tokens.reserve(kBlockTokens);
    size_t block_start = 0;
    for (size_t i = 0; i < data.size();) {
        size_t best_len = 0, best_dist = 0;
        if (i + 3 <= data.size()) {
            const size_t max_len = std::min<size_t>(258, data.size() - i);
            uint32_t cand = head[hash_at(i)];
            for (size_t steps = 0; cand != kNone && i - cand <= kWindow && steps < kChain; ++steps) {
                if (!best_len || data[cand + best_len] == data[i + best_len]) {  // can it beat the best so far?
                    size_t len = 0;
                    while (len + 8 <= max_len && load8(cand + len) == load8(i + len)) len += 8;
                    while (len < max_len && data[cand + len] == data[i + len]) ++len;
                    if (len > best_len) {
                        best_len = len;
                        best_dist = i - cand;
                        if (len == max_len) break;
                    }
                }
                const uint32_t next = prev[cand % kWindow];
                if (next == kNone || next >= cand) break;  // the end, or a slot reused by a newer position
                cand = next;
            }
        }
        if (best_len >= 3) {
            tokens.push_back({uint16_t(best_len), uint16_t(best_dist)});
            // Inside a long match (a flat stretch, mostly) only its ends are worth finding again.
            for (size_t k = 0; k < best_len; ++k)
                if (best_len <= 32 || k < 4 || k + 4 >= best_len) insert(i + k);
            i += best_len;
        } else {
            tokens.push_back({uint16_t(data[i]), 0});
            insert(i);
            ++i;
        }
        if (tokens.size() >= kBlockTokens || i == data.size()) {
            put_block(bits, i == data.size(), tokens, data.data() + block_start, i - block_start);
            tokens.clear();
            block_start = i;
        }
    }
    if (data.empty()) put_stored(bits, true, data.data(), 0);
    bits.align();
    be32(out, adler32(data));
    return out;
}

}  // namespace

bool write_png(const std::string& path, const Image& image) {
    if (image.width == 0 || image.height == 0 || image.rgba.size() != size_t(image.width) * image.height * 4) return false;
    std::vector<uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    be32(ihdr, image.width);
    be32(ihdr, image.height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8 bits, RGBA, deflate, adaptive filters, no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", zlib(filter_rows(image)));
    chunk(out, "IEND", {});
#ifdef _WIN32
    std::FILE* f = _wfopen(qilin::native_path(path).c_str(), L"wb");
#else
    std::FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace qilin::gfx
