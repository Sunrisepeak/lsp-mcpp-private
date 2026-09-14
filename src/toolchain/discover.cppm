// Finding compilers on this machine, in the order design section 14.3 gives:
// PATH, the mcpp and xlings toolchain stores, Homebrew LLVM, Visual Studio.
export module lspmcpp.toolchain.discover;

import std;
import lspmcpp.spec.database;
import lspmcpp.toolchain.probe;

export namespace lspmcpp::toolchain {

struct CompilerCandidate {
    std::string driver;
    spec::Family family { spec::Family::other };
    std::string origin;   // PATH | mcpp | xlings | homebrew | visual-studio | setting
};

std::vector<CompilerCandidate> discover_compilers(const Runner& runner);

// Whether macOS has developer tools the /usr/bin shims (clang++, c++, xcrun) would run, decided from
// the file system alone: a shim started without them opens the system's installation dialog, which a
// language server must never cause. Always false elsewhere.
bool macos_developer_tools_present();

} // namespace lspmcpp::toolchain
