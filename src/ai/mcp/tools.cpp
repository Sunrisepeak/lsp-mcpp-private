module mcppls.ai.mcp.tools;

import std;
import nlohmann.json;
import mcppls.spec.query;
import mcppls.ai.query.view;
import mcppls.ai.query.symbols;
import mcppls.ai.query.files;
import mcppls.ai.query.modules;
import mcppls.ai.context.build;

namespace mcppls::ai::mcp {

namespace {

using Json = nlohmann::json;
using query::Clock;

Json property(std::string_view type, std::string_view description) {
    return Json { { "type", std::string { type } }, { "description", std::string { description } } };
}

Json read_only(std::string_view title) {
    return Json { { "title", std::string { title } }, { "readOnlyHint", true }, { "destructiveHint", false }, { "idempotentHint", true }, { "openWorldHint", false } };
}

Json target_properties() {
    return Json {
        { "name", property("string", "Name, unqualified or qualified: greet, hello::greet") },
        { "id", property("string", "A symbol id from an earlier result") },
        { "file", property("string", "File relative to the workspace root, for a position") },
        { "line", Json { { "type", "integer" }, { "minimum", 1 }, { "description", "Line, from 1" } } },
        { "column", Json { { "type", "integer" }, { "minimum", 1 }, { "description", "Column in characters, from 1" } } },
        { "maxResults", Json { { "type", "integer" }, { "minimum", 1 } } },
    };
}

Json tool(std::string_view name, std::string_view title, std::string_view description, Json properties, std::vector<std::string> required = {}) {
    Json schema { { "type", "object" }, { "properties", std::move(properties) } };
    if (!required.empty()) schema["required"] = required;
    return Json { { "name", std::string { name } }, { "title", std::string { title } }, { "description", std::string { description } },
                  { "inputSchema", std::move(schema) }, { "annotations", read_only(title) } };
}

// Arguments are checked against what each tool reads: a wrong type is the caller's error, said plainly.
struct Arguments {
    const Json& value;

