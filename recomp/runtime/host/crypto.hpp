// SHA-1 and HMAC-SHA1, for the kernel's Xc* crypto exports (kernel_crypto.cpp).
// XAPI signs saved games with them (XCalculateSignature*).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace qilin::host {

struct Sha1 {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t bytes = 0;    // message length so far
    uint8_t buffer[64] = {};

    void update(const uint8_t* data, size_t n);
    std::array<uint8_t, 20> final();
};

std::array<uint8_t, 20> hmac_sha1(const uint8_t* key, size_t key_len, const uint8_t* a, size_t a_len, const uint8_t* b = nullptr,
                                  size_t b_len = 0);

}  // namespace qilin::host
