// The MCP entry (overall design 7.6, S5 6): JSON-RPC messages, one per line, on standard input and
// output. A headless session serves the workspace; tool calls are answered one at a time on its loop,
// and the workspace keeps working (model loads, file watches, the engines) while none is.
export module mcppls.ai.mcp.server;

import std;
import nlohmann.json;
import mcppls.orchestrator.kernel;

export namespace mcppls::ai::mcp {

// The MCP revisions this server speaks, newest first.
inline constexpr std::array<std::string_view, 3> PROTOCOL_VERSIONS { "2025-06-18", "2025-03-26", "2024-11-05" };

struct ServerOptions {
    orchestrator::KernelOptions kernel;
    std::chrono::seconds toolTimeout { 120 };
};

// Handles the messages of one MCP connection against a kernel; a transport feeds it.
class Session {
public:
    Session(orchestrator::Kernel& kernel, std::chrono::seconds toolTimeout, std::function<void(const nlohmann::json&)> send);
    void handle(const nlohmann::json& message);
    const std::string& protocol_version() const { return protocolVersion_; }

private:
    orchestrator::Kernel& kernel_;
    std::chrono::seconds toolTimeout_;
    std::function<void(const nlohmann::json&)> send_;
    std::string protocolVersion_;
};

// Serves MCP on standard input and output until the input ends.
int run_server(const ServerOptions& options);

// One message per line; a line that is not JSON gives a parse error for the caller to send.
std::vector<std::string> take_lines(std::string& buffer);

} // namespace mcppls::ai::mcp
