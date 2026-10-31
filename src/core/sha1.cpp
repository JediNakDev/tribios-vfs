#include "core/sha1.hpp"

#include <bit>
#include <string>

namespace tribios {
namespace {

void compress_sha1_block(std::array<std::uint32_t, 5>& state, const unsigned char* block) {
  std::array<std::uint32_t, 80> words{};
  for (int i = 0; i < 16; ++i) {
    words[i] = (std::uint32_t{block[i * 4]} << 24) | (std::uint32_t{block[i * 4 + 1]} << 16) |
               (std::uint32_t{block[i * 4 + 2]} << 8) | std::uint32_t{block[i * 4 + 3]};
  }
  for (int i = 16; i < 80; ++i) {
    words[i] = std::rotl(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
  }

  auto [a, b, c, d, e] = state;
  for (int i = 0; i < 80; ++i) {
    std::uint32_t mixed = 0;
    std::uint32_t constant = 0;
    if (i < 20) {
      mixed = (b & c) | (~b & d);
      constant = 0x5a827999;
    } else if (i < 40) {
      mixed = b ^ c ^ d;
      constant = 0x6ed9eba1;
    } else if (i < 60) {
      mixed = (b & c) | (b & d) | (c & d);
      constant = 0x8f1bbcdc;
    } else {
      mixed = b ^ c ^ d;
      constant = 0xca62c1d6;
    }
    const std::uint32_t next = std::rotl(a, 5) + mixed + e + constant + words[i];
    e = d;
    d = c;
    c = std::rotl(b, 30);
    b = a;
    a = next;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

}  // namespace

std::array<std::uint8_t, 20> sha1_digest(std::string_view data) {
  std::array<std::uint32_t, 5> state{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
  const std::size_t whole_blocks = data.size() / 64;
  for (std::size_t i = 0; i < whole_blocks; ++i) {
    compress_sha1_block(state, reinterpret_cast<const unsigned char*>(data.data()) + i * 64);
  }

  // The tail is padded with 0x80, zeros, and the message length in bits, which
  // spills into a second block when fewer than nine bytes remain.
  std::string tail(data.substr(whole_blocks * 64));
  tail.push_back(static_cast<char>(0x80));
  while (tail.size() % 64 != 56) tail.push_back('\0');
  const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8;
  for (int shift = 56; shift >= 0; shift -= 8) {
    tail.push_back(static_cast<char>((bit_length >> shift) & 0xff));
  }
  for (std::size_t offset = 0; offset < tail.size(); offset += 64) {
    compress_sha1_block(state, reinterpret_cast<const unsigned char*>(tail.data()) + offset);
  }

  std::array<std::uint8_t, 20> digest{};
  for (int i = 0; i < 5; ++i) {
    digest[i * 4] = static_cast<std::uint8_t>(state[i] >> 24);
    digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
    digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
    digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
  }
  return digest;
}

}  // namespace tribios
