// cl.exe and clang-cl dialect arguments translated for the engine in cl mode
// (design section 14.4, rows P6 and P7).
export module lspmcpp.normalize.msvc;

import std;
import lspmcpp.toolchain.probe;

export namespace lspmcpp::normalize {

inline constexpr std::string_view ENGINE_CLANG_CL_DRIVER { "clang-cl.exe" };

struct MsvcInput {
    std::span<const std::string> arguments;   // full command, argv[0] first, response files expanded
    std::string source;
    std::string workDirectory;
    const toolchain::ToolchainFacts* facts { nullptr };
    bool importable { false };
    std::string vcToolsDirectory;             // VCToolsInstallDir when known
    std::string windowsSdkDirectory;          // WindowsSdkDir when known
};

std::vector<std::string> translate_msvc(const MsvcInput& input);

} // namespace lspmcpp::normalize
