module lspmcpp.normalize.plan;

import std;
import nlohmann.json;
import lspmcpp.os;
import lspmcpp.base.error;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.platform.fs;
import lspmcpp.spec.database;
import lspmcpp.spec.kit;
import lspmcpp.spec.metadata;
import lspmcpp.toolchain.probe;
import lspmcpp.project.scan;
import lspmcpp.project.compdb;
import lspmcpp.normalize.gnu;
import lspmcpp.normalize.msvc;

namespace lspmcpp::normalize {

namespace {

struct Candidate {
    const spec::Set* set { nullptr };
    const spec::TranslationUnit* unit { nullptr };
    std::string source;
    spec::Role role { spec::Role::unknown };
    std::string provided;
    std::vector<std::string> required;
    std::vector<std::string> arguments;      // engine arguments without argv[0] and without the source
    std::string driver;
    bool usesKit { false };
    const toolchain::ToolchainFacts* facts { nullptr };
};

std::vector<std::size_t> context_sets(const spec::Database& database, std::string_view contextSet) {
    std::vector<std::size_t> indices;
    if (contextSet.empty()) {
        for (std::size_t i { 0 }; i < database.sets.size(); ++i) indices.push_back(i);
        return indices;
    }
    const auto selected = spec::find_set(database, contextSet);
    if (!selected) return context_sets(database, {});
    indices.push_back(*selected);
    for (const auto& visible : database.sets[*selected].visibleSets) {
        if (auto index = spec::find_set(database, visible); index && std::ranges::find(indices, *index) == indices.end()) {
            indices.push_back(*index);
        }
    }
    return indices;
}

const toolchain::ToolchainFacts* facts_for(const PlanInput& input, const spec::Set& set) {
    if (input.facts == nullptr || set.toolchain.empty()) return nullptr;
    const auto it = input.facts->find(set.toolchain);
    return it == input.facts->end() ? nullptr : &it->second;
}

bool usable(const toolchain::ToolchainFacts* facts) {
    return facts != nullptr && !facts->appleClang && facts->toolchain.family != spec::Family::other;
}

bool is_std_module(std::string_view name) { return name == "std" || name == "std.compat"; }

// The name a prime unit is written under: stable, file-system safe and unique per module.
std::string prime_file_name(std::size_t index, std::string_view module) {
    std::string name { std::format("{:04}-", index) };
    for (const char c : module) name += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_') ? c : '-';
    return name + ".cpp";
}

} // namespace

EnginePlan plan_engine(const PlanInput& input) {
    EnginePlan plan;
    plan.contextSet = input.contextSet;
    if (input.database == nullptr) return plan;
    const spec::Database& database { *input.database };
    const std::string clangDriver { base::join_path(input.engineDriverDirectory, ENGINE_CLANG_DRIVER) };

    // 0. The standard library's own module sources. A build that compiles them (CMake's
    //    import std writes std.ixx into the compile database) does not decide how the
    //    engine builds them: they are injected once from the manifest, step 5.
    std::set<std::string> standardSources;
    if (input.metadataReader) {
        for (const std::size_t setIndex : context_sets(database, input.contextSet)) {
            const toolchain::ToolchainFacts* facts { facts_for(input, database.sets[setIndex]) };
            if (facts == nullptr || !facts->toolchain.stdlib || facts->toolchain.stdlib->moduleMetadata.empty()) continue;
            for (const auto& entry : input.metadataReader(facts->toolchain.stdlib->moduleMetadata)) {
                if (is_std_module(entry.logicalName)) standardSources.insert(base::path_key(entry.source));
            }
        }
    }

    // 1. Candidates: every unit of the context, first occurrence of a source wins.
    std::vector<Candidate> candidates;
    std::set<std::string> seenSources;
    for (const std::size_t setIndex : context_sets(database, input.contextSet)) {
        const spec::Set& set { database.sets[setIndex] };
        const toolchain::ToolchainFacts* facts { facts_for(input, set) };
        for (const auto& unit : set.units) {
            Candidate candidate;
            candidate.set = &set;
            candidate.unit = &unit;
            candidate.source = spec::absolute_source(unit);
            if (standardSources.contains(base::path_key(candidate.source))) continue;
            if (!seenSources.insert(base::path_key(candidate.source)).second) continue;

            std::optional<project::ScanResult> scanned;
            auto scan = [&]() -> const project::ScanResult& {
                if (!scanned) scanned = input.scanner ? input.scanner(candidate.source) : project::ScanResult {};
                return *scanned;
            };
            candidate.role = unit.role.value_or(spec::Role::unknown);
            if (candidate.role == spec::Role::unknown) candidate.role = project::role_of(scan());
            if (!unit.providedModules.empty()) {
                candidate.provided = unit.providedModules.front().first;
            } else {
                candidate.provided = project::provided_name(scan());
            }
            candidate.required = unit.requiredModules;
            if (candidate.required.empty()) candidate.required = project::required_names(scan());

            const bool importable { spec::is_importable(candidate.role) };
            const auto syntax { lspmcpp::os::FAMILY == lspmcpp::os::Family::windows ? project::CommandSyntax::windows
                                                                                  : project::CommandSyntax::posix };
            const auto arguments = project::expand_response_files(unit.arguments, unit.workDirectory, syntax);
            candidate.facts = facts;
            if (usable(facts) && (facts->toolchain.family == spec::Family::gcc || facts->toolchain.family == spec::Family::clang)) {
                candidate.arguments = translate_gnu(GnuInput { arguments, candidate.source, unit.workDirectory, facts, importable });
                candidate.driver = facts->toolchain.family == spec::Family::gcc ? clangDriver : facts->toolchain.driver;
            } else if (usable(facts)) {
                candidate.arguments = translate_msvc(MsvcInput { arguments, candidate.source, unit.workDirectory, facts, importable });
                candidate.driver = clangDriver;
            } else if (input.kit != nullptr) {
                candidate.usesKit = true;
                candidate.arguments = kit_arguments(*input.kit, language_standard_of(arguments), input.macosSdk);
                for (auto& argument : semantic_subset(arguments)) {
                    if (!argument.starts_with("-std=")) candidate.arguments.push_back(std::move(argument));
                }
                if (importable) {
                    candidate.arguments.emplace_back("-x");
                    candidate.arguments.emplace_back("c++-module");
                }
                candidate.driver = clangDriver;
            } else {
                plan.issues.push_back(PlanIssue { "toolchain-not-found",
                    std::format("no usable compiler or semantic kit for {}", base::file_name(candidate.source)), candidate.source, {} });
                continue;
            }
            candidates.push_back(std::move(candidate));
        }
    }

    // 2. Providers in the context, and the standard library manifest the context uses.
    std::map<std::string, std::vector<std::size_t>, std::less<>> providers;
    for (std::size_t i { 0 }; i < candidates.size(); ++i) {
        if (!candidates[i].provided.empty() && spec::is_importable(candidates[i].role)) providers[candidates[i].provided].push_back(i);
    }
    std::string stdlibManifest;
    std::optional<std::size_t> templateIndex;
    for (std::size_t i { 0 }; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        const std::string manifest { candidate.usesKit ? input.kit->moduleMetadata
                                     : (candidate.facts && candidate.facts->toolchain.stdlib ? candidate.facts->toolchain.stdlib->moduleMetadata : std::string {}) };
        if (manifest.empty()) continue;
        if (stdlibManifest.empty()) {
            stdlibManifest = manifest;
            templateIndex = i;
        }
        if (!spec::is_importable(candidate.role) && base::same_path(manifest, stdlibManifest)) {
            templateIndex = i;
            break;
        }
    }
    std::vector<spec::ModuleEntry> stdEntries;
    if (!stdlibManifest.empty() && input.metadataReader) stdEntries = input.metadataReader(stdlibManifest);
    auto std_provides = [&](std::string_view name) {
        return std::ranges::any_of(stdEntries, [&](const spec::ModuleEntry& entry) { return entry.logicalName == name; });
    };

    // 3. Resolvability. Ambiguous later providers and importable units whose imports
    //    cannot resolve are left out; clangd 23.1 can hang on them (experiment E13).
    std::vector<bool> excluded(candidates.size(), false);
    for (auto& [name, indices] : providers) {
        if (indices.size() < 2) continue;
        for (std::size_t k { 1 }; k < indices.size(); ++k) excluded[indices[k]] = true;
        plan.issues.push_back(PlanIssue { "ambiguous-module", std::format("module {} has {} providers; using {}", name, indices.size(),
            base::file_name(candidates[indices.front()].source)), candidates[indices[1]].source, name });
    }
    bool anyStd { false };
    std::set<std::string> reported;
    for (std::size_t i { 0 }; i < candidates.size(); ++i) {
        for (const auto& name : candidates[i].required) {
            if (is_std_module(name)) anyStd = true;
            if (const auto failed = input.failedModules.find(name); failed != input.failedModules.end()) {
                excluded[i] = true;
                if (reported.insert(candidates[i].source + "\n" + name).second) {
                    plan.issues.push_back(PlanIssue { "module-build-failed", std::format("module {} could not be built: {}", name, failed->second),
                                                      candidates[i].source, name });
                }
                continue;
            }
            if (providers.contains(name) || std_provides(name)) continue;
            const auto setIndex = spec::find_set(database, candidates[i].set->name);
            bool resolved { false };
            if (setIndex && input.metadataReader) {
                const auto resolution = spec::resolve_module(database, *setIndex, name, input.metadataReader);
                resolved = resolution.from == spec::ResolvedFrom::module_metadata;
            }
            if (resolved) continue;
            // Any unit with an import that cannot resolve stays out of the engine database:
            // clangd 23.1 can stop answering for it (experiment E13), module unit or not.
            excluded[i] = true;
            if (reported.insert(candidates[i].source + "\n" + name).second) {
                plan.issues.push_back(PlanIssue { "unresolved-module", std::format("module {} cannot be resolved", name), candidates[i].source, name });
            }
        }
    }
    for (bool changed { true }; changed;) {
        changed = false;
        for (std::size_t i { 0 }; i < candidates.size(); ++i) {
            if (excluded[i]) continue;
            for (const auto& name : candidates[i].required) {
                const auto it = providers.find(name);
                if (it == providers.end()) continue;
                if (std::ranges::all_of(it->second, [&](std::size_t provider) { return excluded[provider]; })) {
                    excluded[i] = true;
                    changed = true;
                    break;
                }
            }
        }
    }

    // 4. Entries.
    for (std::size_t i { 0 }; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (excluded[i]) {
            plan.excludedFiles.push_back(candidate.source);
            continue;
        }
        EngineEntry entry { candidate.unit->workDirectory, candidate.source, {} };
        entry.arguments.push_back(candidate.driver);
        entry.arguments.insert(entry.arguments.end(), candidate.arguments.begin(), candidate.arguments.end());
        entry.arguments.push_back(candidate.source);
        if (spec::is_importable(candidate.role)) entry.provides = candidate.provided;
        entry.imports = candidate.required;
        plan.entries.push_back(std::move(entry));
    }

    // An importer's arguments for a module: a unit that imports it, else the provider's own without module mode.
    auto importer_of = [&](std::string_view module) -> std::optional<std::size_t> {
        std::optional<std::size_t> fallback;
        for (std::size_t i { 0 }; i < candidates.size(); ++i) {
            if (excluded[i] || std::ranges::find(candidates[i].required, module) == candidates[i].required.end()) continue;
            if (!spec::is_importable(candidates[i].role)) return i;
            if (!fallback) fallback = i;
        }
        return fallback;
    };
    auto without_module_mode = [](std::vector<std::string> arguments) {
        for (std::size_t k { 0 }; k + 1 < arguments.size();) {
            if (arguments[k] == "-x" && arguments[k + 1] == "c++-module") arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(k), arguments.begin() + static_cast<std::ptrdiff_t>(k + 2));
            else ++k;
        }
        return arguments;
    };
    auto add_module = [&](const std::string& name, std::vector<std::string> requires_, std::optional<std::size_t> importer,
                          const std::string& workDirectory) {
        PlannedModule module { name, std::move(requires_), {} };
        if (!input.primeDirectory.empty() && name.find(':') == std::string::npos && importer) {
            const auto& unit = candidates[*importer];
            module.primeFile = base::join_path(input.primeDirectory, prime_file_name(plan.modules.size(), name));
            EngineEntry entry { workDirectory, module.primeFile, {} };
            entry.arguments.push_back(unit.driver);
            for (auto& argument : without_module_mode(unit.arguments)) entry.arguments.push_back(std::move(argument));
            entry.arguments.push_back(module.primeFile);
            entry.imports.push_back(name);
            plan.entries.push_back(std::move(entry));
            plan.primeSources.emplace_back(module.primeFile, std::format("import {};\n", name));
        }
        plan.modules.push_back(std::move(module));
    };
    for (std::size_t i { 0 }; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (excluded[i] || candidate.provided.empty() || !spec::is_importable(candidate.role)) continue;
        const auto owners = providers.find(candidate.provided);
        if (owners == providers.end() || owners->second.front() != i) continue;   // the provider in use
        auto importer = importer_of(candidate.provided);
        if (!importer) importer = i;
        add_module(candidate.provided, candidate.required, importer, candidate.unit->workDirectory);
    }

