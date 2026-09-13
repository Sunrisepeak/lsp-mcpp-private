module lspmcpp.project.model;

import std;
import lspmcpp.base.error;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.base.log;
import lspmcpp.platform.fs;
import lspmcpp.spec.database;
import lspmcpp.spec.kit;
import lspmcpp.toolchain.probe;
import lspmcpp.toolchain.discover;
import lspmcpp.project.detect;
import lspmcpp.project.compdb;
import lspmcpp.project.infer;
import lspmcpp.project.provider;
import lspmcpp.project.mcpp;
import lspmcpp.project.cmake;

namespace lspmcpp::project {

namespace {

std::string_view short_family(spec::Family family) {
    switch (family) {
    case spec::Family::gcc: return "gcc";
    case spec::Family::clang: return "clang";
    case spec::Family::msvc: return "msvc";
    case spec::Family::clang_cl: return "clang-cl";
    case spec::Family::other: return "compiler";
    }
    return "compiler";
}

bool usable_for_semantics(const toolchain::ToolchainFacts& facts) {
    if (facts.appleClang) return false;
    if (facts.toolchain.family != spec::Family::gcc && facts.toolchain.family != spec::Family::clang) return false;
    return facts.toolchain.stdlib && !facts.toolchain.stdlib->moduleMetadata.empty()
        && platform::fs::is_regular_file(facts.toolchain.stdlib->moduleMetadata);
}

void set_profile(ProjectModel& model, const spec::Kit* kit) {
    for (const auto& set : model.database.sets) {
        const auto it = model.facts.find(set.toolchain);
        if (it == model.facts.end() || it->second.appleClang) continue;
        const auto& toolchain = it->second.toolchain;
        model.profile.kind = "build-toolchain";
        model.profile.compiler = std::format("{} {}", short_family(toolchain.family), toolchain.version);
        model.profile.stdlib = toolchain.stdlib ? std::format("{} {}", toolchain.stdlib->name, toolchain.stdlib->version) : std::string {};
        if (model.profile.stdlib.ends_with(' ')) model.profile.stdlib.pop_back();
        model.profile.target = toolchain.target;
        return;
    }
    if (kit != nullptr) {
        model.usesKit = true;
        model.profile.kind = "semantic-kit";
        model.profile.compiler.clear();
        model.profile.stdlib = std::format("{} {}", kit->stdlibName, kit->stdlibVersion);
        model.profile.target = kit->target;
    } else {
        model.profile.kind = "semantic-kit";
        model.profile.stdlib = "none";
        model.issues.push_back(ModelIssue { "toolchain-not-found", "no usable compiler and no semantic kit" });
    }
}

} // namespace

std::string workspace_key(std::string_view root) {
    // FNV-1a over the normalized, case-folded path, plus the last component for humans.
    const std::string key { base::path_key(base::normalize_path(root)) };
    std::uint64_t hash { 1469598103934665603ull };
    for (const unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::string name;
    for (const char c : base::file_name(base::normalize_path(root))) {
        name += (base::is_identifier_char(c) || c == '-' || c == '.') ? c : '_';
    }
    return std::format("{}-{:016x}", name.empty() ? std::string { "root" } : name, hash);
}

ProjectModel load_project(std::string_view rootInput, const LoadOptions& options) {
    ProjectModel model;
    model.root = base::normalize_path(rootInput);
    const Detection detection { detect_project(model.root, options.configuredDatabase) };
    const Scanner scanner { options.scanner ? options.scanner : file_scanner() };
    const Prober prober = [&](std::string_view driver, std::span<const std::string> relevant) -> std::optional<toolchain::ToolchainFacts> {
        if (!options.trusted || !options.runner) return std::nullopt;
        auto facts = toolchain::probe_cached(driver, relevant, options.runner, options.probeCache);
        if (!facts) {
            base::log::info("probe of {} failed: {}", driver, facts.error().message);
            return std::nullopt;
        }
        return *facts;
    };
    ProviderContext context { options.trusted, options.runner, scanner, prober };

    std::optional<InferredDatabase> loaded;
    auto accept = [&](base::Result<InferredDatabase> result, SourceKind kind) {
        if (result) {
            loaded = std::move(*result);
            model.source = kind;
            for (const auto& problem : loaded->problems) model.issues.push_back(ModelIssue { "toolchain-not-found", problem });
        } else {
            model.issues.push_back(ModelIssue { result.error().code, result.error().message });
        }
    };

    switch (detection.kind) {
    case SourceKind::build_database: {
        auto database = spec::load_database(detection.buildDatabase);
        if (database) {
            InferredDatabase result;
            result.database = std::move(*database);
            for (const auto& [id, toolchain] : result.database.toolchains) {
                if (auto facts = prober(toolchain.driver, std::vector<std::string> {})) result.facts.emplace(id, std::move(*facts));
            }
            loaded = std::move(result);
            model.source = SourceKind::build_database;
        } else {
            model.issues.push_back(ModelIssue { database.error().code, database.error().message });
        }
        model.watch = { detection.buildDatabase };
        break;
    }
    case SourceKind::mcpp:
        accept(load_mcpp(detection, context), SourceKind::mcpp);
        model.watch = { "mcpp.toml", "mcpp.lock" };
        break;
    case SourceKind::cmake: {
        const std::string privateBuild { base::join_path(options.cacheDirectory, "cmake") };
        accept(load_cmake(detection, privateBuild, context), SourceKind::cmake);
        model.watch = { "**/CMakeLists.txt", "CMakePresets.json", "**/*.cmake" };
        if (!detection.compileCommands.empty()) model.watch.push_back(detection.compileCommands);
        break;
    }
    case SourceKind::compile_commands: {
        auto commands = read_compile_commands(detection.compileCommands);
        if (commands) {
            loaded = database_from_commands(*commands, base::file_name(model.root), scanner, prober);
            model.source = SourceKind::compile_commands;
            for (const auto& problem : loaded->problems) model.issues.push_back(ModelIssue { "toolchain-not-found", problem });
        } else {
            model.issues.push_back(ModelIssue { commands.error().code, commands.error().message });
        }
        model.watch = { detection.compileCommands };
        break;
    }
    case SourceKind::inferred: break;
    }

    if (!loaded || loaded->database.sets.empty() || std::ranges::all_of(loaded->database.sets, [](const spec::Set& set) { return set.units.empty(); })) {
        InferOptions infer;
        if (options.trusted && options.runner) {
            if (!options.compilerOverride.empty()) {
                if (auto facts = prober(options.compilerOverride, std::vector<std::string> {}); facts && usable_for_semantics(*facts)) {
                    infer.facts = std::move(*facts);
                } else {
                    model.issues.push_back(ModelIssue { "toolchain-not-found", std::format("the configured compiler {} cannot provide module semantics", options.compilerOverride) });
                }
            }
            if (!infer.facts && options.discoverCompilers) {
                for (const auto& candidate : toolchain::discover_compilers(options.runner)) {
                    if (candidate.family != spec::Family::gcc && candidate.family != spec::Family::clang) continue;
                    if (auto facts = prober(candidate.driver, std::vector<std::string> {}); facts && usable_for_semantics(*facts)) {
                        infer.facts = std::move(*facts);
                        break;
                    }
                }
            }
        }
        loaded = infer_database(model.root, infer, scanner);
        if (model.source != SourceKind::inferred && detection.kind != SourceKind::inferred) {
            model.issues.push_back(ModelIssue { "model-fallback", std::format("{} data was not available; sources are scanned instead", to_string(detection.kind)) });
        }
        model.source = SourceKind::inferred;
        model.watch.insert(model.watch.end(), { "**/*.cppm", "**/*.ccm", "**/*.cxxm", "**/*.ixx", "**/*.mpp", "**/*.cpp", "**/*.cc", "**/*.cxx" });
    }

    model.database = std::move(loaded->database);
    model.facts = std::move(loaded->facts);
    model.level = model.source == SourceKind::build_database ? spec::conformance_level(model.database) : 2;
    set_profile(model, options.kit);
    if (options.probeCache != nullptr) options.probeCache->save();
    return model;
}

} // namespace lspmcpp::project
