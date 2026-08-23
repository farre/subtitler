#pragma once

#include <string>
#include <string_view>
#include <vector>

// The route headers stay includable without libsoup's include path:
// only the web library's sources see soup.h.
typedef struct _SoupServerMessage SoupServerMessage;

namespace subtitler {

// Hand-rolled JSON response helpers shared by the route modules.
std::string JsonEscape(std::string_view text);
void RespondJson(SoupServerMessage* message, const std::string& json);
// Answers the entries as a JSON string array.
void RespondStringList(SoupServerMessage* message,
                       const std::vector<std::string>& entries);

}  // namespace subtitler
