#include "support/gtk/testServices.h"

namespace KytyTests {

TestServices::TestServices(kyty::core::HttpClient& http, kyty::core::Scheduler* scheduler)
    : m_http(http), m_scheduler(scheduler != nullptr ? *scheduler : m_glib_scheduler) {}

TestServices::~TestServices() = default;

void TestServices::UsePosixEnvironment() {
	m_posix_environment = kyty::core::MakePosixEnvironment();
}

kyty::gtk::Services TestServices::Get() {
	const kyty::core::Environment& used = m_posix_environment
	                                          ? *m_posix_environment
	                                          : static_cast<kyty::core::Environment&>(environment);
	return {m_http,           m_scheduler,  used,       process_runner, file_chooser,
	        desktop_launcher, device_lists, controller, title_sound};
}

} // namespace KytyTests
