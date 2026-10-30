#include "launcher/core/remoteCheats.h"

#include <utility>

namespace kyty::core {

struct RemoteCheats::State {
	State(HttpClient& client, LoadedFn loaded): http(client), on_loaded(std::move(loaded)) {}

	HttpClient& http;
	LoadedFn    on_loaded;
	uint64_t    generation = 0;
	RequestId   request    = 0;

	void CancelRequest() {
		if (request != 0) {
			http.Cancel(request);
		}
		request = 0;
		generation++;
	}

	void Done(const RemoteCheatFile& file, const CheatGame& game, const std::string& body,
	          std::string error) {
		request = 0;
		RemoteCheatResult result;
		result.file = file;
		if (error.empty()) {
			if (auto document = ParseCheatDocument(body, file.name)) {
				if (auto problem = ValidateRemoteImport(*document, game.title_id, game.version,
				                                        game.process)) {
					error = std::move(*problem);
				} else {
					result.importable = true;
				}
				result.document = std::move(*document);
			} else {
				error = std::move(document).error();
			}
		}
		result.error = std::move(error);
		if (on_loaded) {
			on_loaded(result);
		}
	}
};

RemoteCheats::RemoteCheats(HttpClient& http, LoadedFn on_loaded)
    : m_state(std::make_shared<State>(http, std::move(on_loaded))) {}

RemoteCheats::~RemoteCheats() {
	// The cancelled request still ends with one DoneFn call (HttpClient contract); it finds the
	// state gone and does nothing.
	m_state->CancelRequest();
}

Expected<void, std::string> RemoteCheats::Fetch(const RemoteCheatFile& file, CheatGame game) {
	auto& state = *m_state;
	state.CancelRequest();
	if (!IsSupportedRemoteCheatFile(file)) {
		return MakeUnexpected(std::string(kUnsupportedCheatFormatMessage));
	}
	const std::weak_ptr<State> weak       = m_state;
	const auto                 generation = state.generation;
	state.request =
	    StartCheatDownload(state.http, RemoteCheatFileUrl(file), kMaxRemoteCheatFileBytes,
	                       [weak, generation, file, game = std::move(game)](const std::string& body,
	                                                                        std::string error) {
		                       const auto locked = weak.lock();
		                       if (locked && locked->generation == generation) {
			                       locked->Done(file, game, body, std::move(error));
		                       }
	                       });
	return {};
}

void RemoteCheats::Cancel() {
	m_state->CancelRequest();
}

bool RemoteCheats::Fetching() const {
	return m_state->request != 0;
}

} // namespace kyty::core
