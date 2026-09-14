// lsp-mcpp: the language server and its command line.
import std;
import lspmcpp.server.cli;

int main(int argc, char* argv[]) {
    return lspmcpp::server::run_cli(argc, argv);
}
