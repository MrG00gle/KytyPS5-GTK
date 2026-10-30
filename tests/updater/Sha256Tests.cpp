// Example tests of kyty::updater::Sha256 and Sha256Digest (src/updater/core/sha256.h, task 17.1):
// the FIPS 180-4 / NIST example vectors (the empty message, "abc", the 448-bit and 896-bit
// messages, one million "a"), the NIST short-message vectors of 8 and 32 bits, messages around the
// 55-, 56-, 63-, and 64-byte padding boundaries (reference values from coreutils sha256sum), every
// chunking of a message equal to the one-shot digest, and reuse after Final().

#include "support/updaterGenerators.h"
#include "updater/core/sha256.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kyty::updater::Sha256;
using kyty::updater::Sha256Digest;
using KytyTests::UpdaterGen::AsBytes;
using KytyTests::UpdaterGen::DigestHex;
using KytyTests::UpdaterGen::PatternBytes;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "Sha256Tests: failed: %s\n", message.c_str());
		std::abort();
	}
}

void CheckDigest(std::string_view message, std::string_view expected, const std::string& name) {
	const auto actual = DigestHex(Sha256Digest(AsBytes(message)));
	Check(actual == expected, name + ": " + actual + ", expected " + std::string(expected));
}

void TestNistExamples() {
	CheckDigest("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
	CheckDigest("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
	CheckDigest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
	            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "448 bits");
	CheckDigest(
	    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
	    "mnopqrsmnopqrstnopqrstu",
	    "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1", "896 bits");
	CheckDigest(std::string(1000000, 'a'),
	            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
	            "one million a");
	CheckDigest("\xbd", "68325720aabd7c82f30f554b313d0570c95accbb7dc4b5aae11204c08ffe732b",
	            "8 bits (0xbd)");
	CheckDigest("\xc9\x8c\x8e\x55",
	            "7abc22c0ae5af26ce93dbb94433a0e0b2e119d014f8e7f65bd56c61ccccd9504",
	            "32 bits (0xc98c8e55)");
}

// The padding needs a second block from 56 bytes on; 64 bytes are a whole block.
void TestPaddingBoundaries() {
	const std::vector<std::pair<size_t, std::string_view>> vectors = {
	    {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
	    {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
	    {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
	    {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
	    {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
	    {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
	    {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
	    {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"}};
	for (const auto& [length, expected]: vectors) {
		CheckDigest(std::string(length, 'a'), expected, std::to_string(length) + " times a");
	}
}

// Every split of a message into two parts, and chunks of every size from 1 to 130 bytes, give the
// one-shot digest; so do empty chunks between them.
void TestChunking() {
	const std::string message  = PatternBytes(300, 7);
	const auto        one_shot = Sha256Digest(AsBytes(message));
	for (size_t split = 0; split <= message.size(); split++) {
		Sha256 hash;
		hash.Update(AsBytes(std::string_view(message).substr(0, split)));
		hash.Update(AsBytes(std::string_view(message).substr(split)));
		Check(hash.Final() == one_shot, "two parts split at " + std::to_string(split));
	}
	for (size_t chunk = 1; chunk <= 130; chunk++) {
		Sha256 hash;
		for (size_t offset = 0; offset < message.size(); offset += chunk) {
			hash.Update(AsBytes(std::string_view(message).substr(offset, chunk)));
			hash.Update({});
		}
		Check(hash.Final() == one_shot, "chunks of " + std::to_string(chunk) + " bytes");
	}

	// One million "a" in chunks of 4,093 bytes.
	const std::string block(4093, 'a');
	Sha256            hash;
	size_t            remaining = 1000000;
	while (remaining > 0) {
		const size_t count = std::min(remaining, block.size());
		hash.Update(AsBytes(std::string_view(block).substr(0, count)));
		remaining -= count;
	}
	Check(DigestHex(hash.Final()) ==
	          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
	      "one million a in chunks");
}

// Final() starts a new message; a fresh object and a reused one agree.
void TestReuse() {
	Sha256 hash;
	hash.Update(AsBytes("abc"));
	Check(DigestHex(hash.Final()) ==
	          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
	      "first message");
	Check(DigestHex(hash.Final()) ==
	          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
	      "Final() again is the empty message");
	hash.Update(AsBytes("ab"));
	hash.Update(AsBytes("c"));
	Check(DigestHex(hash.Final()) ==
	          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
	      "after reuse");
}

} // namespace

int main() {
	TestNistExamples();
	TestPaddingBoundaries();
	TestChunking();
	TestReuse();
	std::printf("Sha256Tests: all tests passed\n");
	return 0;
}
