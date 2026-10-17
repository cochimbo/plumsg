#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plumsg {

// Downloads `url` (http or https) into `body`. Returns the HTTP status (200, 404...)
// or 0 if the server could not be reached. Blocks: call it from a worker thread.
// Windows uses WinINet, so it follows the system's proxy settings; elsewhere it runs
// curl. `url` must already be percent-encoded.
int httpGet(const std::string& url, std::vector<uint8_t>& body);

}  // namespace plumsg
