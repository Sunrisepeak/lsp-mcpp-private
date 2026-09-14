// The semantic engine: a pinned clangd started over openkal with the engine
// database of one context (design section 15.1), implementing the
// lspmcpp.engine interface (usable plan W9.5).
export module lspmcpp.engine.clangd;

import std;
import nlohmann.json;
import lspmcpp.base.error;
import lspmcpp.lsp.connection;
import lspmcpp.toolchain.probe;
import lspmcpp.engine;

export namespace lspmcpp::engine {

inline constexpr std::string_view PINNED_CLANGD_MAJOR { "23" };

std::vector<std::string> clangd_arguments(const EngineConfig& config);

// The capability table of usable plan W9.5, keyed by clangd version: which
// startup flags it honors meaningfully, whether its module cache survives a
// restart, and whether MSVC STL contexts need aligned allocation turned off
// (W1.4). An unrecognized version gets the all-false, most conservative row.
EngineCapabilities capabilities_for_clangd_version(std::string_view version);

// clangd's log line for a module it could not build:
// "E[..] Failed to build module greet; due to Failed to compile C:/.../std.ixx. Use '--log=verbose' ..."
struct ModuleFailure {
    std::string module;
    std::string reason;
    std::string failedSource;   // the source that did not compile, when the reason names one
};
std::optional<ModuleFailure> parse_module_failure(std::string_view line);
// "clangd version 23.1.0 (https://github.com/llvm/llvm-project ea7d852a70e8...)" -> "23.1.0"
std::string parse_clangd_version(std::string_view output);

class Clangd : public Engine {
private:
    std::unique_ptr<lsp::Connection> connection_;
    EngineConfig config_;

public:
    base::Result<void> start(const EngineConfig& config, MessageHandler onMessage, ClosedHandler onClosed, LogHandler onLog) override;
    base::Result<void> push_database(std::string_view compileCommandsJson) override;
    base::Result<void> send(const nlohmann::json& message) override;
    void stop(std::chrono::milliseconds grace) override;
    bool running() const override;
    const EngineConfig& config() const override { return config_; }
    EngineCapabilities capabilities() const override { return capabilities_for_clangd_version(config_.version); }
};

} // namespace lspmcpp::engine
