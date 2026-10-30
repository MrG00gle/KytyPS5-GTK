#pragma once

// Streams a release asset to a file while checking it against the published size and SHA-256
// (task 17.1; design.md "Updater interfaces"; Requirements 13.7, 13.8). The update session feeds
// it the body chunks of the download; nothing is extracted unless Finish() succeeds.

#include "launcher/core/expected.h"
#include "updater/core/releaseMetadata.h"
#include "updater/core/sha256.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace kyty::updater {

// Success, or the reason of the failure.
using Status = kyty::core::Expected<void, std::string>;

inline constexpr std::string_view kDownloadTooLargeMessage =
    "The downloaded file is larger than the release asset.";
inline constexpr std::string_view kDownloadDigestMessage =
    "The update failed its SHA-256 verification. Please try again.";
inline constexpr std::string_view kDownloadAbortedMessage = "The download was cancelled.";

class DownloadVerifier {
public:
	// Creates out with O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC and mode 0666 minus the umask. When
	// that fails (out exists, or its directory does not), Append() and Finish() return the reason
	// and nothing is deleted.
	DownloadVerifier(const ReleaseAsset& asset, std::filesystem::path out);
	// Deletes the file unless Finish() succeeded.
	~DownloadVerifier();

	DownloadVerifier(const DownloadVerifier&)            = delete;
	DownloadVerifier& operator=(const DownloadVerifier&) = delete;

	// Writes bytes and adds them to the digest. Fails, deleting the file, when the total would
	// exceed asset.size (kDownloadTooLargeMessage, before anything of bytes is written) or a write
	// fails ("Cannot write <out>: <reason>"). After any failure every call returns that failure.
	Status Append(std::span<const std::byte> bytes);

	// Closes the file and accepts it exactly when the total equals asset.size and the digest equals
	// asset.sha256; otherwise deletes the file ("The downloaded file is smaller than the release
	// asset (<received> of <size> bytes).", kDownloadDigestMessage, or a close error). After a
	// successful Finish() the file stays, Finish() succeeds again, and Append() fails.
	Status Finish();

	// Stops the download: closes and deletes the file unless Finish() already succeeded; later
	// calls return kDownloadAbortedMessage.
	void Abort();

	[[nodiscard]] uint64_t                     Received() const { return m_received; }
	[[nodiscard]] const std::filesystem::path& Path() const { return m_path; }

private:
	enum class State { Open, Failed, Finished };

	Status Fail(std::string message);
	void   CloseAndRemove();

	ReleaseAsset          m_asset;
	std::filesystem::path m_path;
	int                   m_fd      = -1;
	bool                  m_created = false;
	State                 m_state   = State::Open;
	std::string           m_error;
	uint64_t              m_received = 0;
	Sha256                m_hash;
};

// Tests only, not thread-safe: before each write of a non-empty Append(), the verifier calls
// fault(); a non-zero return value is the errno that the write then fails with, without writing.
// An empty function removes the hook.
void SetDownloadWriteFaultForTesting(std::function<int()> fault);

} // namespace kyty::updater
