module lspmcpp.project.mcpp;

import std;
import nlohmann.json;
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

std::vector<CompileCommand> mcpp_standard_units(std::span<const CompileCommand> commands) {
    static constexpr std::string_view STAGED { "-fmodule-file=std=" };
    std::string stagedBmi;
    for (const auto& command : commands) {
        const auto argument = std::ranges::find_if(command.arguments, [](const std::string& word) { return word.starts_with(STAGED); });
        if (argument == command.arguments.end()) continue;
        stagedBmi = argument->substr(STAGED.size());
        if (!base::is_absolute_path(stagedBmi)) stagedBmi = base::join_path(command.directory, stagedBmi);
        break;
    }
    if (stagedBmi.empty()) return {};
    // <build directory>/pcm.cache/std.pcm, staged by `build pcm.cache/std.pcm : stage_file <cache>/pcm.cache/std.pcm`.
    const std::string buildDirectory { base::parent_path(base::parent_path(stagedBmi)) };
    const auto ninja = platform::fs::read_file(base::join_path(buildDirectory, "build.ninja"));
    if (!ninja) return {};
    static constexpr std::string_view RULE { "build pcm.cache/std.pcm : stage_file " };
    std::string cachedBmi;
    for (const auto line : base::split_lines(*ninja)) {
        if (!line.starts_with(RULE)) continue;
        const std::string_view rest { line.substr(RULE.size()) };
        cachedBmi = std::string { base::trim(rest.substr(0, rest.find_first_of(" |"))) };
        break;
    }
    if (cachedBmi.empty()) return {};
    const std::string recordPath { base::join_path(base::parent_path(base::parent_path(cachedBmi)), "std-module.json") };
    const auto recordText = platform::fs::read_file(recordPath);
    if (!recordText) return {};
    const nlohmann::json record = nlohmann::json::parse(*recordText, nullptr, false);
    if (!record.is_object() || record.value("schema", 0) != 1) return {};

    std::vector<CompileCommand> units;
    auto add = [&](const char* sourceKey, const char* commandsKey) {
        const std::string source { record.value(sourceKey, std::string {}) };
        if (source.empty() || !record.contains(commandsKey) || !record[commandsKey].is_array()) return;
        for (const auto& text : record[commandsKey]) {
            if (!text.is_string()) continue;
            // `cd '<dir>' && env NAME=value... '<compiler>' <arguments> --precompile '<source>' -o '<bmi>' 2>&1`
            const auto words = split_command(text.get<std::string>(), CommandSyntax::posix);
            if (std::ranges::find(words, std::string_view { "--precompile" }) == words.end()) continue;
            CompileCommand unit;
            unit.directory = base::parent_path(recordPath);
            unit.file = source;
            std::size_t i { 0 };
            if (words.size() > 3 && words[0] == "cd" && words[2] == "&&") {
                unit.directory = words[1];
                i = 3;
            }
            if (i < words.size() && words[i] == "env") {
                for (++i; i < words.size() && words[i].find('=') != std::string::npos && !words[i].starts_with('-'); ++i) {}
            }
            for (; i < words.size(); ++i) {
                const std::string& word { words[i] };
                if (word == "2>&1" || word == "--precompile") continue;
                if (word == "-o") {
                    ++i;
                    continue;
                }
                if (word.starts_with("-fmodule-file=")) continue;
                unit.arguments.push_back(word);
            }
            if (unit.arguments.empty()) return;
            if (std::ranges::find(unit.arguments, source) == unit.arguments.end()) unit.arguments.push_back(source);
            units.push_back(std::move(unit));
            return;
        }
    };
    add("std_module_source", "std_build_commands");
    add("std_compat_source", "std_compat_build_commands");
    return units;
}