    // 5. Standard library units, once for the context, with the arguments of a representative unit.
    if (anyStd && templateIndex && !stdEntries.empty()) {
        const auto& representative = candidates[*templateIndex];
        std::vector<std::string> base { representative.arguments };
        for (std::size_t k { 0 }; k + 1 < base.size();) {
            if (base[k] == "-x" && base[k + 1] == "c++-module") base.erase(base.begin() + static_cast<std::ptrdiff_t>(k), base.begin() + static_cast<std::ptrdiff_t>(k + 2));
            else ++k;
        }
        const bool msvcStl { representative.facts != nullptr && representative.facts->toolchain.stdlib
                             && representative.facts->toolchain.stdlib->name == "msvc-stl" };
        for (const auto& module : stdEntries) {
            if (seenSources.contains(base::path_key(module.source))) continue;
            // A producer that describes where std comes from (a dependency package's std.cppm) is taken at its word.
            if (providers.contains(module.logicalName)) continue;
            EngineEntry entry { representative.unit->workDirectory, module.source, {} };
            entry.arguments.push_back(representative.driver);
            entry.arguments.insert(entry.arguments.end(), base.begin(), base.end());
            for (const auto& directory : module.systemIncludeDirectories) {
                entry.arguments.emplace_back("-isystem");
                entry.arguments.push_back(directory);
            }
            entry.arguments.emplace_back("-Wno-reserved-module-identifier");
            // The MSVC STL includes its headers inside the module purview (usable plan E7).
            if (msvcStl) entry.arguments.emplace_back("-Wno-include-angled-in-module-purview");
            entry.arguments.emplace_back("-x");
            entry.arguments.emplace_back("c++-module");
            entry.arguments.push_back(module.source);
            std::vector<std::string> requires_;
            if (module.logicalName == "std.compat") requires_.emplace_back("std");
            entry.provides = module.logicalName;
            entry.imports = requires_;
            plan.entries.push_back(std::move(entry));
            ++plan.stdUnits;
            add_module(module.logicalName, std::move(requires_), templateIndex, representative.unit->workDirectory);
        }
    } else if (anyStd && stdEntries.empty()) {
        for (const auto& candidate : candidates) {
            if (std::ranges::any_of(candidate.required, is_std_module)) {
                plan.issues.push_back(PlanIssue { "unresolved-module", "the standard library module manifest was not found", candidate.source, "std" });
                break;
            }
        }
    }

