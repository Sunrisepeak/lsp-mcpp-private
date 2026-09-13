// Well-known directories: where this program keeps state, where temporary
// files go, and the user's home.
export module lspmcpp.platform.dirs;

import std;

export namespace lspmcpp::platform::dirs {

std::string home_directory();
// <user cache>/lsp-mcpp. LSP_MCPP_CACHE_DIR overrides the whole path.
std::string cache_directory();
std::string temp_directory();

} // namespace lspmcpp::platform::dirs
