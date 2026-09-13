module lspmcpp.project.mcpp;

import std;
import lspmcpp.base.error;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.base.log;
import lspmcpp.platform.fs;
import lspmcpp.platform.dirs;
import lspmcpp.platform.process;
import lspmcpp.spec.database;
import lspmcpp.project.detect;
import lspmcpp.project.compdb;
import lspmcpp.project.infer;
import lspmcpp.project.provider;

namespace lspmcpp::project {

std::string mcpp_package_name(std::string_view manifestText) {
    bool inPackage { false };
    for (auto line : base::split_lines(manifestText)) {
        line = base::trim(line);
        if (line.starts_with('[')) {
            inPackage = line == "[package]";
            continue;
        }
        if (!inPackage || !line.starts_with("name")) continue;
        const std::size_t equals { line.find('=') };
        if (equals == std::string_view::npos || base::trim(line.substr(0, equals)) != "name") continue;
        std::string_view value { base::trim(line.substr(equals + 1)) };
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) {
            const std::size_t close { value.find(value.front(), 1) };
            if (close != std::string_view::npos) return std::string { value.substr(1, close - 1) };
        }
    }
    return {};
}

base::Result<InferredDatabase> load_mcpp(const Detection& detection, const ProviderContext& context) {
    const std::string commandsPath { base::join_path(detection.root, "compile_commands.json") };
    if (context.trusted) {
        const std::string home { platform::dirs::home_directory() };
        const std::vector<std::string> fallbacks { base::join_path(home, ".mcpp/bin/mcpp"), base::join_path(home, ".xlings/subos/current/bin/mcpp") };
        if (auto mcpp = find_tool("mcpp", fallbacks)) {
            platform::SpawnOptions options;
            options.program = *mcpp;
            options.arguments = { "build", "--configure-only" };
            options.workDirectory = detection.root;
            auto result = platform::run(std::move(options), context.configureTimeout);
            if (!result) {
                base::log::warning("mcpp configure could not start: {}", result.error().message);
            } else if (result->timedOut || result->exitCode != 0) {
                base::log::warning("mcpp build --configure-only failed ({}): {}", result->exitCode, base::trim(result->error));
            }
        } else {
            base::log::info("mcpp was not found; using an existing compile_commands.json if there is one");
        }
    }
    if (!platform::fs::is_regular_file(commandsPath)) {
        return base::fail("mcpp-no-database", context.trusted ? "mcpp did not produce compile_commands.json"
                                                              : "the workspace is not trusted, so mcpp was not run");
    }
    auto commands = read_compile_commands(commandsPath);
    if (!commands) return std::unexpected { commands.error() };
    std::string name { std::string { base::file_name(detection.root) } };
    if (auto manifest = platform::fs::read_file(detection.manifest)) {
        if (std::string package { mcpp_package_name(*manifest) }; !package.empty()) name = package;
    }
    auto database = database_from_commands(*commands, name, context.scanner, context.prober);
    database.database.generator = spec::Generator { "lsp-mcpp", "mcpp compile_commands.json" };
    return database;
}

} // namespace lspmcpp::project