    // 6. Module hints (usable plan W7). To find the unit that provides a module, clangd 23.1
    //    scans every file of the database, one after another, each time it prepares a file whose
    //    imports it has not looked up yet: seconds for a few hundred files, in every worker that
    //    starts at once. A producer's -fmodule-output=<path> and an importer's
    //    -fmodule-file=<name>=<path> name the unit instead, and clangd scans only that file to
    //    confirm it (ProjectModules.cpp, CompileCommandsProjectModules). It resolves every module
    //    a file reaches through that file's own command, so an entry names all of them. The
    //    paths are never written: clangd builds into its own cache.
    if (!input.moduleHintDirectory.empty()) {
        std::map<std::string_view, const std::vector<std::string>*, std::less<>> graph;
        for (const auto& entry : plan.entries) {
            if (!entry.provides.empty()) graph.emplace(entry.provides, &entry.imports);
        }
        auto hint_path = [&](std::string_view module) {
            std::string name { module };
            std::ranges::replace(name, ':', '-');   // a partition; ':' appears in no other module name
            return base::join_path(input.moduleHintDirectory, name + ".pcm");
        };
        for (auto& entry : plan.entries) {
            std::set<std::string_view, std::less<>> reached;
            std::vector<std::string_view> pending { entry.imports.begin(), entry.imports.end() };
            while (!pending.empty()) {
                const std::string_view name { pending.back() };
                pending.pop_back();
                const auto provider = graph.find(name);
                if (provider == graph.end() || !reached.insert(name).second) continue;
                for (const auto& required : *provider->second) pending.emplace_back(required);
            }
            for (const std::string_view name : reached) entry.moduleHints.push_back(std::format("-fmodule-file={}={}", name, hint_path(name)));
            if (!entry.provides.empty()) entry.moduleHints.push_back("-fmodule-output=" + hint_path(entry.provides));
        }
    }
    return plan;
}

nlohmann::json to_compile_commands(const EnginePlan& plan, bool moduleHints) {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& entry : plan.entries) {
        std::vector<std::string> arguments { entry.arguments };
        if (moduleHints && !entry.moduleHints.empty() && !arguments.empty()) {
            arguments.insert(arguments.end() - 1, entry.moduleHints.begin(), entry.moduleHints.end());
        }
        entries.push_back(nlohmann::json { { "directory", entry.directory }, { "file", entry.file }, { "arguments", std::move(arguments) } });
    }
    return entries;
}

base::Result<void> write_engine_database(std::string_view directory, const EnginePlan& plan) {
    if (auto created = platform::fs::create_directories(directory); !created) return created;
    return platform::fs::write_file_atomic(base::join_path(directory, "compile_commands.json"), to_compile_commands(plan).dump(1));
}

} // namespace lspmcpp::normalize
