#include "updater/core/downloadVerifier.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace kyty::updater {

namespace {

std::function<int()>& FaultHook() {
	static std::function<int()> hook;
	return hook;
}

int InjectedFault() {
	const auto& hook = FaultHook();
	return hook ? hook() : 0;
}

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

// 0 or the errno of the first failed write.
int WriteAll(int fd, std::span<const std::byte> bytes) {
	constexpr size_t kMaxWrite = size_t {1} << 30;
	while (!bytes.empty()) {
		const ssize_t written = ::write(fd, bytes.data(), std::min(bytes.size(), kMaxWrite));
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return errno;
		}
		if (written == 0) {
			return EIO;
		}
		bytes = bytes.subspan(static_cast<size_t>(written));
	}
	return 0;
}

} // namespace

DownloadVerifier::DownloadVerifier(const ReleaseAsset& asset, std::filesystem::path out)
    : m_asset(asset), m_path(std::move(out)) {
	m_fd = ::open(m_path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0666);
	if (m_fd < 0) {
		const int error_number = errno;
		m_state                = State::Failed;
		m_error   = "Cannot create " + m_path.string() + ": " + ErrorText(error_number);
		m_failure = {DownloadFailureKind::Create, error_number, 0, 0};
		return;
	}
	m_created = true;
}

DownloadVerifier::~DownloadVerifier() {
	if (m_state != State::Finished) {
		CloseAndRemove();
	}
}

void DownloadVerifier::CloseAndRemove() {
	if (m_fd >= 0) {
		::close(m_fd);
		m_fd = -1;
	}
	if (m_created) {
		::unlink(m_path.c_str());
		m_created = false;
	}
}

Status DownloadVerifier::Fail(std::string message, DownloadFailure failure) {
	CloseAndRemove();
	m_state   = State::Failed;
	m_error   = std::move(message);
	m_failure = failure;
	return Failing();
}

// The stored failure again.
Status DownloadVerifier::Failing() {
	m_last_failure = m_failure;
	return kyty::core::MakeUnexpected(m_error);
}

Status DownloadVerifier::Append(std::span<const std::byte> bytes) {
	if (m_state == State::Failed) {
		return Failing();
	}
	if (m_state == State::Finished) {
		m_last_failure = {DownloadFailureKind::Finished, 0, 0, 0};
		return kyty::core::MakeUnexpected(std::string("The download is already finished."));
	}
	if (bytes.size() > m_asset.size - m_received) {
		return Fail(std::string(kDownloadTooLargeMessage),
		            {DownloadFailureKind::TooLarge, 0, 0, 0});
	}
	if (bytes.empty()) {
		return {};
	}
	int error_number = InjectedFault();
	if (error_number == 0) {
		error_number = WriteAll(m_fd, bytes);
	}
	if (error_number != 0) {
		return Fail("Cannot write " + m_path.string() + ": " + ErrorText(error_number),
		            {DownloadFailureKind::Write, error_number, 0, 0});
	}
	m_hash.Update(bytes);
	m_received += bytes.size();
	return {};
}

Status DownloadVerifier::Finish() {
	if (m_state == State::Finished) {
		return {};
	}
	if (m_state == State::Failed) {
		return Failing();
	}
	const int result       = ::close(m_fd);
	const int error_number = errno;
	m_fd                   = -1;
	if (result != 0) {
		return Fail("Cannot write " + m_path.string() + ": " + ErrorText(error_number),
		            {DownloadFailureKind::Write, error_number, 0, 0});
	}
	if (m_received != m_asset.size) {
		return Fail("The downloaded file is smaller than the release asset (" +
		                std::to_string(m_received) + " of " + std::to_string(m_asset.size) +
		                " bytes).",
		            {DownloadFailureKind::TooSmall, 0, m_received, m_asset.size});
	}
	if (m_hash.Final() != m_asset.sha256) {
		return Fail(std::string(kDownloadDigestMessage), {DownloadFailureKind::Digest, 0, 0, 0});
	}
	m_state = State::Finished;
	return {};
}

void DownloadVerifier::Abort() {
	if (m_state == State::Finished) {
		return;
	}
	CloseAndRemove();
	m_state   = State::Failed;
	m_error   = kDownloadAbortedMessage;
	m_failure = {DownloadFailureKind::Aborted, 0, 0, 0};
}

void SetDownloadWriteFaultForTesting(std::function<int()> fault) {
	FaultHook() = std::move(fault);
}

} // namespace kyty::updater
