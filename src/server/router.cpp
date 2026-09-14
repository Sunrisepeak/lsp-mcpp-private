module lspmcpp.server.router;

import std;
import nlohmann.json;
import lspmcpp.base.text;
import lspmcpp.index.modules;
import lspmcpp.lsp.jsonrpc;
import lspmcpp.lsp.protocol;

namespace lspmcpp::server {

using Json = nlohmann::json;

namespace {

std::optional<base::Position> position_of(const Json& params) {
    const Json* position { lsp::find(params, "position") };
    if (position == nullptr) return std::nullopt;
    const auto line = lsp::int_at(*position, "line");
    const auto character = lsp::int_at(*position, "character");
    if (!line || !character) return std::nullopt;
    return base::Position { static_cast<int>(*line), static_cast<int>(*character) };
}

} // namespace

RouteDecision route_request(std::string_view method, const Json& params, const index::ModuleIndex& index,
                            std::string_view path, std::string_view text) {
    RouteDecision decision;
    if (method == lsp::method::TEXT_DOCUMENT_DOCUMENT_SYMBOL) {
        decision.merge = Merge::document_symbols;
        return decision;
    }
    if (method == lsp::method::WORKSPACE_SYMBOL) {
        decision.merge = Merge::workspace_symbols;
        return decision;
    }
    if (path.empty()) return decision;
    const auto position = position_of(params);
    if (!position) return decision;
    if (method == lsp::method::TEXT_DOCUMENT_DEFINITION || method == lsp::method::TEXT_DOCUMENT_DECLARATION) {
        if (Json result = index.definition(path, *position); !result.is_null()) {
            decision.route = Route::local;
            decision.localResult = std::move(result);
        }
    } else if (method == lsp::method::TEXT_DOCUMENT_HOVER) {
        if (Json result = index.hover(path, *position); !result.is_null()) {
            decision.route = Route::local;
            decision.localResult = std::move(result);
        }
    } else if (method == lsp::method::TEXT_DOCUMENT_COMPLETION) {
        if (Json result = index.completion(path, text, *position); !result.is_null()) {
            decision.route = Route::local;
            decision.localResult = std::move(result);
        }
    }
    return decision;
}

Json merge_document_symbols(const Json& engineResult, const Json& moduleSymbols) {
    if (!moduleSymbols.is_array() || moduleSymbols.empty()) return engineResult.is_null() ? Json::array() : engineResult;
    if (!engineResult.is_array() || engineResult.empty()) return moduleSymbols;
    // SymbolInformation[] (flat, with "location") cannot hold DocumentSymbol entries.
    if (engineResult.front().contains("location")) return engineResult;
    Json merged = moduleSymbols;
    for (const auto& symbol : engineResult) merged.push_back(symbol);
    return merged;
}

Json merge_workspace_symbols(const Json& engineResult, const Json& moduleSymbols) {
    Json merged = moduleSymbols.is_array() ? moduleSymbols : Json::array();
    if (engineResult.is_array()) {
        for (const auto& symbol : engineResult) merged.push_back(symbol);
    }
    return merged;
}

Json merge_diagnostics(const Json& engineDiagnostics, const Json& moduleDiagnostics, std::string_view engineSourceLabel) {
    Json merged = Json::array();
    std::set<std::string> moduleRanges;
    if (moduleDiagnostics.is_array()) {
        for (const auto& diagnostic : moduleDiagnostics) {
            moduleRanges.insert(lsp::dump(diagnostic.value("range", Json {})));
            merged.push_back(diagnostic);
        }
    }
    if (engineDiagnostics.is_array()) {
        for (auto diagnostic : engineDiagnostics) {
            // The index already explains an unresolved import at the same place.
            if (moduleRanges.contains(lsp::dump(diagnostic.value("range", Json {})))) continue;
            if (!engineSourceLabel.empty()) {
                const std::string source { diagnostic.value("source", std::string { "clangd" }) };
                diagnostic["source"] = std::format("{} · {}", source, engineSourceLabel);
            }
            merged.push_back(std::move(diagnostic));
        }
    }
    return merged;
}

Json merge_capabilities(const Json& engineCapabilities) {
    Json capabilities = engineCapabilities.is_object() ? engineCapabilities : Json::object();
    if (!capabilities.contains("definitionProvider")) capabilities["definitionProvider"] = true;
    if (!capabilities.contains("hoverProvider")) capabilities["hoverProvider"] = true;
    if (!capabilities.contains("documentSymbolProvider")) capabilities["documentSymbolProvider"] = true;
    if (!capabilities.contains("workspaceSymbolProvider")) capabilities["workspaceSymbolProvider"] = true;
    if (!capabilities.contains("completionProvider")) {
        capabilities["completionProvider"] = Json { { "triggerCharacters", Json::array({ ".", ":" }) } };
    }
    if (!capabilities.contains("experimental") || !capabilities["experimental"].is_object()) capabilities["experimental"] = Json::object();
    capabilities["experimental"]["cxxModules"] = Json { { "version", 1 }, { "databaseSpec", ">=0.2 <1" } };
    // usable plan W9.1: without this, a client has no reason to ever send
    // workspace/didChangeWorkspaceFolders, and the session would never learn of an added or
    // removed root.
    if (!capabilities.contains("workspace") || !capabilities["workspace"].is_object()) capabilities["workspace"] = Json::object();
    capabilities["workspace"]["workspaceFolders"] = Json { { "supported", true }, { "changeNotifications", true } };
    return capabilities;
}

bool client_supports(const Json& clientCapabilities, std::string_view feature) {
    const Json* modules { lsp::find_path(clientCapabilities, { "experimental", "cxxModules" }) };
    if (modules == nullptr || !modules->is_object()) return false;
    const auto value = modules->find(feature);
    return value != modules->end() && value->is_boolean() && value->get<bool>();
}

} // namespace lspmcpp::server
