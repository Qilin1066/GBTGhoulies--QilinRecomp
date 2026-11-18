// The kernel's crypto exports the game reaches (through XAPI's saved-game
// signatures): SHA-1 (XcSHAInit, XcSHAUpdate, XcSHAFinal) and XcHMAC.
//
// The SHA context is the caller's memory (A_SHA_CTX, 116 bytes where XAPI
// allocates it); the runtime keeps its state there: the five hash words
// (+0), the message length in bytes (+20, 64-bit) and the partial block (+28).
//
// The keys XAPI signs with come from the kernel's XboxSignatureKey, which
// on a console is set up from its EEPROM. Here it's the kernel data area's
// zeros, the same on every machine, so saves sign and verify the same
// wherever the runtime runs (and a console's own saves don't verify).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "crypto.hpp"
#include "host.hpp"

namespace qilin::host {

// ---------------------------------------------------------------- SHA-1
namespace {

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1_block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
        else f = b ^ c ^ d, k = 0xCA62C1D6u;
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

}  // namespace

void Sha1::update(const uint8_t* data, size_t n) {
    size_t used = size_t(bytes % 64);
    bytes += n;
    while (n) {
        const size_t take = std::min(n, 64 - used);
        std::memcpy(buffer + used, data, take);
        used += take, data += take, n -= take;
        if (used == 64) sha1_block(h, buffer), used = 0;
    }
}

std::array<uint8_t, 20> Sha1::final() {
    const uint64_t bits = bytes * 8;
    const uint8_t pad = 0x80, zero = 0;
    update(&pad, 1);
    while (bytes % 64 != 56) update(&zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; ++i) length[i] = uint8_t(bits >> (56 - 8 * i));
    update(length, 8);
    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 20; ++i) out[size_t(i)] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
    return out;
}

std::array<uint8_t, 20> hmac_sha1(const uint8_t* key, size_t key_len, const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len) {
    uint8_t k[64] = {};
    if (key_len > 64) {
        Sha1 s;
        s.update(key, key_len);
        const auto d = s.final();
        std::memcpy(k, d.data(), d.size());
    } else {
        std::memcpy(k, key, key_len);
    }
    uint8_t pad[64];
    Sha1 inner;
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x36;
    inner.update(pad, 64);
    inner.update(a, a_len);
    if (b_len) inner.update(b, b_len);
    const auto ih = inner.final();
    Sha1 outer;
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x5C;
    outer.update(pad, 64);
    outer.update(ih.data(), ih.size());
    return outer.final();
}

// ---------------------------------------------------------------- the exports
namespace {

rt::Mem mem(const rt::Context& c) { return rt::Mem{c.mem, c.mem_size}; }

std::vector<uint8_t> guest_bytes(const rt::Context& c, uint32_t addr, uint32_t n) {
    std::vector<uint8_t> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = rt::ld8(mem(c), addr + i, 0);
    return v;
}

Sha1 load_context(const rt::Context& c, uint32_t ctx) {
    Sha1 s;
    for (int i = 0; i < 5; ++i) s.h[i] = rt::ld32(mem(c), ctx + 4 * uint32_t(i), 0);
    s.bytes = uint64_t(rt::ld32(mem(c), ctx + 20, 0)) | uint64_t(rt::ld32(mem(c), ctx + 24, 0)) << 32;
    for (uint32_t i = 0; i < 64; ++i) s.buffer[i] = rt::ld8(mem(c), ctx + 28 + i, 0);
    return s;
}

void store_context(const rt::Context& c, uint32_t ctx, const Sha1& s) {
    for (int i = 0; i < 5; ++i) rt::st32(mem(c), ctx + 4 * uint32_t(i), s.h[i], 0);
    rt::st32(mem(c), ctx + 20, uint32_t(s.bytes), 0);
    rt::st32(mem(c), ctx + 24, uint32_t(s.bytes >> 32), 0);
    for (uint32_t i = 0; i < 64; ++i) rt::st8(mem(c), ctx + 28 + i, s.buffer[i], 0);
}

// VOID XcSHAInit(PUCHAR pbSHAContext)
void xc_sha_init(rt::Context& c) {
    store_context(c, arg(c, 0), Sha1{});
    ret(c, c.eax, 4);
}

// VOID XcSHAUpdate(PUCHAR pbSHAContext, PUCHAR pbInput, ULONG dwInputLength)
void xc_sha_update(rt::Context& c) {
    Sha1 s = load_context(c, arg(c, 0));
    const auto data = guest_bytes(c, arg(c, 1), arg(c, 2));
    s.update(data.data(), data.size());
    store_context(c, arg(c, 0), s);
    ret(c, c.eax, 12);
}

// VOID XcSHAFinal(PUCHAR pbSHAContext, PUCHAR pbDigest)
void xc_sha_final(rt::Context& c) {
    Sha1 s = load_context(c, arg(c, 0));
    const auto d = s.final();
    for (uint32_t i = 0; i < 20; ++i) rt::st8(mem(c), arg(c, 1) + i, d[i], 0);
    store_context(c, arg(c, 0), s);
    ret(c, c.eax, 8);
}

// VOID XcHMAC(PBYTE pbKeyMaterial, ULONG cbKeyMaterial, PBYTE pbData,
//     ULONG cbData, PBYTE pbData2, ULONG cbData2, PBYTE HmacData): HMAC-SHA1
// of the two data blocks, one after the other.
void xc_hmac(rt::Context& c) {
    const auto key = guest_bytes(c, arg(c, 0), arg(c, 1));
    const auto a = guest_bytes(c, arg(c, 2), arg(c, 3));
    const auto b = guest_bytes(c, arg(c, 4), arg(c, 5));
    const auto d = hmac_sha1(key.data(), key.size(), a.data(), a.size(), b.data(), b.size());
    for (uint32_t i = 0; i < 20; ++i) rt::st8(mem(c), arg(c, 6) + i, d[i], 0);
    ret(c, c.eax, 28);
}

}  // namespace

void register_kernel_crypto() {
    struct Fn {
        const char* name;
        uint32_t arg_bytes;
        HleFn fn;
    };
    static constexpr Fn kFns[] = {
        {"XcSHAInit", 4, xc_sha_init},
        {"XcSHAUpdate", 12, xc_sha_update},
        {"XcSHAFinal", 8, xc_sha_final},
        {"XcHMAC", 28, xc_hmac},
    };
    for (const Fn& f : kFns) {
        const KernelExport* e = kernel_export(f.name);
        if (!e || e->data || e->arg_bytes != f.arg_bytes) {
            std::fprintf(stderr, "qilin: kernel replacement %s doesn't match the export table\n", f.name);
            std::abort();
        }
        register_hle(kernel_addr(e->ordinal), e->name, f.fn);
    }
}

}  // namespace qilin::host
