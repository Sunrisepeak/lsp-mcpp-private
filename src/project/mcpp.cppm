// mcpp as a producer (design section 14.2): `mcpp emit build-database --format json`
// when this mcpp advertises the kind (mcpp-community/mcpp#636), otherwise
// `mcpp build --configure-only`'s compile database plus scanning and probing.
export module lspmcpp.project.mcpp;

import std;
import lspmcpp.base.error;
import lspmcpp.project.compdb;
import lspmcpp.project.detect;
import lspmcpp.project.infer;
import lspmcpp.project.provider;

export namespace lspmcpp::project {

std::string mcpp_package_name(std::string_view manifestText);

// The standard library's module units of an mcpp compile database (usable plan W8, until mcpp
// describes them itself, mcpp-community/mcpp#636). A package that supplies `std`, such as
// openkal-llvm-runtime, is built into mcpp's std build cache outside the compile database: the
// build directory's build.ninja stages that BMI, and std-module.json beside it records the
// sources and the commands. Empty when the commands name no staged std BMI or the record is unreadable.
std::vector<CompileCommand> mcpp_standard_units(std::span<const CompileCommand> commands);
base::Result<InferredDatabase> load_mcpp(const Detection& detection, const ProviderContext& context);

} // namespace lspmcpp::project
