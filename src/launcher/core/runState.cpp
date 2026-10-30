#include "launcher/core/runState.h"

#include <utility>

namespace kyty::core {

namespace {

bool Alive(RunPhase phase) {
	return phase == RunPhase::Running || phase == RunPhase::Stopping;
}

} // namespace

bool RunState::CanRun(const EntryId& /*entry*/) const {
	return m_phase == RunPhase::Idle;
}

bool RunState::CanStop() const {
	return Alive(m_phase);
}

bool RunState::CanCompress(const EntryId& entry) const {
	return !Alive(m_phase) || m_running != entry;
}

bool RunState::CanRemoveSaveData(const EntryId& entry) const {
	return CanCompress(entry);
}

bool RunState::CanInstallUpdate() const {
	return !Alive(m_phase);
}

std::string_view RunState::RefusalMessage() {
	return kInstallRefusalMessage;
}

std::optional<EntryId> RunState::Running() const {
	if (!Alive(m_phase)) {
		return std::nullopt;
	}
	return m_running;
}

void RunState::Started(EntryId entry) {
	if (m_phase != RunPhase::Idle) {
		return;
	}
	m_phase   = RunPhase::Running;
	m_running = std::move(entry);
}

void RunState::StopRequested() {
	if (m_phase == RunPhase::Running) {
		m_phase = RunPhase::Stopping;
	}
}

ExitReport RunState::Exited(int wait_status) {
	ExitReport report;
	report.description = DescribeWaitStatus(wait_status);
	if (!Alive(m_phase)) {
		return report;
	}
	report.user_stopped = m_phase == RunPhase::Stopping;
	if (!report.user_stopped && !report.description.normal) {
		report.abnormal_message = report.description.text;
	}
	m_phase = RunPhase::Idle;
	m_running.reset();
	return report;
}

void RunState::InstallStarted() {
	if (m_phase == RunPhase::Idle) {
		m_phase = RunPhase::Installing;
	}
}

void RunState::InstallFinished() {
	if (m_phase == RunPhase::Installing) {
		m_phase = RunPhase::Idle;
	}
}

} // namespace kyty::core
