module lspmcpp.engine.clangd;

import std;
import nlohmann.json;
import lspmcpp.base.error;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.platform.fs;
import lspmcpp.platform.process;
import lspmcpp.lsp.connection;

namespace lspmcpp::engine {

namespace {

// usable plan W9.5: one row per clangd release line this server has been run against (design 15.1
// pins 23.1.x). Extend with a new row, never by changing behaviour under an existing version.
struct CapabilityRow {
    std::string_view versionPrefix;
    EngineCapabilities capabilities;
};

constexpr std::array<CapabilityRow, 1> CAPABILITY_TABLE { {
    { "23.", EngineCapabilities { .experimentalModulesSupport = true, .useDirtyHeaders = true,
                                  .persistentModuleCache = true, .msvcStlNeedsNoAlignedAllocation = true } },
} };

} // namespace

EngineCapabilities capabilities_for_clangd_version(std::string_view version) {
    for (const auto& row : CAPABILITY_TABLE) {
        if (version.starts_with(row.versionPrefix)) return row.capabilities;
    }
    return {};   // an unrecognized version: assume none of the optional behaviour
}

std::vector<std::string> clangd_arguments(const EngineConfig& config) {
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

std::optional<ModuleFailure> parse_module_failure(std::string_view line) {
    static constexpr std::string_view MARKER { "Failed to build module " };
    const std::size_t marker { line.find(MARKER) };
    if (marker == std::string_view::npos) return std::nullopt;
    std::string_view rest { line.substr(marker + MARKER.size()) };
    const std::size_t semicolon { rest.find(';') };
    if (semicolon == std::string_view::npos || semicolon == 0) return std::nullopt;
    ModuleFailure failure;
    failure.module = std::string { base::trim(rest.substr(0, semicolon)) };
    std::string_view reason { rest.substr(semicolon + 1) };
    if (const std::size_t due { reason.find("due to ") }; due != std::string_view::npos) reason = reason.substr(due + 7);
    if (const std::size_t hint { reason.find(" Use '--log=verbose'") }; hint != std::string_view::npos) reason = reason.substr(0, hint);
    reason = base::trim(reason);
    if (reason.ends_with('.')) reason.remove_suffix(1);
    failure.reason = std::string { reason };
    static constexpr std::string_view COMPILE { "Failed to compile " };
    if (reason.starts_with(COMPILE)) failure.failedSource = std::string { base::trim(reason.substr(COMPILE.size())) };
    return failure;
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

base::Result<void> Clangd::start(const EngineConfig& config, MessageHandler onMessage, ClosedHandler onClosed, LogHandler onLog) {
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

base::Result<void> Clangd::push_database(std::string_view compileCommandsJson) {
    // clangd rereads --compile-commands-dir/compile_commands.json itself (design 15.1); "pushing"
    // the database to this engine means writing it there atomically.
    if (auto created = platform::fs::create_directories(config_.databaseDirectory); !created) return created;
    return platform::fs::write_file_atomic(base::join_path(config_.databaseDirectory, "compile_commands.json"), std::string { compileCommandsJson });
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
