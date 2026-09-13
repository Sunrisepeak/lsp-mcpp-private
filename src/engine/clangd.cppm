// The semantic engine: a pinned clangd started over openkal with the engine
// database of one context (design section 15.1).
export module lspmcpp.engine.clangd;

import std;
import nlohmann.json;
import lspmcpp.base.error;
import lspmcpp.lsp.connection;
import lspmcpp.toolchain.probe;

export namespace lspmcpp::engine {

inline constexpr std::string_view PINNED_CLANGD_MAJOR { "23" };

struct ClangdConfig {
    std::string executable;
    std::string databaseDirectory;             // --compile-commands-dir
    std::string workDirectory;
    std::vector<std::string> extraArguments;
    bool verboseLog { false };
};

std::vector<std::string> clangd_arguments(const ClangdConfig& config);
// "clangd version 23.1.0 (https://github.com/llvm/llvm-project ea7d852a70e8...)" -> "23.1.0"
std::string parse_clangd_version(std::string_view output);

class Clangd {
public:
    using MessageHandler = lsp::Connection::MessageHandler;
    using ClosedHandler = lsp::Connection::ClosedHandler;
    using LogHandler = lsp::Connection::ErrorLineHandler;

private:
    std::unique_ptr<lsp::Connection> connection_;
    ClangdConfig config_;

public:
    base::Result<void> start(const ClangdConfig& config, MessageHandler onMessage, ClosedHandler onClosed, LogHandler onLog);
    base::Result<void> send(const nlohmann::json& message);
    void stop(std::chrono::milliseconds grace);
    bool running() const;
    const ClangdConfig& config() const { return config_; }
};

} // namespace lspmcpp::engine
