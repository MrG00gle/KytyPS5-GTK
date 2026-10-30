// Feature: linux-gtk-modernization, Property 42: Download verification
//
// Validates Requirements 13.7 and 13.8. Each case publishes an asset (0 bytes, up to 5,000 bytes,
// or up to 300,000 bytes, with its SHA-256 and size) and receives a byte stream that is exact,
// corrupted at a random byte, truncated, extended beyond the published size, interrupted by an
// injected write error at a random chunk, or exact against a published digest that differs; the
// stream is split into random chunks (empty ones included). The DownloadVerifier writes into a
// workspace inside a real installation directory. It must accept exactly when the total size
// equals the published size and the streamed digest equals the published one; the streamed digest
// must equal the one-shot SHA-256 for that chunking; an extended stream must fail in Append()
// before anything beyond the size is accepted, a write error in Append(), and every other
// rejection in Finish(). On acceptance the file holds the asset's bytes; on rejection the file is
// gone and the installation directory is byte-identical to before.

#include "support/property.h"
#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/core/downloadVerifier.h"
#include "updater/core/sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace UpdaterGen = KytyTests::UpdaterGen;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

enum class Stream { Exact, Corrupted, Truncated, Extended, WriteError, WrongDigest };

const char* StreamName(Stream stream) {
	switch (stream) {
		case Stream::Exact: return "exact";
		case Stream::Corrupted: return "corrupted";
		case Stream::Truncated: return "truncated";
		case Stream::Extended: return "extended";
		case Stream::WriteError: return "write error";
		case Stream::WrongDigest: return "wrong published digest";
	}
	return "?";
}

// Removes the write fault hook however the case ends (RC_ASSERT throws).
struct FaultHookReset {
	~FaultHookReset() { kyty::updater::SetDownloadWriteFaultForTesting({}); }
};

bool Exists(const fs::path& path) {
	std::error_code ec;
	return fs::symlink_status(path, ec).type() != fs::file_type::not_found && !ec;
}

} // namespace

