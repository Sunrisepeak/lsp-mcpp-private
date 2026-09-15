export module mcppls.base.version;

import std;

export namespace mcppls::base {

inline constexpr std::string_view VERSION { "0.1.0" };
inline constexpr std::string_view DATABASE_SPEC_VERSION { "0.2.0" };
inline constexpr std::string_view KIT_SPEC_VERSION { "1" };
inline constexpr std::string_view CLANGD_VERSION { "23.1.0" };
inline constexpr std::string_view KIT_NAME { "mcppls-kit" };

} // namespace mcppls::base
