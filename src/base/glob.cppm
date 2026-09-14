// Glob patterns in the syntax of LSP 3.17/3.18 (`*`, `**`, `?`, `{a,b}`, `[...]`, `[!...]`), which S2
// uses for the inputs a producer asks a consumer to watch. Paths use '/' separators.
export module lspmcpp.base.glob;

import std;
import lspmcpp.os;

export namespace lspmcpp::base {

// Whether `path` matches `pattern`; both are '/'-separated and compared segment by segment.
bool glob_match(std::string_view pattern, std::string_view path, bool caseInsensitive = lspmcpp::os::CASE_INSENSITIVE_PATHS);

} // namespace lspmcpp::base
