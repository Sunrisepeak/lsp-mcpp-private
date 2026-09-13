// file: URIs as editors send them. VS Code writes Windows paths as
// "file:///c%3A/Users/x"; both that form and "file:///C:/Users/x" are accepted.
export module lspmcpp.base.uri;

import std;
import lspmcpp.base.error;
import lspmcpp.base.path;

export namespace lspmcpp::base {

std::string percent_decode(std::string_view text);
std::string percent_encode_path(std::string_view path);
Result<std::string> uri_to_path(std::string_view uri, PathStyle style = NATIVE_PATH_STYLE);
std::string path_to_uri(std::string_view path, PathStyle style = NATIVE_PATH_STYLE);

} // namespace lspmcpp::base
