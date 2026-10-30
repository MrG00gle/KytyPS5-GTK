#include "updater/core/sha256.h"

#include <algorithm>
#include <cstring>

namespace kyty::updater {

namespace {

// FIPS 180-4 section 4.2.2: the first 32 bits of the fractional parts of the cube roots of the
// first 64 primes.
constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// FIPS 180-4 section 5.3.3: the initial hash value.
constexpr std::array<uint32_t, 8> kInitialState = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

constexpr uint32_t RotateRight(uint32_t value, unsigned count) {
	return (value >> count) | (value << (32u - count));
}

} // namespace

Sha256::Sha256() {
	Reset();
}

void Sha256::Reset() {
	m_state      = kInitialState;
	m_block_size = 0;
	m_length     = 0;
}

// FIPS 180-4 section 6.2.2.
void Sha256::Compress(const uint8_t* block) {
	std::array<uint32_t, 64> w {};
	for (size_t t = 0; t < 16; t++) {
		w[t] = (uint32_t {block[4 * t]} << 24u) | (uint32_t {block[4 * t + 1]} << 16u) |
		       (uint32_t {block[4 * t + 2]} << 8u) | uint32_t {block[4 * t + 3]};
	}
	for (size_t t = 16; t < 64; t++) {
		const uint32_t s0 =
		    RotateRight(w[t - 15], 7) ^ RotateRight(w[t - 15], 18) ^ (w[t - 15] >> 3u);
		const uint32_t s1 =
		    RotateRight(w[t - 2], 17) ^ RotateRight(w[t - 2], 19) ^ (w[t - 2] >> 10u);
		w[t] = w[t - 16] + s0 + w[t - 7] + s1;
	}

	uint32_t a = m_state[0];
	uint32_t b = m_state[1];
	uint32_t c = m_state[2];
	uint32_t d = m_state[3];
	uint32_t e = m_state[4];
	uint32_t f = m_state[5];
	uint32_t g = m_state[6];
	uint32_t h = m_state[7];
	for (size_t t = 0; t < 64; t++) {
		const uint32_t sum1   = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
		const uint32_t choose = (e & f) ^ (~e & g);
		const uint32_t t1     = h + sum1 + choose + kRoundConstants[t] + w[t];
		const uint32_t sum0   = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
		const uint32_t major  = (a & b) ^ (a & c) ^ (b & c);
		const uint32_t t2     = sum0 + major;
		h                     = g;
		g                     = f;
		f                     = e;
		e                     = d + t1;
		d                     = c;
		c                     = b;
		b                     = a;
		a                     = t1 + t2;
	}
	m_state[0] += a;
	m_state[1] += b;
	m_state[2] += c;
	m_state[3] += d;
	m_state[4] += e;
	m_state[5] += f;
	m_state[6] += g;
	m_state[7] += h;
}

void Sha256::Update(std::span<const std::byte> bytes) {
	if (bytes.empty()) {
		return;
	}
	const auto* data      = reinterpret_cast<const uint8_t*>(bytes.data());
	size_t      remaining = bytes.size();
	m_length += remaining;
	if (m_block_size > 0) {
		const size_t count = std::min(remaining, m_block.size() - m_block_size);
		std::memcpy(m_block.data() + m_block_size, data, count);
		m_block_size += count;
		data += count;
		remaining -= count;
		if (m_block_size < m_block.size()) {
			return;
		}
		Compress(m_block.data());
		m_block_size = 0;
	}
	while (remaining >= m_block.size()) {
		Compress(data);
		data += m_block.size();
		remaining -= m_block.size();
	}
	if (remaining > 0) {
		std::memcpy(m_block.data(), data, remaining);
		m_block_size = remaining;
	}
}

// FIPS 180-4 section 5.1.1: a 1 bit, zeros up to 56 bytes modulo 64, and the length in bits as a
// 64-bit big-endian number.
Sha256Bytes Sha256::Final() {
	const uint64_t bit_length = m_length * 8;
	m_block[m_block_size++]   = 0x80;
	if (m_block_size > 56) {
		std::fill(m_block.begin() + static_cast<std::ptrdiff_t>(m_block_size), m_block.end(), 0);
		Compress(m_block.data());
		m_block_size = 0;
	}
	std::fill(m_block.begin() + static_cast<std::ptrdiff_t>(m_block_size), m_block.begin() + 56, 0);
	for (size_t i = 0; i < 8; i++) {
		m_block[56 + i] = static_cast<uint8_t>(bit_length >> (56u - 8u * i));
	}
	Compress(m_block.data());

	Sha256Bytes digest {};
	for (size_t i = 0; i < m_state.size(); i++) {
		digest[4 * i]     = static_cast<uint8_t>(m_state[i] >> 24u);
		digest[4 * i + 1] = static_cast<uint8_t>(m_state[i] >> 16u);
		digest[4 * i + 2] = static_cast<uint8_t>(m_state[i] >> 8u);
		digest[4 * i + 3] = static_cast<uint8_t>(m_state[i]);
	}
	Reset();
	return digest;
}

Sha256Bytes Sha256Digest(std::span<const std::byte> bytes) {
	Sha256 hash;
	hash.Update(bytes);
	return hash.Final();
}

} // namespace kyty::updater
