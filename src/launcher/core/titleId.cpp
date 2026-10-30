#include "launcher/core/titleId.h"

#include "launcher/core/text.h"

namespace kyty::core {

std::string NormalizeTitleId(std::string_view title_id) {
	std::string title(TrimWhitespace(title_id));
	for (auto& c: title) {
		if (c >= 'a' && c <= 'z') {
			c = static_cast<char>(c - 'a' + 'A');
		}
	}
	return title;
}

} // namespace kyty::core
