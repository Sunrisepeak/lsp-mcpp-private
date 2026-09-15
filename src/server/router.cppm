// Request routing and result merging (design section 12.6): the module index
// answers on module names, the engine answers everything else, and outlines,
// workspace symbols and diagnostics combine both.
export module mcppls.server.router;

import std;
import nlohmann.json;
import mcppls.index.modules;

export namespace mcppls::server {

enum class Route { local, engine };
enum class Merge { none, document_symbols, workspace_symbols };

struct RouteDecision {
    Route route { Route::engine };
    Merge merge { Merge::none };
    nlohmann::json localResult;
};

// `path` is the file of params.textDocument, empty when there is none; `text`
// is its current buffer.
RouteDecision route_request(std::string_view method, const nlohmann::json& params, const index::ModuleIndex& index,
                            std::string_view path, std::string_view text);

nlohmann::json merge_document_symbols(const nlohmann::json& engineResult, const nlohmann::json& moduleSymbols);
nlohmann::json merge_workspace_symbols(const nlohmann::json& engineResult, const nlohmann::json& moduleSymbols);
nlohmann::json merge_diagnostics(const nlohmann::json& engineDiagnostics, const nlohmann::json& moduleDiagnostics,
                                 std::string_view engineSourceLabel);
// The engine's capabilities with this server's additions.
nlohmann::json merge_capabilities(const nlohmann::json& engineCapabilities);
// Whether the client advertised experimental.cxxModules.<feature>.
bool client_supports(const nlohmann::json& clientCapabilities, std::string_view feature);

} // namespace mcppls::server