namespace {

// What a failed command said, on one line: its standard error, or its output when that is empty.
// An mcpp that a project's .xlings.json selects but that is not installed fails before it starts,
// and xlings says so there.
std::string explanation(const platform::RunResult& result) {
    std::string text;
    const std::string& said { base::trim(result.error).empty() ? result.output : result.error };
    for (const std::string_view line : base::split_lines(said)) {
        const std::string_view trimmed { base::trim(line) };
        if (trimmed.empty()) continue;
        if (!text.empty()) text += "; ";
        for (std::size_t i { 0 }; i < trimmed.size(); ++i) {
            if (trimmed[i] == ' ' && i > 0 && trimmed[i - 1] == ' ') continue;   // alignment
            text += trimmed[i];
        }
    }
    if (text.size() > 400) text = text.substr(0, 400) + "...";
    return text;
}

// S2 0.2 single-document mode against mcpp's machine-output protocol (mcpp-community/mcpp#636):
// `mcpp --protocol-version` advertises the kind, `mcpp emit build-database --format json` answers it.
// nullopt when this mcpp cannot be asked, with `version` set when it said which it is; an mcpp that
// can be asked and fails says why, and nothing else is tried: configuring instead would write into
// the project and hide what mcpp reported. The query runs in the project, whose .xlings.json may
// select another mcpp than the one on PATH.
std::optional<base::Result<InferredDatabase>> emit_build_database(const std::string& mcpp, const Detection& detection, const ProviderContext& context,
                                                                  std::string& version) {
    platform::SpawnOptions query;
    query.program = mcpp;
    query.arguments = { "--protocol-version" };
    query.workDirectory = detection.root;
    auto answered = platform::run(std::move(query), std::chrono::seconds { 20 });
    if (!answered || answered->timedOut) return std::nullopt;
    if (answered->exitCode != 0) {
        base::log::info("mcpp --protocol-version failed ({}): {}", answered->exitCode, explanation(*answered));
        return std::nullopt;
    }
    const nlohmann::json described = nlohmann::json::parse(answered->output, nullptr, false);
    if (described.is_object()) {
        if (const auto producer = described.find("mcpp"); producer != described.end() && producer->is_object()) {
            version = producer->value("version", std::string {});
        }
    }
    auto protocol = spec::parse_producer_protocol(answered->output);
    if (!protocol || !protocol->kinds.contains("mcpp.build-database")) {
        base::log::info("mcpp does not produce build databases (no mcpp.build-database kind); using its compile database");
        return std::nullopt;
    }
    if (const auto effects = protocol->commandEffects.find("emit build-database");
        effects != protocol->commandEffects.end() && !spec::effects_acceptable(effects->second)) {
        base::log::warning("mcpp emit build-database declares that it writes into the project; not running it");
        return std::nullopt;
    }
    const std::vector<std::string> command { mcpp, "emit", "build-database", "--format", "json" };
    auto document = spec::run_database_command(command, detection.root, context.configureTimeout);
    if (!document) {
        base::log::warning("mcpp emit build-database failed: {}", document.error().message);
        return base::fail(document.error().code, std::format("mcpp emit build-database failed: {}", document.error().message));
    }
    if (std::ranges::find(document->effects, std::string_view { "write-project" }) != document->effects.end()) {
        base::log::warning("mcpp emit build-database reports that it wrote into the project");
    }
    auto database = spec::from_json(document->database, detection.root);
    if (!database) {
        base::log::warning("mcpp's build database cannot be read: {}", database.error().message);
        return base::fail(database.error().code, std::format("mcpp's build database cannot be read: {}", database.error().message));
    }
    auto enriched = enrich_database(std::move(*database), context.scanner, context.prober);
    enriched.watch = std::move(document->watch);
    return base::Result<InferredDatabase> { std::move(enriched) };
}

} // namespace

base::Result<InferredDatabase> load_mcpp(const Detection& detection, const ProviderContext& context) {
    const std::string commandsPath { base::join_path(detection.root, "compile_commands.json") };
    std::vector<std::pair<std::string, std::string>> notices;
    std::string failure;   // why mcpp produced no compile database, in its own words
    if (context.trusted) {
        const std::string home { platform::dirs::home_directory() };
        const std::vector<std::string> fallbacks { base::join_path(home, ".mcpp/bin/mcpp"), base::join_path(home, ".xlings/subos/current/bin/mcpp") };
        const std::optional<std::string> mcpp { context.mcppExecutable.empty() ? find_tool("mcpp", fallbacks) : std::optional<std::string> { context.mcppExecutable } };
        if (mcpp) {
            std::string version;
            if (auto emitted = emit_build_database(*mcpp, detection, context, version)) return std::move(*emitted);
            // This mcpp cannot describe the build without configuring it, which writes into the project.
            platform::SpawnOptions options;
            options.program = *mcpp;
            options.arguments = { "build", "--configure-only" };
            options.workDirectory = detection.root;
            auto result = platform::run(std::move(options), context.configureTimeout);
            if (!result) {
                base::log::warning("mcpp configure could not start: {}", result.error().message);
                failure = result.error().message;
            } else if (result->timedOut || result->exitCode != 0) {
                base::log::warning("mcpp build --configure-only failed ({}): {}", result->exitCode, base::trim(result->error));
                failure = result->timedOut ? std::string { "mcpp build --configure-only timed out" } : explanation(*result);
            } else {
                notices.emplace_back("producer-writes-project",
                                     std::format("{} has no emit build-database, which mcpp 2026.9.15.1 added; mcpp build --configure-only "
                                                 "wrote compile_commands.json and target/ into the project",
                                                 version.empty() ? std::string { "this mcpp" } : std::format("mcpp {}", version)));
            }
        } else {
            base::log::info("mcpp was not found; using an existing compile_commands.json if there is one");
        }
    }
    if (!platform::fs::is_regular_file(commandsPath)) {
        if (!context.trusted) return base::fail("mcpp-no-database", "the workspace is not trusted, so mcpp was not run");
        return base::fail("mcpp-no-database", failure.empty() ? std::string { "mcpp did not produce compile_commands.json" }
                                                              : std::format("mcpp could not describe the project: {}", failure));
    }
    auto commands = read_compile_commands(commandsPath);
    if (!commands) return std::unexpected { commands.error() };
    // A package's std, which an mcpp before 2026.9.15.1 builds outside the compile database (W8).
    for (auto& unit : mcpp_standard_units(*commands)) {
        const bool listed { std::ranges::any_of(*commands, [&](const CompileCommand& command) { return base::same_path(command.file, unit.file); }) };
        if (!listed) {
            base::log::info("the standard library module {} comes from mcpp's std build record", unit.file);
            commands->push_back(std::move(unit));
        }
    }
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
