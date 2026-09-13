// The language server session: one event loop owning all state, fed by the
// editor's standard input, the engine's output and background model loads
// (design sections 12.3 and 12.5).
export module lspmcpp.server.session;

import std;

export namespace lspmcpp::server {

struct SessionOptions {
    std::string payloadDirectory;
    std::string clangd;
    std::string kit;
    bool trusted { true };
    bool discoverCompilers { true };
    bool verboseEngineLog { false };
    std::chrono::milliseconds requestTimeout { std::chrono::seconds { 60 } };
};

// Serves the Language Server Protocol on standard input and output until the
// client exits. Returns the process exit code.
int run_session(const SessionOptions& options);

} // namespace lspmcpp::server
