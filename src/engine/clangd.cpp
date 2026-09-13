module lspmcpp.engine.clangd;

import std;
import nlohmann.json;
import lspmcpp.base.error;
import lspmcpp.base.text;
import lspmcpp.platform.process;
import lspmcpp.lsp.connection;

namespace lspmcpp::engine {

std::vector<std::string> clangd_arguments(const ClangdConfig& config) {
    std::vector<std::string> arguments {
        "--experimental-modules-support",
        "--use-dirty-headers",
        "--compile-commands-dir=" + config.databaseDirectory,
        "--background-index",
        "--header-insertion=never",
        "--pretty=false",
        config.verboseLog ? "--log=verbose" : "--log=error",
    };
    arguments.insert(arguments.end(), config.extraArguments.begin(), config.extraArguments.end());
    return arguments;
}

std::string parse_clangd_version(std::string_view output) {
    for (auto line : base::split_lines(output)) {
        const std::size_t marker { line.find("clangd version ") };
        if (marker == std::string_view::npos) continue;
        std::string_view rest { line.substr(marker + 15) };
        return std::string { rest.substr(0, rest.find_first_of(" \t\r")) };
    }
    return {};
}

base::Result<void> Clangd::start(const ClangdConfig& config, MessageHandler onMessage, ClosedHandler onClosed, LogHandler onLog) {
    stop(std::chrono::milliseconds { 200 });
    config_ = config;
    platform::SpawnOptions options;
    options.program = config.executable;
    options.arguments = clangd_arguments(config);
    options.workDirectory = config.workDirectory;
    auto connection = lsp::Connection::start(std::move(options), std::move(onMessage), std::move(onClosed), std::move(onLog));
    if (!connection) return std::unexpected { connection.error() };
    connection_ = std::move(*connection);
    return {};
}

base::Result<void> Clangd::send(const nlohmann::json& message) {
    if (!connection_) return base::fail("engine-stopped", "clangd is not running");
    return connection_->send(message);
}

void Clangd::stop(std::chrono::milliseconds grace) {
    if (!connection_) return;
    connection_->stop(grace);
    connection_.reset();
}

bool Clangd::running() const { return connection_ && !connection_->closed(); }

} // namespace lspmcpp::engine
