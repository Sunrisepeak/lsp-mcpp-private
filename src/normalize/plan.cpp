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

} // namespace

EnginePlan plan_engine(const PlanInput& input) {
    EnginePlan plan;
    plan.contextSet = input.contextSet;
    if (input.database == nullptr) return plan;
    const spec::Database& database { *input.database };
    const std::string clangDriver { base::join_path(input.engineDriverDirectory, ENGINE_CLANG_DRIVER) };
    const std::string clangClDriver { base::join_path(input.engineDriverDirectory, ENGINE_CLANG_CL_DRIVER) };

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
                candidate.arguments = translate_msvc(MsvcInput { arguments, candidate.source, unit.workDirectory, facts, importable, {}, {} });
                candidate.driver = clangClDriver;
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
        plan.entries.push_back(std::move(entry));
    }

    // 5. Standard library units, once for the context, with the arguments of a representative unit.
    if (anyStd && templateIndex && !stdEntries.empty()) {
        const auto& representative = candidates[*templateIndex];
        std::vector<std::string> base { representative.arguments };
        const bool clMode { !base.empty() && base.front() == "--driver-mode=cl" };
        std::erase(base, std::string { "/clang:-xc++-module" });
        for (std::size_t k { 0 }; k + 1 < base.size();) {
            if (base[k] == "-x" && base[k + 1] == "c++-module") base.erase(base.begin() + static_cast<std::ptrdiff_t>(k), base.begin() + static_cast<std::ptrdiff_t>(k + 2));
            else ++k;
        }
        for (const auto& module : stdEntries) {
            if (seenSources.contains(base::path_key(module.source))) continue;
            EngineEntry entry { representative.unit->workDirectory, module.source, {} };
            entry.arguments.push_back(representative.driver);
            entry.arguments.insert(entry.arguments.end(), base.begin(), base.end());
            for (const auto& directory : module.systemIncludeDirectories) {
                if (clMode) {
                    entry.arguments.push_back("/clang:-isystem" + directory);
                } else {
                    entry.arguments.emplace_back("-isystem");
                    entry.arguments.push_back(directory);
                }
            }
            if (clMode) {
                entry.arguments.emplace_back("/clang:-Wno-reserved-module-identifier");
                entry.arguments.emplace_back("/clang:-xc++-module");
            } else {
                entry.arguments.emplace_back("-Wno-reserved-module-identifier");
                entry.arguments.emplace_back("-x");
                entry.arguments.emplace_back("c++-module");
            }
            entry.arguments.push_back(module.source);
            plan.entries.push_back(std::move(entry));
            ++plan.stdUnits;
        }
    } else if (anyStd && stdEntries.empty()) {
        for (const auto& candidate : candidates) {
            if (std::ranges::any_of(candidate.required, is_std_module)) {
                plan.issues.push_back(PlanIssue { "unresolved-module", "the standard library module manifest was not found", candidate.source, "std" });
                break;
            }
        }
    }
    return plan;
}

nlohmann::json to_compile_commands(const EnginePlan& plan) {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& entry : plan.entries) {
        entries.push_back(nlohmann::json { { "directory", entry.directory }, { "file", entry.file }, { "arguments", entry.arguments } });
    }
    return entries;
}

base::Result<void> write_engine_database(std::string_view directory, const EnginePlan& plan) {
    if (auto created = platform::fs::create_directories(directory); !created) return created;
    return platform::fs::write_file_atomic(base::join_path(directory, "compile_commands.json"), to_compile_commands(plan).dump(1));
}

} // namespace lspmcpp::normalize
