#include "texture.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#include "strf.hpp"

namespace qilin::gfx {

namespace {

enum class Kind : uint8_t { Unsupported, Argb8, Xrgb8, Rgb565, Argb1555, Xrgb1555, Argb4444, L8, Al8, A8, A8l8, Abgr8, Bgra8, Rgba8, Rgba5551, Rgba4444, L16, Dxt1, Dxt3, Dxt5 };

struct FormatInfo {
    Kind kind;
    uint8_t bits;
    bool linear;
};

// The X_D3DFMT codes (XDK 5659), swizzled and linear.
FormatInfo format_info(uint32_t code) {
    switch (code) {
        case 0x00: return {Kind::L8, 8, false};
        case 0x01: return {Kind::Al8, 8, false};
        case 0x02: return {Kind::Argb1555, 16, false};
        case 0x03: return {Kind::Xrgb1555, 16, false};
        case 0x04: return {Kind::Argb4444, 16, false};
        case 0x05: return {Kind::Rgb565, 16, false};
        case 0x06: return {Kind::Argb8, 32, false};
        case 0x07: return {Kind::Xrgb8, 32, false};
        case 0x0C: return {Kind::Dxt1, 4, false};
        case 0x0E: return {Kind::Dxt3, 8, false};
        case 0x0F: return {Kind::Dxt5, 8, false};
        case 0x10: return {Kind::Argb1555, 16, true};
        case 0x11: return {Kind::Rgb565, 16, true};
        case 0x12: return {Kind::Argb8, 32, true};
        case 0x13: return {Kind::L8, 8, true};
        case 0x19: return {Kind::A8, 8, false};
        case 0x1A: return {Kind::A8l8, 16, false};
        case 0x1B: return {Kind::Al8, 8, true};
        case 0x1C: return {Kind::Xrgb1555, 16, true};
        case 0x1D: return {Kind::Argb4444, 16, true};
        case 0x1E: return {Kind::Xrgb8, 32, true};
        case 0x1F: return {Kind::A8, 8, true};
        case 0x20: return {Kind::A8l8, 16, true};
        case 0x32: return {Kind::L16, 16, false};
        case 0x35: return {Kind::L16, 16, true};
        case 0x38: return {Kind::Rgba5551, 16, false};
        case 0x39: return {Kind::Rgba4444, 16, false};
        case 0x3A: return {Kind::Abgr8, 32, false};
        case 0x3B: return {Kind::Bgra8, 32, false};
        case 0x3C: return {Kind::Rgba8, 32, false};
        case 0x3D: return {Kind::Rgba5551, 16, true};
        case 0x3E: return {Kind::Rgba4444, 16, true};
        case 0x3F: return {Kind::Abgr8, 32, true};
        case 0x40: return {Kind::Bgra8, 32, true};
        case 0x41: return {Kind::Rgba8, 32, true};
        default: return {Kind::Unsupported, 0, false};
    }
}

uint8_t expand(uint32_t v, uint32_t bits) { return uint8_t(bits == 1 ? (v ? 255 : 0) : (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); }

void texel(Kind k, const uint8_t* p, uint8_t* o) {
    const uint32_t h = uint32_t(p[0]) | uint32_t(p[1]) << 8;
    switch (k) {
        case Kind::Argb8: o[0] = p[2], o[1] = p[1], o[2] = p[0], o[3] = p[3]; break;
        case Kind::Xrgb8: o[0] = p[2], o[1] = p[1], o[2] = p[0], o[3] = 255; break;
        case Kind::Abgr8: o[0] = p[0], o[1] = p[1], o[2] = p[2], o[3] = p[3]; break;
        case Kind::Bgra8: o[0] = p[1], o[1] = p[2], o[2] = p[3], o[3] = p[0]; break;
        case Kind::Rgba8: o[0] = p[3], o[1] = p[2], o[2] = p[1], o[3] = p[0]; break;
        case Kind::Rgb565: o[0] = expand(h >> 11, 5), o[1] = expand(h >> 5 & 63, 6), o[2] = expand(h & 31, 5), o[3] = 255; break;
        case Kind::Argb1555: o[0] = expand(h >> 10 & 31, 5), o[1] = expand(h >> 5 & 31, 5), o[2] = expand(h & 31, 5), o[3] = h >> 15 ? 255 : 0; break;
        case Kind::Xrgb1555: o[0] = expand(h >> 10 & 31, 5), o[1] = expand(h >> 5 & 31, 5), o[2] = expand(h & 31, 5), o[3] = 255; break;
        case Kind::Argb4444: o[0] = expand(h >> 8 & 15, 4), o[1] = expand(h >> 4 & 15, 4), o[2] = expand(h & 15, 4), o[3] = expand(h >> 12, 4); break;
        case Kind::Rgba5551: o[0] = expand(h >> 11, 5), o[1] = expand(h >> 6 & 31, 5), o[2] = expand(h >> 1 & 31, 5), o[3] = h & 1 ? 255 : 0; break;
        case Kind::Rgba4444: o[0] = expand(h >> 12, 4), o[1] = expand(h >> 8 & 15, 4), o[2] = expand(h >> 4 & 15, 4), o[3] = expand(h & 15, 4); break;
        case Kind::L8: o[0] = o[1] = o[2] = p[0], o[3] = 255; break;
        case Kind::Al8: o[0] = o[1] = o[2] = o[3] = p[0]; break;
        case Kind::A8: o[0] = o[1] = o[2] = 0, o[3] = p[0]; break;
        case Kind::A8l8: o[0] = o[1] = o[2] = p[0], o[3] = p[1]; break;
        case Kind::L16: o[0] = o[1] = o[2] = p[1], o[3] = 255; break;
        default: o[0] = o[1] = o[2] = o[3] = 0; break;
    }
}

// One 4x4 block of DXT1 colours (and the 1-bit alpha DXT1 gives, or opaque).
void dxt_colors(const uint8_t* b, uint8_t out[16][4], bool dxt1) {
    const uint32_t c0 = uint32_t(b[0]) | uint32_t(b[1]) << 8, c1 = uint32_t(b[2]) | uint32_t(b[3]) << 8;
    uint8_t pal[4][4];
    auto rgb = [](uint32_t c, uint8_t* o) { o[0] = expand(c >> 11, 5), o[1] = expand(c >> 5 & 63, 6), o[2] = expand(c & 31, 5), o[3] = 255; };
    rgb(c0, pal[0]);
    rgb(c1, pal[1]);
    if (c0 > c1 || !dxt1) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2), pal[3][k] = 0;
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    const uint32_t bits = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
    for (int i = 0; i < 16; ++i) std::memcpy(out[i], pal[bits >> (2 * i) & 3], 4);
}

void dxt_block(Kind k, const uint8_t* b, uint8_t out[16][4]) {
    if (k == Kind::Dxt1) return dxt_colors(b, out, true);
    dxt_colors(b + 8, out, false);
    if (k == Kind::Dxt3) {
        for (int i = 0; i < 16; ++i) out[i][3] = expand(b[i / 2] >> (4 * (i & 1)) & 15, 4);
    } else {
        const uint32_t a0 = b[0], a1 = b[1];
        uint8_t a[8] = {uint8_t(a0), uint8_t(a1)};
        if (a0 > a1) {
            for (int i = 1; i < 7; ++i) a[i + 1] = uint8_t(((7 - i) * a0 + i * a1 + 3) / 7);
        } else {
            for (int i = 1; i < 5; ++i) a[i + 1] = uint8_t(((5 - i) * a0 + i * a1 + 2) / 5);
            a[6] = 0, a[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) out[i][3] = a[bits >> (3 * i) & 7];
    }
}


}  // namespace

uint32_t swizzle_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth) {
    uint32_t r = 0, bit = 0;
    for (uint32_t m = 1; m < width || m < height || m < depth; m <<= 1) {
        if (m < width) r |= (x & m ? 1u : 0u) << bit++;
        if (m < height) r |= (y & m ? 1u : 0u) << bit++;
        if (m < depth) r |= (z & m ? 1u : 0u) << bit++;
    }
    return r;
}

size_t TextureData::level_offset(uint32_t face, uint32_t level) const {
    size_t off = 0, face_bytes = 0;
    for (uint32_t l = 0; l < levels; ++l) face_bytes += size_t(level_width(l)) * level_height(l) * level_depth(l) * 4;
    off = face_bytes * face;
    for (uint32_t l = 0; l < level; ++l) off += size_t(level_width(l)) * level_height(l) * level_depth(l) * 4;
    return off;
}

bool texture_shape(uint32_t format, uint32_t size, TextureShape* out, std::string* why) {
    TextureShape s;
    s.color_format = format >> 8 & 0xFF;
    const FormatInfo info = format_info(s.color_format);
    if (info.kind == Kind::Unsupported) {
        *why = strf("texture format %#x", s.color_format);
        return false;
    }
    s.cube = format & 4;
    const uint32_t dims = format >> 4 & 0xF;
    s.linear = info.linear;
    s.compressed = info.kind == Kind::Dxt1 || info.kind == Kind::Dxt3 || info.kind == Kind::Dxt5;
    s.bits_per_texel = info.bits;
    if (s.linear) {
        s.width = (size & 0xFFF) + 1;
        s.height = (size >> 12 & 0xFFF) + 1;
        s.pitch = ((size >> 24 & 0xFF) + 1) * 64;
        s.levels = 1;
        s.bytes = size_t(s.pitch) * s.height;
    } else {
        s.width = 1u << (format >> 20 & 0xF);
        s.height = 1u << (format >> 24 & 0xF);
        s.depth = dims == 3 ? 1u << (format >> 28 & 0xF) : 1;
        s.levels = std::max(1u, format >> 16 & 0xF);
        size_t face = 0;
        for (uint32_t l = 0; l < s.levels; ++l) {
            const uint32_t w = std::max(1u, s.width >> l), h = std::max(1u, s.height >> l), d = std::max(1u, s.depth >> l);
            face += s.compressed ? size_t(std::max(1u, w / 4)) * std::max(1u, h / 4) * (info.bits * 2) * d
                                 : size_t(w) * h * d * info.bits / 8;
        }
        if (s.cube) face = (face + 127) & ~size_t(127);  // cube faces start on 128-byte boundaries
        s.bytes = face * (s.cube ? 6 : 1);
    }
    if (dims != 2 && dims != 3) {
        *why = strf("a texture with %u dimensions", dims);
        return false;
    }
    if (s.linear && s.compressed) {
        *why = "a linear compressed texture";
        return false;
    }
    *out = s;
    return true;
}

TextureData decode_texture(const TextureShape& s, const uint8_t* data) {
    const FormatInfo info = format_info(s.color_format);
    TextureData t;
    t.width = s.width, t.height = s.height, t.depth = s.depth, t.levels = s.levels, t.faces = s.cube ? 6 : 1, t.linear = s.linear;
    t.rgba.resize(t.level_offset(t.faces, 0));
    const uint32_t bpp = info.bits / 8;
    size_t face_bytes = 0;
    for (uint32_t l = 0; l < s.levels; ++l) {
        const uint32_t w = t.level_width(l), h = t.level_height(l), d = t.level_depth(l);
        face_bytes += s.compressed ? size_t(std::max(1u, w / 4)) * std::max(1u, h / 4) * (info.bits * 2) * d : size_t(w) * h * d * bpp;
    }
    if (s.cube) face_bytes = (face_bytes + 127) & ~size_t(127);
    for (uint32_t f = 0; f < t.faces; ++f) {
        const uint8_t* src = data + face_bytes * f;
        for (uint32_t l = 0; l < s.levels; ++l) {
            const uint32_t w = t.level_width(l), h = t.level_height(l), d = t.level_depth(l);
            uint8_t* dst = t.rgba.data() + t.level_offset(f, l);
            if (s.compressed) {
                const uint32_t bw = std::max(1u, w / 4), bh = std::max(1u, h / 4), block = info.bits * 2;
                for (uint32_t by = 0; by < bh; ++by)
                    for (uint32_t bx = 0; bx < bw; ++bx) {
                        uint8_t px[16][4];
                        dxt_block(info.kind, src + (size_t(by) * bw + bx) * block, px);
                        for (uint32_t i = 0; i < 16; ++i) {
                            const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                            if (x < w && y < h) std::memcpy(dst + (size_t(y) * w + x) * 4, px[i], 4);
                        }
                    }
                src += size_t(bw) * bh * block * d;
            } else if (s.linear) {
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x) texel(info.kind, src + size_t(y) * s.pitch + size_t(x) * bpp, dst + (size_t(y) * w + x) * 4);
            } else {
                for (uint32_t z = 0; z < d; ++z)
                    for (uint32_t y = 0; y < h; ++y)
                        for (uint32_t x = 0; x < w; ++x)
                            texel(info.kind, src + size_t(swizzle_offset(x, y, z, w, h, d)) * bpp, dst + ((size_t(z) * h + y) * w + x) * 4);
                src += size_t(w) * h * d * bpp;
            }
        }
    }
    return t;
}

}  // namespace qilin::gfx
