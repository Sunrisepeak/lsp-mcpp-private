module lspmcpp.normalize.msvc;

import std;
import lspmcpp.base.path;
import lspmcpp.base.text;
import lspmcpp.toolchain.probe;

namespace lspmcpp::normalize {

namespace {

// Options are case-sensitive in cl; both '/' and '-' introduce them.
std::string_view option_body(std::string_view argument) {
    if (argument.size() > 1 && (argument.front() == '/' || argument.front() == '-')) return argument.substr(1);
    return {};
}

bool is_option(std::string_view argument, std::string_view name) { return option_body(argument) == name; }

bool option_starts(std::string_view argument, std::string_view prefix) {
    const std::string_view body { option_body(argument) };
    return !body.empty() && body.starts_with(prefix);
}

} // namespace

std::vector<std::string> translate_msvc(const MsvcInput& input) {
    std::vector<std::string> out { "--driver-mode=cl" };
    // cl commands use Windows path rules on every host.
    const std::string source { base::normalize_path(input.source, base::PathStyle::windows) };
    for (std::size_t i { 1 }; i < input.arguments.size(); ++i) {
        const std::string_view argument { input.arguments[i] };
        // Options whose value is the next argument.
        if (is_option(argument, "reference") || is_option(argument, "ifcSearchDir") || is_option(argument, "ifcOutput")
            || is_option(argument, "headerUnit") || is_option(argument, "sourceDependencies")
            || is_option(argument, "sourceDependencies:directives") || is_option(argument, "scanDependencies")
            || is_option(argument, "headerUnit:quote") || is_option(argument, "headerUnit:angle")) {
            if (i + 1 < input.arguments.size() && !option_body(input.arguments[i + 1]).size()) ++i;
            continue;
        }
        if (option_starts(argument, "reference") || option_starts(argument, "ifcOutput") || option_starts(argument, "ifcSearchDir")
            || is_option(argument, "interface") || is_option(argument, "internalPartition") || option_starts(argument, "headerUnit")
            || option_starts(argument, "scanDependencies") || option_starts(argument, "sourceDependencies")
            || option_starts(argument, "Fo") || option_starts(argument, "Fd") || option_starts(argument, "Fp")
            || option_starts(argument, "FS") || is_option(argument, "c") || option_starts(argument, "Tp")
            || option_starts(argument, "showIncludes") || option_starts(argument, "experimental:module")) {
            continue;
        }
        if (!argument.starts_with('/') && !argument.starts_with('-')
            && base::same_path(base::join_path(input.workDirectory, argument, base::PathStyle::windows), source, true)) {
            continue;
        }
        out.emplace_back(argument);
    }
    if (!input.vcToolsDirectory.empty()) out.push_back("/vctoolsdir" + input.vcToolsDirectory);
    if (!input.windowsSdkDirectory.empty()) out.push_back("/winsdkdir" + input.windowsSdkDirectory);
    if (input.importable) {
        // clang-cl passes GNU driver options through /clang:.
        out.emplace_back("/clang:-xc++-module");
    }
    return out;
}

} // namespace lspmcpp::normalize