    std::optional<query::Failure> check(std::string_view key, Json::value_t type) const {
        const auto found = value.find(key);
        if (found == value.end() || found->is_null()) return std::nullopt;
        const bool matches { type == Json::value_t::number_integer ? found->is_number_integer() : found->type() == type };
        if (matches) return std::nullopt;
        return query::invalid_arguments(std::format("argument {} has the wrong type", key));
    }
    std::string string(std::string_view key) const {
        const auto found = value.find(key);
        return found != value.end() && found->is_string() ? found->get<std::string>() : std::string {};
    }
    int integer(std::string_view key, int fallback) const {
        const auto found = value.find(key);
        return found != value.end() && found->is_number_integer() ? found->get<int>() : fallback;
    }
    bool boolean(std::string_view key, bool fallback) const {
        const auto found = value.find(key);
        return found != value.end() && found->is_boolean() ? found->get<bool>() : fallback;
    }
    std::vector<std::string> strings(std::string_view key) const {
        std::vector<std::string> values;
        const auto found = value.find(key);
        if (found == value.end()) return values;
        if (found->is_string()) values.push_back(found->get<std::string>());
        if (found->is_array()) {
            for (const auto& item : *found) {
                if (item.is_string()) values.push_back(item.get<std::string>());
            }
        }
        return values;
    }
};

query::SymbolTarget target_of(const Arguments& arguments) {
    query::SymbolTarget target;
    target.name = arguments.string("name");
    target.id = arguments.string("id");
    target.file = arguments.string("file");
    target.line = arguments.integer("line", 0);
    target.column = arguments.integer("column", 1);
    target.kind = arguments.string("kind");
    target.module = arguments.string("module");
    return target;
}

std::size_t limit_of(const Arguments& arguments, std::size_t fallback) {
    return static_cast<std::size_t>(std::max(1, arguments.integer("maxResults", static_cast<int>(fallback))));
}

ToolResult failure(const query::Failure& failed) { return ToolResult { Json { { "error", query::to_json(failed) } }, true }; }

template <class T>
ToolResult result_of(const query::Outcome<T>& outcome) {
    if (!outcome) return failure(outcome.error());
    return ToolResult { query::to_json(*outcome), false };
}

} // namespace

Json tool_list() {
    Json tools = Json::array();
    Json symbol = target_properties();
    symbol["kind"] = property("string", "Only this kind: function, method, class, struct, namespace, variable, field, enum, module");
    symbol["module"] = property("string", "Only symbols declared in this module");
    tools.push_back(tool("cxx_symbol", "C++ symbol",
                         "Find C++ symbols by name, id or position: declaration and definition locations, signature, type, documentation and module.",
                         std::move(symbol)));
    Json references = target_properties();
    references["direction"] = Json { { "type", "string" }, { "enum", Json::array({ "references", "callers", "callees" }) }, { "description", "Default references" } };
    references["includeDeclaration"] = property("boolean", "Include the declaration itself (default true)");
    tools.push_back(tool("cxx_references", "C++ references",
                         "References to a C++ symbol grouped by module and file, or its callers or callees. Searches the symbol's module and every unit that imports it.",
                         std::move(references)));
    tools.push_back(tool("cxx_outline", "C++ outline", "A compact outline of a file: its module declaration and declarations with kinds and lines.",
                         Json { { "file", property("string", "File relative to the workspace root") } }, { "file" }));
    tools.push_back(tool("cxx_module", "C++ module",
                         "A C++ module's units, partitions, imports and importers; with graph, the module graph of modules whose names contain the name.",
                         Json { { "name", property("string", "Module name; a partition names its module") },
                                { "file", property("string", "A file of the module, instead of its name") },
                                { "graph", property("boolean", "The module graph instead") },
                                { "maxResults", Json { { "type", "integer" }, { "minimum", 1 } } } }));
    tools.push_back(tool("cxx_build_context", "C++ build context",
                         "How a file is built and read: sets and role, compiler, standard library, language standard, macros, and issues.",
                         Json { { "file", property("string", "File relative to the workspace root") } }, { "file" }));
    tools.push_back(tool("cxx_diagnostics", "C++ diagnostics",
                         "Compiler diagnostics of files as they are on disk now, waiting for fresh results unless fresh is false.",
                         Json { { "files", Json { { "type", "array" }, { "items", Json { { "type", "string" } } }, { "description", "Files relative to the workspace root" } } },
                                { "fresh", property("boolean", "Wait for the compiler's diagnostics of the current content (default true)") } },
                         { "files" }));
    return tools;
}

bool has_tool(std::string_view name) {
    const Json tools = tool_list();
    return std::ranges::any_of(tools, [&](const Json& tool) { return tool.value("name", std::string {}) == name; });
}

ToolResult call_tool(query::View& view, std::string_view name, const Json& value, Clock::time_point deadline) {
    const Json empty = Json::object();
    const Arguments arguments { value.is_object() ? value : empty };
    for (const auto& [key, type] : std::initializer_list<std::pair<std::string_view, Json::value_t>> {
             { "name", Json::value_t::string }, { "id", Json::value_t::string }, { "file", Json::value_t::string }, { "kind", Json::value_t::string },
             { "module", Json::value_t::string }, { "direction", Json::value_t::string }, { "line", Json::value_t::number_integer },
             { "column", Json::value_t::number_integer }, { "maxResults", Json::value_t::number_integer }, { "includeDeclaration", Json::value_t::boolean },
             { "graph", Json::value_t::boolean }, { "fresh", Json::value_t::boolean } }) {
        if (auto wrong = arguments.check(key, type)) return failure(*wrong);
    }

    if (name == "cxx_symbol") {
        auto found = query::find_symbols(view, target_of(arguments), query::Limit { limit_of(arguments, 20) }, true, deadline);
        if (found && found->symbols.empty()) return failure(query::not_found(std::format("no symbol named {}", arguments.string("name"))));
        return result_of(found);
    }
    if (name == "cxx_references") {
        const std::string direction { arguments.string("direction") };
        if (direction == "callers" || direction == "callees") {
            return result_of(query::find_calls(view, target_of(arguments), direction == "callers" ? query::CallDirection::incoming : query::CallDirection::outgoing,
                                               query::Limit { limit_of(arguments, 100) }, deadline));
        }
        if (!direction.empty() && direction != "references") return failure(query::invalid_arguments("direction is references, callers or callees"));
        return result_of(query::find_references(view, target_of(arguments), arguments.boolean("includeDeclaration", true), query::Limit { limit_of(arguments, 200) },
                                                deadline));
    }
    if (name == "cxx_outline") {
        if (arguments.string("file").empty()) return failure(query::invalid_arguments("file is required"));
        return result_of(query::outline_file(view, arguments.string("file"), deadline));
    }
    if (name == "cxx_module") {
        (void)view.settle(deadline);
        if (arguments.boolean("graph", false)) {
            return ToolResult { query::to_json(query::module_graph(view, arguments.string("name"), query::Limit { limit_of(arguments, 500) })), false };
        }
        return result_of(query::describe_module(view, arguments.string("name"), arguments.string("file")));
    }
    if (name == "cxx_build_context") {
        if (arguments.string("file").empty()) return failure(query::invalid_arguments("file is required"));
        auto context = context::build_context(view, arguments.string("file"), deadline);
        if (!context) return failure(context.error());
        return ToolResult { context::to_json(*context), false };
    }
    if (name == "cxx_diagnostics") {
        const auto files = arguments.strings("files");
        if (files.empty()) return failure(query::invalid_arguments("files is required"));
        return result_of(query::file_diagnostics(view, files, arguments.boolean("fresh", true), deadline));
    }
    return failure(query::Failure { "unknown-tool", std::format("no tool named {}", name), nullptr });
}

} // namespace mcppls::ai::mcp
