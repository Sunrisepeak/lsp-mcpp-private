// The lsp-mcpp command line (design section 12.2):
//   lsp-mcpp [serve] [--payload DIR] [--clangd PATH] [--kit DIR] [--untrusted] [--log-level L]
//   lsp-mcpp check <file> [--payload DIR] [--clangd PATH] [--kit DIR]
//   lsp-mcpp model [--root DIR] [--export s1|compile-commands|engine] [--payload DIR] [--kit DIR]
//   lsp-mcpp version
export module lspmcpp.server.cli;

import std;

export namespace lspmcpp::server {

int run_cli(int argc, char* argv[]);

} // namespace lspmcpp::server
