module lspmcpp.spec.metadata;

import std;
import nlohmann.json;
import lspmcpp.base.error;
import lspmcpp.base.path;
import lspmcpp.platform.fs;

namespace lspmcpp::spec {

namespace {

std::string resolve(std::string_view directory, std::string_view path) {
    return base::join_path(directory, path);
}

} // namespace

base::Result<std::vector<ModuleEntry>> parse_module_metadata(const nlohmann::json& document, std::string_view manifestDirectory) {
    if (!document.is_object()) return base::fail("metadata-invalid", "module metadata is not a JSON object");
    const auto modules = document.find("modules");
    if (modules == document.end() || !modules->is_array()) {
        return base::fail("metadata-invalid", "module metadata has no \"modules\" array");
    }
    std::vector<ModuleEntry> entries;
    for (const auto& module : *modules) {
        if (!module.is_object()) continue;
        const auto name = module.find("logical-name");
        const auto source = module.find("source-path");
        if (name == module.end() || !name->is_string() || source == module.end() || !source->is_string()) {
            return base::fail("metadata-invalid", "a module entry lacks \"logical-name\" or \"source-path\"");
        }
        ModuleEntry entry;
        entry.logicalName = name->get<std::string>();
        entry.source = resolve(manifestDirectory, source->get<std::string>());
        entry.isStdLibrary = module.value("is-std-library", false);
        if (const auto local = module.find("local-arguments"); local != module.end() && local->is_object()) {
            for (const auto& directory : local->value("system-include-directories", nlohmann::json::array())) {
                if (directory.is_string()) entry.systemIncludeDirectories.push_back(resolve(manifestDirectory, directory.get<std::string>()));
            }
            for (const auto& definition : local->value("definitions", nlohmann::json::array())) {
                if (!definition.is_object() || !definition.contains("name")) continue;
                ModuleDefinition item { definition.value("name", std::string {}), std::nullopt };
                if (const auto value = definition.find("value"); value != definition.end() && value->is_string()) {
                    item.value = value->get<std::string>();
                }
                entry.definitions.push_back(std::move(item));
            }
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

base::Result<std::vector<ModuleEntry>> read_module_metadata(std::string_view manifestPath) {
    auto text = platform::fs::read_file(manifestPath);
    if (!text) return std::unexpected { text.error() };
    nlohmann::json document = nlohmann::json::parse(*text, nullptr, false);
    if (document.is_discarded()) return base::fail("metadata-invalid", std::format("{} is not valid JSON", manifestPath));
    return parse_module_metadata(document, base::parent_path(manifestPath));
}

} // namespace lspmcpp::spec
