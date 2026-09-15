module mcppls.server.cli;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.version;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.platform.process;
import mcppls.spec.database;
import mcppls.spec.kit;
import mcppls.spec.metadata;
import mcppls.toolchain.probe;
import mcppls.project.scan;
import mcppls.project.detect;
import mcppls.project.infer;
import mcppls.project.model;
import mcppls.normalize.plan;
import mcppls.index.modules;
import mcppls.server.payload;
import mcppls.server.session;

namespace mcppls::server {

namespace {

using Json = nlohmann::json;
using namespace mcpplibs;

std::string absolute(std::string_view path) {
    if (path.empty() || base::is_absolute_path(path)) return base::normalize_path(path);
    return base::join_path(platform::fs::current_directory(), path);
}

struct Loaded {
    PayloadPaths payload;
    std::optional<spec::Kit> kit;
    project::ProjectModel model;
    normalize::EnginePlan plan;
};

// The root for a file: the nearest directory with a build description, else the file's directory.
std::string root_for(std::string_view file) {
    std::string directory { base::parent_path(file) };
    while (true) {
        for (std::string_view marker : { "mcpp.toml", "CMakeLists.txt", "compile_commands.json" }) {
            if (platform::fs::is_regular_file(base::join_path(directory, marker))) return directory;
        }
        const std::string parent { base::parent_path(directory) };
        if (parent == directory) return base::parent_path(file);
        directory = parent;
    }
}

Loaded load(std::string_view root, const cmdline::ParsedArgs& args, bool trusted) {
    Loaded loaded;
    loaded.payload = resolve_payload(PayloadRequest { args.value("payload").value_or(""), args.value("clangd").value_or(""), args.value("kit").value_or("") });
    if (!loaded.payload.kit.empty()) {
        if (auto kit = spec::load_kit(loaded.payload.kit)) loaded.kit = std::move(*kit);
        else base::log::warning("semantic kit unusable: {}", kit.error().message);
    }
    toolchain::ProbeCache cache { base::join_path(platform::dirs::cache_directory(), "toolchains/probe.json") };
    project::LoadOptions options;
    options.trusted = trusted;
    options.cacheDirectory = base::join_path(platform::dirs::cache_directory(), base::join_path("workspaces", project::workspace_key(root)));
    options.kit = loaded.kit ? &*loaded.kit : nullptr;
    options.runner = toolchain::process_runner(std::chrono::seconds { 20 });
    options.probeCache = &cache;
    options.discoverCompilers = !args.is_flag_set("no-discover");
    options.mcppExecutable = args.value("mcpp").value_or("");
    options.configuredDatabase = args.value("database").value_or("");
    loaded.model = project::load_project(root, options);

    normalize::PlanInput input;
    input.database = &loaded.model.database;
    input.facts = &loaded.model.facts;
    input.kit = options.kit;
    input.engineDriverDirectory = loaded.payload.clangd.empty() ? std::string {} : base::parent_path(loaded.payload.clangd);
    input.macosSdk = options.kit && spec::requires_macos_sdk(*options.kit) ? macos_sdk_path() : std::string {};
    input.scanner = project::file_scanner();
    input.metadataReader = spec::caching_metadata_reader();
    loaded.plan = normalize::plan_engine(input);
    return loaded;
}

int command_model(const cmdline::ParsedArgs& args) {
    const std::string root { absolute(args.value("root").value_or(platform::fs::current_directory())) };
    const Loaded loaded { load(root, args, !args.is_flag_set("untrusted")) };
    const std::string format { args.value("export").value_or("s1") };
    if (format == "compile-commands") {
        // S1 section 12: the export keeps the default context's command per file and loses the rest.
        std::println(std::cerr, "note: compile_commands.json has no module graph, toolchain or role information, and one set per file");
        std::println("{}", spec::to_compile_commands(loaded.model.database).dump(2));
    } else if (format == "engine") {
        std::println("{}", normalize::to_compile_commands(loaded.plan).dump(2));
    } else if (format == "s1") {
        std::println("{}", spec::to_json(loaded.model.database).dump(2));
    } else {
        std::println(std::cerr, "unknown export format {}; use s1, compile-commands or engine", format);
        return 2;
    }
    return 0;
}

int command_check(const cmdline::ParsedArgs& args) {
    const std::string file { absolute(args.value("file").value_or("")) };
    if (file.empty() || !platform::fs::is_regular_file(file)) {
        std::println(std::cerr, "check: no such file: {}", file);
        return 2;
    }
    const std::string root { args.value("root") ? absolute(*args.value("root")) : root_for(file) };
    const Loaded loaded { load(root, args, !args.is_flag_set("untrusted")) };
    const auto& model = loaded.model;
    std::println("root      {}", root);
    std::println("source    {} (level {})", project::to_string(model.source), model.level);
    std::println("profile   {} {} {} {}", model.profile.kind, model.profile.compiler, model.profile.stdlib, model.profile.target);
    std::println("engine    clangd {} {}", loaded.payload.clangdVersion, loaded.payload.clangd);
    std::println("database  {} entries, {} standard library units, {} left out", loaded.plan.entries.size(), loaded.plan.stdUnits, loaded.plan.excludedFiles.size());
    for (const auto& issue : model.issues) std::println("issue     [{}] {}", issue.code, issue.message);
    for (const auto& issue : loaded.plan.issues) std::println("issue     [{}] {} ({})", issue.code, issue.message, issue.file);

    index::ModuleIndex index;
    for (const auto& set : model.database.sets) {
        for (const auto& unit : set.units) {
            const std::string path { spec::absolute_source(unit) };
            if (auto text = platform::fs::read_file(path)) index.update(path, *text);
        }
    }
    if (auto text = platform::fs::read_file(file)) index.update(file, *text);
    std::vector<std::pair<std::string, std::string>> manifests;
    for (auto& manifest : project::module_manifests(model, loaded.kit ? &*loaded.kit : nullptr)) manifests.emplace_back(manifest.path, manifest.origin);
    index.set_external(index::external_modules(manifests, spec::caching_metadata_reader()));
    for (const auto& diagnostic : index.diagnostics(file)) {
        std::println("module    {}:{}: {}", base::file_name(file), diagnostic["range"]["start"]["line"].get<int>() + 1, diagnostic.value("message", std::string {}));
    }
    if (loaded.payload.clangd.empty() || !platform::fs::is_regular_file(loaded.payload.clangd)) {
        std::println("clangd    not found; skipped the semantic check");
        return loaded.plan.issues.empty() ? 0 : 1;
    }
    const std::string directory { base::join_path(platform::dirs::temp_directory(), std::format("mcppls-check-{}", project::workspace_key(root))) };
    if (auto written = normalize::write_engine_database(directory, loaded.plan); !written) {
        std::println(std::cerr, "check: {}", written.error().message);
        return 2;
    }
    platform::SpawnOptions options;
    options.program = loaded.payload.clangd;
    options.arguments = { "--check=" + file, "--experimental-modules-support", "--compile-commands-dir=" + directory, "--log=error" };
    options.workDirectory = root;
    auto result = platform::run(std::move(options), std::chrono::minutes { 5 });
    if (!result) {
        std::println(std::cerr, "check: {}", result.error().message);
        return 2;
    }
    std::print("{}", result->error);
    std::println("clangd    exit {}{}", result->exitCode, result->timedOut ? " (timed out)" : "");
    return result->exitCode == 0 && !result->timedOut ? 0 : 1;
}

} // namespace

int run_cli(int argc, char* argv[]) {
    int status { 0 };
    bool handled { false };
    auto serve = [&](const cmdline::ParsedArgs& args) {
        handled = true;
        if (auto level = args.value("log-level")) {
            if (auto parsed = base::log::parse_level(*level)) base::log::set_level(*parsed);
        }
        SessionOptions options;
        options.payloadDirectory = args.value("payload").value_or("");
        options.clangd = args.value("clangd").value_or("");
        options.kit = args.value("kit").value_or("");
        options.mcpp = args.value("mcpp").value_or("");
        options.database = args.value("database").value_or("");
        options.trusted = !args.is_flag_set("untrusted");
        options.discoverCompilers = !args.is_flag_set("no-discover");
        options.verboseEngineLog = args.value("log-level").value_or("") == "debug";
        if (auto timeout = args.value("request-timeout")) {
            try {
                options.requestTimeout = std::chrono::seconds { std::stoi(*timeout) };
            } catch (...) {
            }
        }
        status = run_session(options);
    };

    // Built statement by statement: a fluent chain nests a subcommand under the
    // previous one once an option has been added to it.
    cmdline::App app { "mcppls" };
    (void)app.version(std::string { base::VERSION });
    (void)app.description("Compiler-agnostic C++ modules language server");
    (void)app.option("payload").takes_value().global(true).help("Payload directory with clangd and the semantic kit");
    (void)app.option("clangd").takes_value().global(true).help("clangd executable (overrides the payload)");
    (void)app.option("kit").takes_value().global(true).help("Semantic kit directory (overrides the payload)");
    (void)app.option("mcpp").takes_value().global(true).help("The mcpp executable for mcpp projects (default: found on PATH)");
    (void)app.option("database").takes_value().global(true).help("A workspace's own S1 build database, relative to its root");
    (void)app.option("untrusted").global(true).help("Do not run build tools or compilers");
    (void)app.option("no-discover").global(true).help("Do not look for compilers; loose sources use the semantic kit");
    (void)app.option("log-level").takes_value().global(true).help("debug | info | warning | error");
    (void)app.option("request-timeout").takes_value().global(true).help("Seconds before an engine request is answered without it");
    // Language clients pass these by convention; this server always speaks over its standard streams.
    (void)app.option("stdio").global(true).help("Accepted for language clients; standard input and output are always used");
    (void)app.option("clientProcessId").takes_value().global(true).help("Accepted for language clients; not used");
    (void)app.action(serve);

    cmdline::App serveCommand { "serve" };
    (void)serveCommand.description("Serve the Language Server Protocol on standard input and output (the default)");
    (void)serveCommand.action(serve);
    (void)app.subcommand(std::move(serveCommand));

    cmdline::App checkCommand { "check" };
    (void)checkCommand.description("Load the project of a file, print the model, plan and diagnostics, and run clangd --check");
    (void)checkCommand.option("root").takes_value().help("Project root (default: nearest build description)");
    (void)checkCommand.arg("file").required();
    (void)checkCommand.action([&](const cmdline::ParsedArgs& args) { handled = true; status = command_check(args); });
    (void)app.subcommand(std::move(checkCommand));

    cmdline::App modelCommand { "model" };
    (void)modelCommand.description("Print the project model as S1, as compile_commands.json, or the engine database");
    (void)modelCommand.option("root").takes_value().help("Project root (default: the current directory)");
    (void)modelCommand.option("export").takes_value().help("s1 | compile-commands | engine");
    (void)modelCommand.action([&](const cmdline::ParsedArgs& args) { handled = true; status = command_model(args); });
    (void)app.subcommand(std::move(modelCommand));

    cmdline::App versionCommand { "version" };
    (void)versionCommand.description("Print the version");
    (void)versionCommand.action([&](const cmdline::ParsedArgs&) {
        handled = true;
        std::println("mcppls {} ({}; S1 {}; clangd {})", base::VERSION, mcppls::os::VSCODE_TARGET, spec::PROFILE_VERSION, base::CLANGD_VERSION);
    });
    (void)app.subcommand(std::move(versionCommand));
    const int parsed { app.run(argc, argv) };
    if (parsed != 0) return parsed;
    return handled ? status : 0;
}

} // namespace mcppls::server
