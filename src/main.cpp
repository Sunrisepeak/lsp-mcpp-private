import std;
import lspmcpp.os;
import lspmcpp.base.version;
import lspmcpp.platform.process;
import lspmcpp.platform.env;
import lspmcpp.platform.fs;
import lspmcpp.platform.dirs;
import lspmcpp.platform.stdio;
import lspmcpp.platform.task;
import nlohmann.json;

int main() {
    std::println("lsp-mcpp {} ({})", lspmcpp::base::VERSION, lspmcpp::os::FAMILY_NAME);
    return 0;
}
