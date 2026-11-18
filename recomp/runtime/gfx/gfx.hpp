// The renderer: Vulkan 1.3, and only Vulkan (docs/graphics.md).
//
// The D3D replacements (recomp/runtime/host/d3d) draw through this interface
// and nothing else in the runtime touches Vulkan. Frames are rendered into
// the renderer's own images, the D3D back buffers, so a frame can be read
// back, saved or scaled whether or not there's a window; presenting to a
// window copies the finished frame to it.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qilin::gfx {

struct Config {
    uint32_t width = 640, height = 480;  // the frame buffers' size in pixels
    uint32_t back_buffers = 1;           // D3DPRESENT_PARAMETERS.BackBufferCount; with the front buffer, one more image
    bool depth_stencil = true;           // a 24-bit depth, 8-bit stencil buffer
    int gpu = -1;                        // index among the Vulkan devices; -1 picks the best one
    bool validation = false;             // Vulkan validation layers, if installed (also QILIN_VK_VALIDATION=1)
};

struct Rect {
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
};

enum ClearFlags : uint32_t { kClearColor = 1, kClearDepth = 2, kClearStencil = 4 };

struct Image {  // 8-bit RGBA, rows top to bottom
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

class Renderer {
public:
    // Creates the renderer on a Vulkan 1.3 device, or returns null with the
    // reason in *error: no Vulkan library, no device, or a missing feature.
    static std::unique_ptr<Renderer> create(const Config& config, std::string* error);
    virtual ~Renderer() = default;

    virtual const std::string& device_name() const = 0;  // "NVIDIA GeForce …", "llvmpipe (LLVM …)"
    virtual const Config& config() const = 0;

    // Clears the back buffer and/or the depth and stencil buffers: all of
    // them (count 0) or the given rectangles. rgba is 0..1 per channel.
    virtual void clear(const Rect* rects, uint32_t count, uint32_t flags, const float rgba[4], float depth,
                       uint32_t stencil) = 0;

    // Ends the frame: the back buffer becomes the front buffer (a flip, as
    // D3D's Swap), and the oldest buffer becomes the new back buffer.
    virtual void present() = 0;
    virtual uint64_t frames_presented() const = 0;
    // The frame buffers are numbered 0 … back_buffers; these say which is which now.
    virtual uint32_t back_index() const = 0;
    virtual uint32_t front_index() const = 0;
    // Submits what's recorded and waits for the GPU to finish it.
    virtual void flush() = 0;

    // Reads a frame buffer back: the front buffer (the last frame presented)
    // or the back buffer as drawn so far.
    virtual Image read_front_buffer() = 0;
    virtual Image read_back_buffer() = 0;

    // Validation messages of error severity so far (0 without the layers).
    virtual uint32_t validation_errors() const = 0;
};

// Writes an image as a PNG (uncompressed). False if the file can't be written.
bool write_png(const std::string& path, const Image& image);

}  // namespace qilin::gfx
