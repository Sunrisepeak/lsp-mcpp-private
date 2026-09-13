// mcpp as a producer (design section 14.2). Until `mcpp emit build-database`
// exists, the model comes from `mcpp build --configure-only`'s compile database
// plus scanning and probing.
export module lspmcpp.project.mcpp;

import std;
import lspmcpp.base.error;
import lspmcpp.project.detect;
import lspmcpp.project.infer;
import lspmcpp.project.provider;

export namespace lspmcpp::project {

std::string mcpp_package_name(std::string_view manifestText);
base::Result<InferredDatabase> load_mcpp(const Detection& detection, const ProviderContext& context);

} // namespace lspmcpp::project
