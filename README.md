# lsp-mcpp

编译器无关的 C++ 模块语言服务器。

C++20 把 named modules 写进了语言标准，但 BMI 格式、依赖扫描、`import std` 的来源在 GCC、Clang、MSVC 之间各不相同，
导致同一份模块代码换一个编译器，编辑器里的跳转、补全、诊断就可能全部失效。
lsp-mcpp 把任何构建（mcpp、CMake、`compile_commands.json`，甚至什么都没有）归一化成同一种模块描述，
驱动锁定版本的 clangd，并补上模块级功能，让 C++ 模块在任何编译器、任何平台上都“装上就能用”。

## 能做什么

| 场景 | 行为 |
|---|---|
| mcpp 工程（GCC 16 / LLVM 22） | 调用 `mcpp build --configure-only` 取得编译数据库，探测工具链，GCC 参数翻译给 clangd，注入该工具链的 `std` 模块 |
| CMake 工程（`FILE_SET CXX_MODULES`） | 读取构建目录的 `build_database.json` 或 `compile_commands.json`（展开 `@modmap`）；没有构建目录且工作区受信任时私有配置一次 |
| 只有 `compile_commands.json` | 扫描源码补全模块角色，按驱动探测工具链；MinGW-w64 等交叉目标同样适用 |
| 只有源码、没有编译器 | 用内置的语义工具包（与 clangd 同版本的 libc++）提供完整模块语义 |
| 不受信任的工作区 | 不执行任何构建工具或编译器，只用语义工具包，并在状态中说明 |

模块级功能由语法模块索引直接回答：模块名跳转（含分区与实现单元）、`import` 补全、模块悬停、大纲中的模块节点、
按模块名搜索、无法解析 / 歧义 / 跨模块导入分区的诊断，以及 S3 扩展 `cxxModules/status`、`graph`、`moduleInfo`、`contexts`、`setContext`。
其余请求转发给 clangd 23.1，并带看门狗、崩溃重启与导入可解析性检查（避开 clangd 23.1 在模块无法解析时的挂起）。

## 构建与测试

服务端是纯 C++23 模块（没有头文件与宏），基于 [openkal](https://github.com/mcpplibs/openkal) 平台层，用 [mcpp](https://github.com/mcpp-community/mcpp) 构建：

```bash
mcpp build                                   # Linux 本机
mcpp build --target aarch64-macos            # macOS 主机，或从 Linux 交叉构建
mcpp build --target x86_64-windows-gnu       # Windows 主机，或从 Linux 交叉构建
mcpp test                                    # 单元测试（tests/，lspmcpp.testing）
```

一致性测试直接驱动 LSP，见 [conformance/README.md](conformance/README.md)：

```bash
lsp-mcpp-conformance run --server <lsp-mcpp> --fixture conformance/fixtures/inferred --payload <payload>
```

命令行：

```bash
lsp-mcpp [serve] [--payload DIR] [--clangd PATH] [--kit DIR] [--untrusted]   # 以 stdio 运行语言服务器
lsp-mcpp check <file>                       # 输出工程模型、语义配置、模块诊断，并运行 clangd --check
lsp-mcpp model [--root DIR] [--export s1|compile-commands|engine]
lsp-mcpp version
```

## 仓库结构

| 目录 | 内容 |
|---|---|
| [specs/](specs/) | 规范：S1 构建数据库 IDE Profile、S2 发现协议、S3 LSP 模块扩展、S4 语义工具包，含 JSON Schema 与示例 |
| [src/](src/) | 服务端，`src/<目录>/<名字>.cppm` 对应模块 `lspmcpp.<目录>.<名字>` |
| [os/](os/) | 按目标选择的 `lspmcpp.os` 平台常量包，代码中以 `if constexpr` 使用 |
| [tests/](tests/)、[testing/](testing/) | 单元测试与测试支持包 |
| [conformance/](conformance/) | 一致性测试用例与说明 |
| [editors/vscode/](editors/vscode/) | VS Code 扩展“C++ Modules”（`mcpp-community.lsp-mcpp`） |
| [packaging/](packaging/) | clangd 与语义工具包 `lsp-mcpp-kit` 的锁文件与组装脚本 |
| [tools/lspgen/](tools/lspgen/) | LSP 3.18 meta model，协议模块由 `lsp-mcpp-lspgen` 生成 |
| [.agents/docs/](.agents/docs/) | 设计、调研、实验与实现计划 |

## 文档

- [设计方案](.agents/docs/2026-09-13-cxx-modules-unified-lsp-design.md)（决策 D1–D25、生态问题记录 12.9）
- [实现计划](.agents/docs/2026-09-14-lsp-mcpp-v1-implementation-plan.md)
- [规范草案（中文原稿）](.agents/docs/2026-09-13-cxx-module-build-database-ide-profile-spec.md)
- [调研综述](.agents/docs/2026-09-13-cxx-modules-landscape-research.md)
- [本地实测](.agents/docs/2026-09-13-cxx-modules-lsp-experiments.md)

## 许可

Apache-2.0