KYTY_PROPERTY(42, "Download verification") {
	return rc::check(name, []() {
		const KytyTests::TempDir temp("kyty_p42");
		temp.WriteFile("install/kyty_launcher", "launcher");
		temp.WriteFile("install/kyty_emulator",
		               UpdaterGen::PatternBytes(Choose<size_t>(0, 2000), 1));
		temp.WriteFile("install/assets/sounds/trophy-unlock.wav", "wav");
		const auto install   = temp.Path() / "install";
		const auto workspace = temp.MakeDirectories("install/.kyty-update-Ab12Cd");
		const auto out       = workspace / "package-Linux-x86_64.tar.gz";
		const auto before    = UpdaterGen::SnapshotTree(install);

		// The published asset.
		const size_t      size  = OneIn(12) ? 0 : Choose<size_t>(1, OneIn(5) ? 300001 : 5001);
		const std::string bytes = UpdaterGen::PatternBytes(size, Choose<uint32_t>(0, 1000000));
		kyty::updater::ReleaseAsset asset {"v1-Linux-x86_64.tar.gz", UpdaterGen::DigestOf(bytes),
		                                   size};

		// The received stream.
		// Exact streams twice as often as each kind of rejection.
		std::vector<Stream> kinds = {Stream::Exact, Stream::Exact, Stream::Extended,
		                             Stream::WrongDigest};
		if (size > 0) {
			kinds.insert(kinds.end(), {Stream::Corrupted, Stream::Truncated, Stream::WriteError});
		}
		const Stream stream   = kinds[Choose<size_t>(0, kinds.size())];
		std::string  received = bytes;
		switch (stream) {
			case Stream::Corrupted:
				received[Choose<size_t>(0, size)] ^= static_cast<char>(Choose(1, 256));
				break;
			case Stream::Truncated: received.resize(Choose<size_t>(0, size)); break;
			case Stream::Extended:
				received += UpdaterGen::PatternBytes(Choose<size_t>(1, 5000), 77);
				break;
			case Stream::WrongDigest:
				asset.sha256[Choose<size_t>(0, 32)] ^= static_cast<uint8_t>(Choose(1, 256));
				break;
			case Stream::Exact:
			case Stream::WriteError: break;
		}

		// Random chunks: sizes from 0 to about a third of the stream.
		std::vector<size_t> sizes;
		const int           chunk_count = Choose(0, 12);
		for (int i = 0; i < chunk_count; i++) {
			sizes.push_back(OneIn(5) ? 0 : Choose<size_t>(1, received.size() / 3 + 2));
		}
		const auto          chunks = UpdaterGen::SplitBytes(received, sizes);
		std::vector<size_t> non_empty;
		for (size_t i = 0; i < chunks.size(); i++) {
			if (!chunks[i].empty()) {
				non_empty.push_back(i);
			}
		}

		// The streamed digest of this chunking equals the one-shot digest.
		kyty::updater::Sha256 streamed;
		for (const auto chunk: chunks) {
			streamed.Update(UpdaterGen::AsBytes(chunk));
		}
		RC_ASSERT(streamed.Final() == UpdaterGen::DigestOf(received));

		// The injected write error: at one of the non-empty chunks.
		size_t               failing_chunk = chunks.size();
		int                  write_calls   = 0;
		const FaultHookReset reset;
		if (stream == Stream::WriteError) {
			failing_chunk = non_empty[Choose<size_t>(0, non_empty.size())];
			const int failing_call =
			    static_cast<int>(std::find(non_empty.begin(), non_empty.end(), failing_chunk) -
			                     non_empty.begin()) +
			    1;
			const int error_number = OneIn(2) ? ENOSPC : EIO;
			kyty::updater::SetDownloadWriteFaultForTesting(
			    [&write_calls, failing_call, error_number] {
				    write_calls++;
				    return write_calls == failing_call ? error_number : 0;
			    });
		}
		RC_LOG() << StreamName(stream) << ": size " << size << ", received " << received.size()
		         << " bytes in " << chunks.size() << " chunks\n";

		bool        accepted = true;
		std::string append_error;
		size_t      failed_at = chunks.size();
		{
			kyty::updater::DownloadVerifier verifier(asset, out);
			for (size_t i = 0; i < chunks.size(); i++) {
				const auto status = verifier.Append(UpdaterGen::AsBytes(chunks[i]));
				if (!status) {
					append_error = status.error();
					failed_at    = i;
					break;
				}
				RC_ASSERT(verifier.Received() <= size);
			}
			const auto finish = verifier.Finish();
			accepted          = append_error.empty() && finish.has_value();
			if (!append_error.empty()) {
				RC_ASSERT(!finish.has_value());
				RC_ASSERT(finish.error() == append_error);
			}
		}

		const bool expected = stream == Stream::Exact;
		RC_ASSERT(accepted == expected);
		switch (stream) {
			case Stream::Extended: {
				RC_ASSERT(append_error == kyty::updater::kDownloadTooLargeMessage);
				// The chunk that fails is the first one that would pass the size.
				size_t total = 0;
				for (size_t i = 0; i < failed_at; i++) {
					total += chunks[i].size();
				}
				RC_ASSERT(total <= size);
				RC_ASSERT(total + chunks[failed_at].size() > size);
				break;
			}
			case Stream::WriteError:
				RC_ASSERT(failed_at == failing_chunk);
				RC_ASSERT(append_error.starts_with("Cannot write " + out.string() + ": "));
				break;
			default: RC_ASSERT(append_error.empty()); break;
		}

		if (accepted) {
			RC_ASSERT(UpdaterGen::ReadFile(out) == bytes);
			auto after = UpdaterGen::SnapshotTree(install);
			RC_ASSERT(after.erase(".kyty-update-Ab12Cd/package-Linux-x86_64.tar.gz") == 1u);
			RC_ASSERT(after == before);
		} else {
			RC_ASSERT(!Exists(out));
			RC_ASSERT(UpdaterGen::SnapshotTree(install) == before);
		}
		RC_TAG(StreamName(stream));
	});
}
