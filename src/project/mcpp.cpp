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
import lspmcpp.spec.discovery;
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

namespace {

// S2 0.2 single-document mode against mcpp's machine-output protocol (mcpp-community/mcpp#636):
// `mcpp --protocol-version` advertises the kind, `mcpp emit build-database --format json` answers it.
std::optional<InferredDatabase> emit_build_database(const std::string& mcpp, const Detection& detection, const ProviderContext& context) {
    platform::SpawnOptions query;
    query.program = mcpp;
    query.arguments = { "--protocol-version" };
    query.workDirectory = detection.root;
    auto answered = platform::run(std::move(query), std::chrono::seconds { 20 });
    if (!answered || answered->timedOut) return std::nullopt;
    auto protocol = spec::parse_producer_protocol(answered->output);
    if (!protocol || !protocol->kinds.contains("mcpp.build-database")) {
        base::log::info("mcpp does not produce build databases (no mcpp.build-database kind); using its compile database");
        return std::nullopt;
    }
    const std::vector<std::string> command { mcpp, "emit", "build-database", "--format", "json" };
    auto document = spec::run_database_command(command, detection.root, context.configureTimeout);
    if (!document) {
        base::log::warning("mcpp emit build-database failed: {}", document.error().message);
        return std::nullopt;
    }
    if (std::ranges::find(document->effects, std::string_view { "write-project" }) != document->effects.end()) {
        base::log::warning("mcpp emit build-database reports that it wrote into the project");
    }
    auto database = spec::from_json(document->database, detection.root);
    if (!database) {
        base::log::warning("mcpp's build database cannot be read: {}", database.error().message);
        return std::nullopt;
    }
    auto enriched = enrich_database(std::move(*database), context.scanner, context.prober);
    enriched.watch = std::move(document->watch);
    return enriched;
}

} // namespace

base::Result<InferredDatabase> load_mcpp(const Detection& detection, const ProviderContext& context) {
    const std::string commandsPath { base::join_path(detection.root, "compile_commands.json") };
    std::vector<std::pair<std::string, std::string>> notices;
    if (context.trusted) {
        const std::string home { platform::dirs::home_directory() };
        const std::vector<std::string> fallbacks { base::join_path(home, ".mcpp/bin/mcpp"), base::join_path(home, ".xlings/subos/current/bin/mcpp") };
        const std::optional<std::string> mcpp { context.mcppExecutable.empty() ? find_tool("mcpp", fallbacks) : std::optional<std::string> { context.mcppExecutable } };
        if (mcpp) {
            if (auto emitted = emit_build_database(*mcpp, detection, context)) return std::move(*emitted);
            // This mcpp cannot describe the build without configuring it, which writes into the project.
            notices.emplace_back("producer-writes-project",
                                 "this mcpp has no emit build-database; mcpp build --configure-only writes compile_commands.json and target/ into the project");
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
    database.notices = std::move(notices);
    return database;
}

} // namespace lspmcpp::project
