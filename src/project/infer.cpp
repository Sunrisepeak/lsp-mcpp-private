module lspmcpp.project.infer;

import std;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.platform.fs;
import lspmcpp.spec.database;
import lspmcpp.toolchain.probe;
import lspmcpp.project.compdb;
import lspmcpp.project.scan;

namespace lspmcpp::project {

namespace {

constexpr std::array<std::string_view, 16> SOURCE_EXTENSIONS {
    ".cpp", ".cc", ".cxx", ".c++", ".cppm", ".ccm", ".cxxm", ".c++m", ".ixx", ".mpp", ".mxx", ".cp",
    ".CPP", ".CC", ".CXX", ".C",
};
constexpr std::array<std::string_view, 7> SKIPPED_DIRECTORIES { "target", "build", "node_modules", "out", "_build", "cmake-build-debug", "cmake-build-release" };

void fill_modules(spec::TranslationUnit& unit, const ScanResult& scanned) {
    unit.role = role_of(scanned);
    if (const std::string provided { provided_name(scanned) }; !provided.empty()) unit.providedModules.emplace_back(provided, std::string {});
    unit.requiredModules = required_names(scanned);
    unit.isPrivate = unit.role == spec::Role::non_module || unit.role == spec::Role::module_implementation;
}

} // namespace

Scanner file_scanner() {
    return [](std::string_view path) {
        auto text = platform::fs::read_file(path);
        return text ? scan_source(*text) : ScanResult {};
    };
}

InferredDatabase database_from_commands(std::span<const CompileCommand> commands, std::string_view familyName,
                                        const Scanner& scanner, const Prober& prober) {
    InferredDatabase result;
    result.database.hasIde = true;
    result.database.generator = spec::Generator { "lsp-mcpp", "compile-commands" };
    std::map<std::string, std::size_t, std::less<>> setByToolchain;
    std::map<std::string, std::string, std::less<>> toolchainByProbeKey;

    for (const auto& command : commands) {
        if (command.arguments.empty()) continue;
        const std::string_view language { base::extension(command.file) };
        if (base::iequals_ascii(language, ".c") || base::iequals_ascii(language, ".s") || base::iequals_ascii(language, ".asm")) continue;
        std::string driver { command.arguments.front() };
        if (!base::is_absolute_path(driver) && driver.find('/') != std::string::npos) driver = base::join_path(command.directory, driver);
        const auto relevant = toolchain::probe_relevant_arguments(command.arguments);
        std::string probeKey { driver };
        for (const auto& argument : relevant) probeKey += "|" + argument;

        std::string toolchainId;
        if (const auto known = toolchainByProbeKey.find(probeKey); known != toolchainByProbeKey.end()) {
            toolchainId = known->second;
        } else {
            if (auto facts = prober(driver, relevant)) {
                toolchainId = toolchain::toolchain_id(facts->toolchain);
                if (!result.facts.contains(toolchainId)) {
                    result.database.toolchains.emplace_back(toolchainId, facts->toolchain);
                    result.facts.emplace(toolchainId, std::move(*facts));
                }
            } else {
                result.problems.push_back(std::format("cannot probe compiler {}", driver));
            }
            toolchainByProbeKey.emplace(probeKey, toolchainId);
        }

        auto setIt = setByToolchain.find(toolchainId);
        if (setIt == setByToolchain.end()) {
            spec::Set set;
            set.name = toolchainId.empty() ? std::string { familyName } : std::format("{}@{}", familyName, toolchainId);
            set.familyName = std::string { familyName };
            set.hasIde = true;
            set.toolchain = toolchainId;
            set.kind = "other";
            result.database.sets.push_back(std::move(set));
            setIt = setByToolchain.emplace(toolchainId, result.database.sets.size() - 1).first;
        }
        spec::TranslationUnit unit;
        unit.source = command.file;
        unit.workDirectory = command.directory;
        unit.arguments = command.arguments;
        unit.object = command.output;
        fill_modules(unit, scanner(command.file));
        result.database.sets[setIt->second].units.push_back(std::move(unit));
    }
    // Every set sees every other: a compile_commands.json has no visibility information.
    for (auto& set : result.database.sets) {
        for (const auto& other : result.database.sets) {
            if (other.name != set.name) set.visibleSets.push_back(other.name);
        }
    }
    return result;
}

InferredDatabase infer_database(std::string_view rootInput, const InferOptions& options, const Scanner& scanner) {
    InferredDatabase result;
    const std::string root { base::normalize_path(rootInput) };
    result.database.hasIde = true;
    result.database.generator = spec::Generator { "lsp-mcpp", "inferred" };

    spec::Set set;
    set.name = "inferred";
    set.familyName = std::string { base::file_name(root) };
    set.hasIde = true;
    set.kind = "other";
    std::string driver { options.engineDriver };
    if (options.facts) {
        set.toolchain = toolchain::toolchain_id(options.facts->toolchain);
        driver = options.facts->toolchain.driver;
        result.database.toolchains.emplace_back(set.toolchain, options.facts->toolchain);
        result.facts.emplace(set.toolchain, *options.facts);
    }
    std::vector<std::string> baseline { "-std=" + options.languageStandard };
    for (std::string_view include : { "include", "src" }) {
        if (const std::string directory { base::join_path(root, include) }; platform::fs::is_directory(directory)) baseline.push_back("-I" + directory);
    }
    set.baselineArguments = baseline;

    for (const auto& file : platform::fs::list_files(root, SOURCE_EXTENSIONS, SKIPPED_DIRECTORIES)) {
        spec::TranslationUnit unit;
        unit.source = file;
        unit.workDirectory = root;
        unit.arguments.push_back(driver);
        unit.arguments.insert(unit.arguments.end(), baseline.begin(), baseline.end());
        unit.arguments.push_back("-c");
        unit.arguments.push_back(file);
        fill_modules(unit, scanner(file));
        set.units.push_back(std::move(unit));
    }
    result.database.sets.push_back(std::move(set));
    return result;
}

} // namespace lspmcpp::project
