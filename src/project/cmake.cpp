module lspmcpp.project.cmake;

import std;
import lspmcpp.base.error;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.base.log;
import lspmcpp.platform.fs;
import lspmcpp.platform.process;
import lspmcpp.spec.database;
import lspmcpp.toolchain.probe;
import lspmcpp.project.detect;
import lspmcpp.project.compdb;
import lspmcpp.project.scan;
import lspmcpp.project.infer;
import lspmcpp.project.provider;

namespace lspmcpp::project {

namespace {

// A P2977 document from CMake has no ide objects: probe each set's compiler and scan roles.
InferredDatabase enrich(spec::Database database, const ProviderContext& context) {
    InferredDatabase result;
    database.hasIde = true;
    if (database.profileVersion.empty()) database.profileVersion = std::string { spec::PROFILE_VERSION };
    if (!database.generator) database.generator = spec::Generator { "cmake", "build_database.json" };
    for (auto& set : database.sets) {
        set.hasIde = true;
        if (set.toolchain.empty() && !set.units.empty()) {
            const auto& first = set.units.front();
            std::string driver { first.arguments.front() };
            if (!base::is_absolute_path(driver)) driver = base::join_path(first.workDirectory, driver);
            const auto relevant = toolchain::probe_relevant_arguments(first.arguments);
            if (auto facts = context.prober(driver, relevant)) {
                set.toolchain = toolchain::toolchain_id(facts->toolchain);
                if (!result.facts.contains(set.toolchain)) {
                    database.toolchains.emplace_back(set.toolchain, facts->toolchain);
                    result.facts.emplace(set.toolchain, std::move(*facts));
                }
            } else {
                result.problems.push_back(std::format("cannot probe compiler {}", driver));
            }
        }
        for (auto& unit : set.units) {
            if (unit.role) continue;
            const ScanResult scanned { context.scanner(spec::absolute_source(unit)) };
            unit.role = role_of(scanned);
            if (unit.requiredModules.empty()) unit.requiredModules = required_names(scanned);
            if (unit.providedModules.empty()) {
                if (const std::string provided { provided_name(scanned) }; !provided.empty()) unit.providedModules.emplace_back(provided, std::string {});
            }
        }
    }
    result.database = std::move(database);
    return result;
}

base::Result<InferredDatabase> from_commands(std::string_view path, const Detection& detection, const ProviderContext& context) {
    auto commands = read_compile_commands(path);
    if (!commands) return std::unexpected { commands.error() };
    auto database = database_from_commands(*commands, base::file_name(detection.root), context.scanner, context.prober);
    database.database.generator = spec::Generator { "cmake", "compile_commands.json" };
    return database;
}

} // namespace

base::Result<InferredDatabase> load_cmake(const Detection& detection, std::string_view privateBuildDirectory, const ProviderContext& context) {
    if (!detection.buildDatabase.empty()) {
        auto database = spec::load_database(detection.buildDatabase);
        if (database) return enrich(std::move(*database), context);
        base::log::warning("ignoring {}: {}", detection.buildDatabase, database.error().message);
    }
    if (!detection.compileCommands.empty()) return from_commands(detection.compileCommands, detection, context);

    if (!context.trusted) return base::fail("untrusted-workspace", "the workspace is not trusted, so CMake was not configured");
    const auto cmake = find_tool("cmake", std::vector<std::string> {});
    if (!cmake) return base::fail("cmake-not-found", "CMakeLists.txt has no build directory and cmake is not on PATH");
    const std::string buildDirectory { privateBuildDirectory };
    (void)platform::fs::create_directories(buildDirectory);
    platform::SpawnOptions options;
    options.program = *cmake;
    options.arguments = { "-S", detection.root, "-B", buildDirectory, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DCMAKE_EXPORT_BUILD_DATABASE=ON" };
    if (find_tool("ninja", std::vector<std::string> {})) {
        options.arguments.emplace_back("-G");
        options.arguments.emplace_back("Ninja");
    }
    options.workDirectory = detection.root;
    auto result = platform::run(std::move(options), context.configureTimeout);
    if (!result) return std::unexpected { result.error() };
    if (result->timedOut || result->exitCode != 0) {
        return base::fail("cmake-configure-failed", std::format("cmake configure failed ({}): {}", result->exitCode,
                                                                base::trim(result->error.empty() ? result->output : result->error)));
    }
    const std::string commands { base::join_path(buildDirectory, "compile_commands.json") };
    if (!platform::fs::is_regular_file(commands)) return base::fail("cmake-no-database", "cmake did not write compile_commands.json");
    return from_commands(commands, detection, context);
}

} // namespace lspmcpp::project
