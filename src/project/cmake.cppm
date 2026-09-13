// CMake as a data source: its build database when exported, its compile
// database otherwise, and a private configure in trusted workspaces (design D12).
export module lspmcpp.project.cmake;

import std;
import lspmcpp.base.error;
import lspmcpp.project.detect;
import lspmcpp.project.infer;
import lspmcpp.project.provider;

export namespace lspmcpp::project {

base::Result<InferredDatabase> load_cmake(const Detection& detection, std::string_view privateBuildDirectory, const ProviderContext& context);

} // namespace lspmcpp::project
