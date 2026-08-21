#pragma once

#include <string>
#include <string_view>

// The route headers stay includable without libsoup's include path:
// only the web library's sources see soup.h.
typedef struct _SoupServerMessage SoupServerMessage;

namespace subtitler {

// Hand-rolled JSON response helpers shared by the route modules.
std::string JsonEscape(std::string_view text);
void RespondJson(SoupServerMessage* message, const std::string& json);

}  // namespace subtitler
