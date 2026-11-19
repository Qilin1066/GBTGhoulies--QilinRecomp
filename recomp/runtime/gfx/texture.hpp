// NV2A textures as the renderer takes them: D3D's texture header (Format
// and Size words) and the texels in guest memory, decoded to RGBA8 with
// every mip level and cube face. docs/graphics.md, "Textures".
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qilin::gfx {

struct TextureData {
    uint32_t width = 0, height = 0, depth = 1;
    uint32_t levels = 1, faces = 1;  // faces: 6 for a cube map
    bool linear = false;             // a linear (LIN_) texture: the hardware addresses it in texels
    // RGBA8, face by face, each face its levels from the largest, each level rows top to bottom.
    std::vector<uint8_t> rgba;
    size_t level_offset(uint32_t face, uint32_t level) const;
    uint32_t level_width(uint32_t l) const { return width >> l ? width >> l : 1; }
    uint32_t level_height(uint32_t l) const { return height >> l ? height >> l : 1; }
    uint32_t level_depth(uint32_t l) const { return depth >> l ? depth >> l : 1; }
    uint32_t dims() const { return faces == 6 ? 6 : depth > 1 ? 3 : 2; }
};

// What a texture's Format and Size words describe, without the texels.
struct TextureShape {
    uint32_t color_format = 0;  // the X_D3DFMT code (Format bits 8-15)
    uint32_t width = 0, height = 0, depth = 1, levels = 1, pitch = 0;
    bool cube = false, linear = false, compressed = false;
    uint32_t bits_per_texel = 0;  // 4 for DXT1, 8 for DXT3/5
    bool palettized = false;      // P8: indices into the stage's palette
    size_t bytes = 0;             // all levels and faces, as stored in guest memory
};

// Reads the header words; false (with the reason) for a format not supported yet.
bool texture_shape(uint32_t format, uint32_t size, TextureShape* out, std::string* why);

// Decodes the texels at `data` (bytes from that address on) to RGBA8; a
// palettized texture (palettized) through `palette`, 256 A8R8G8B8 entries.
TextureData decode_texture(const TextureShape& shape, const uint8_t* data, const uint8_t* palette = nullptr);

// The Morton (swizzled) offset of texel (x, y, z) in a power-of-two texture.
uint32_t swizzle_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth);

}  // namespace qilin::gfx
