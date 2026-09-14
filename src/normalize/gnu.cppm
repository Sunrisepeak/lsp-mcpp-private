// GCC and Clang dialect arguments translated for the Clang-based engine
// (design section 14.4, rows P1-P5), and engine commands built from a kit.
export module lspmcpp.normalize.gnu;

import std;
import lspmcpp.spec.database;
import lspmcpp.spec.kit;
import lspmcpp.toolchain.probe;

export namespace lspmcpp::normalize {

// The driver name written into engine commands when the build's own driver is
// not a Clang driver. The engine never executes it.
inline constexpr std::string_view ENGINE_CLANG_DRIVER { "clang++" };

struct GnuInput {
    std::span<const std::string> arguments;   // full compile command, argv[0] first, response files expanded
    std::string source;                       // absolute path of the unit's source
    std::string workDirectory;
    const toolchain::ToolchainFacts* facts { nullptr };
    bool importable { false };
};

// Arguments without argv[0], output, dependency files, BMI and scanning flags,
// the source file or -x; for GCC with target, standard library and installation made explicit.
std::vector<std::string> translate_gnu(const GnuInput& input);
// For a toolchain that targets the MSVC ABI: target, emulated cl version, the Visual
// Studio toolset and Windows SDK, and aligned allocation off with the MSVC STL
// (clangd 23.1 reports `align_val_t` as ambiguous inside its std module otherwise;
// usable plan E9). Arguments already in `existing` are not repeated.
std::vector<std::string> windows_msvc_arguments(const toolchain::ToolchainFacts& facts, std::span<const std::string> existing);
// Semantic arguments any dialect can keep: include paths, macros, standard, forced includes.
std::vector<std::string> semantic_subset(std::span<const std::string> arguments);
// Base arguments for a kit: target, standard, the kit's own arguments, include directories, sysroot.
std::vector<std::string> kit_arguments(const spec::Kit& kit, std::string_view languageStandard, std::string_view macosSdk);
// The -std= value in arguments, or empty.
std::string language_standard_of(std::span<const std::string> arguments);

} // namespace lspmcpp::normalize
