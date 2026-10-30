#pragma once

// SHA-256 (FIPS 180-4) for the Updater's download verification (task 17.1; Requirement 13.7).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace kyty::updater {

using Sha256Bytes = std::array<uint8_t, 32>;

// Streaming SHA-256: Update() any number of times, in chunks of any size, then Final(). The digest
// does not depend on how the message is split into chunks.
class Sha256 {
public:
	Sha256();

	void Update(std::span<const std::byte> bytes);

	// The digest of everything passed to Update() since construction or the previous Final(); the
	// object then starts a new message.
	Sha256Bytes Final();

private:
	void Reset();
	void Compress(const uint8_t* block);

	std::array<uint32_t, 8> m_state {};
	std::array<uint8_t, 64> m_block {};
	size_t                  m_block_size = 0; // bytes waiting in m_block
	uint64_t                m_length     = 0; // message length in bytes
};

// The SHA-256 digest of bytes in one call.
Sha256Bytes Sha256Digest(std::span<const std::byte> bytes);

} // namespace kyty::updater
