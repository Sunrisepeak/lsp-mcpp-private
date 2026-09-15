// mcppls: the language server and its command line.
import std;
import mcppls.server.cli;

int main(int argc, char* argv[]) {
    return mcppls::server::run_cli(argc, argv);
}
