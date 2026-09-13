module lspmcpp.base.error;

import std;

namespace lspmcpp::base {

Error make_error(std::string_view code, std::string message) {
    return Error { std::string { code }, std::move(message) };
}

std::unexpected<Error> fail(std::string_view code, std::string message) {
    return std::unexpected<Error> { make_error(code, std::move(message)) };
}

std::string to_string(const Error& error) {
    return std::format("[{}] {}", error.code, error.message);
}

} // namespace lspmcpp::base
