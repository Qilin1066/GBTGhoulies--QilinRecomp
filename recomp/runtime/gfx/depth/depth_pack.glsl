#version 450
// A depth target's contents as the console's memory holds a D24S8 surface,
// for a draw that reads that memory as an A8R8G8B8 texture: each 32-bit word
// is the depth (24 bits, the depth range 0 to 1 as 0 to 0xFFFFFF) above the
// stencil (8 bits), so alpha, red and green are the depth's bytes from the
// top and blue the stencil. Written into an image of the renderer's colour
// format, the same size: a full-screen triangle, one texel per pixel.
#ifdef VERTEX
void main() {
    const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
#else
layout(set = 0, binding = 0) uniform sampler2D depth_aspect;
layout(set = 0, binding = 1) uniform usampler2D stencil_aspect;
layout(location = 0) out vec4 packed_word;
void main() {
    const ivec2 at = ivec2(gl_FragCoord.xy);
    const uint z = uint(round(clamp(texelFetch(depth_aspect, at, 0).r, 0.0, 1.0) * 16777215.0));
    const uint s = texelFetch(stencil_aspect, at, 0).r & 255u;
    packed_word = vec4(float((z >> 8) & 255u), float(z & 255u), float(s), float(z >> 16)) / 255.0;
}
#endif
