// The mcppls command line (design section 12.2):
//   mcppls [serve] [--payload DIR] [--clangd PATH] [--kit DIR] [--untrusted] [--log-level L]
//   mcppls check <file> [--payload DIR] [--clangd PATH] [--kit DIR]
//   mcppls model [--root DIR] [--export s1|compile-commands|engine] [--payload DIR] [--kit DIR]
//   mcppls version
export module mcppls.server.cli;

import std;

export namespace mcppls::server {

int run_cli(int argc, char* argv[]);

} // namespace mcppls::server
