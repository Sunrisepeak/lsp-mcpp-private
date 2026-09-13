// Where the engine and the semantic kit come from: the payload bundled with an
// editor extension, an xlings installation, or explicit paths (design 15.3).
export module lspmcpp.server.payload;

import std;
import lspmcpp.base.error;

export namespace lspmcpp::server {

struct PayloadPaths {
    std::string directory;     // empty when no payload is used
    std::string clangd;        // absolute executable path, or empty
    std::string clangdVersion;
    std::string kit;           // kit root directory, or empty
    std::string platform;
};

struct PayloadRequest {
    std::string payloadDirectory;   // --payload
    std::string clangd;             // --clangd
    std::string kit;                // --kit
};

// Explicit paths win; then the payload named or enclosing this executable
// (payload.json, else the conventional layout); then clangd on PATH and a kit
// installed by xlings (design 15.3).
PayloadPaths resolve_payload(const PayloadRequest& request);
// The macOS SDK path for kits that need one, or empty.
std::string macos_sdk_path();

} // namespace lspmcpp::server
