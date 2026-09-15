# mcpp-language-server（mcppls）

AI 时代的 C++ 语言服务引擎：编译器无关的 C++ 模块语言服务器，同时服务编辑器、编码代理与 CI。
可执行文件、C++ 模块与命名空间都叫 `mcppls`；项目原名 lsp-mcpp，改名见[总体设计](.agents/docs/2026-09-15-mcppls-overall-design.md)第 12 节。

C++20 把 named modules 写进了语言标准，但 BMI 格式、依赖扫描、`import std` 的来源在 GCC、Clang、MSVC 之间各不相同，
导致同一份模块代码换一个编译器，编辑器里的跳转、补全、诊断就可能全部失效。
mcppls 把任何构建（mcpp、CMake、`compile_commands.json`，甚至什么都没有）归一化成同一种模块描述，
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
引擎抽象层让 mcppls 自己的模块引擎与 clangd 并列服务同一工作区（`--engine none` 时只用前者）。

### 面向代理与 CI（规范 S5）

| 能力 | 入口 |
|---|---|
| 符号、引用、调用者与被调用者（在符号所在模块及其所有导入方中查找，含重导出） | MCP `cxx_symbol`、`cxx_references`；`mcppls query symbol/refs/calls` |
| 文件大纲、模块描述与接口摘要、模块图、文件的构建上下文 | `cxx_outline`、`cxx_module`、`cxx_build_context`；`mcppls query outline/module/context` |
| 按当前磁盘内容的新鲜诊断；编辑后校验（改动文件及其导入方）、候选片段原位校验、跨工具链校验 | `cxx_diagnostics`、`cxx_verify`；`mcppls diagnostics`、`mcppls verify` |
| 变更审查：语义 diff、影响分析、带证据的确定性规则发现，SARIF / LSP / Markdown 输出 | `cxx_impact`、`cxx_review`；`mcppls impact`、`mcppls review`；编辑器命令“Review Changes” |
| 模型判断（可选，默认关闭）：代理自身、模型网关 `mcppls-model`、MCP sampling；证据校验与修复校验 | `cxx_review {model}`；`mcppls review --model` |
| 共享的工作区守护进程 | `mcppls mcp --daemon`、`mcppls daemon` |

接入 Claude Code、GitHub Copilot CLI 与其他 MCP 客户端见 [editors/agents/README.md](editors/agents/README.md)。

## 构建与测试

服务端是纯 C++23 模块（没有头文件与宏），基于 [openkal](https://github.com/mcpplibs/openkal) 平台层，用 [mcpp](https://github.com/mcpp-community/mcpp) 构建：

```bash
mcpp build                                   # Linux 本机
mcpp build --target aarch64-macos            # macOS 主机，或从 Linux 交叉构建
mcpp build --target x86_64-windows-gnu       # Windows 主机，或从 Linux 交叉构建
mcpp test                                    # 单元测试（tests/，mcppls.testing）
```

一致性测试直接驱动 LSP，见 [conformance/README.md](conformance/README.md)：

```bash
mcppls-conformance run --server <mcppls> --fixture conformance/fixtures/inferred --payload <payload>
```

命令行：

```bash
mcppls [serve] [--payload DIR] [--clangd PATH] [--kit DIR] [--engine clangd|none] [--untrusted]   # LSP over stdio
mcppls mcp [--root DIR] [--daemon]        # MCP over stdio，代理工具（S5 第 6 节）
mcppls query symbol|refs|calls|outline|module|context ...   # 语义查询，输出 S5 JSON
mcppls diagnostics <file>... | verify [--changed] | impact | review [--base REV] [--format sarif]
mcppls daemon run|start|status|stop       # 工作区守护进程
mcppls check <file>                       # 输出工程模型、语义配置、模块诊断，并运行 clangd --check
mcppls model [--root DIR] [--export s1|compile-commands|engine]
mcppls version
```

审查夹具（精确率与召回率）见 [bench/README.md](bench/README.md)：`python3 bench/review.py --server <mcppls> --payload <payload>`。

## 仓库结构

| 目录 | 内容 |
|---|---|
| [specs/](specs/) | 规范：S1 构建数据库 IDE Profile、S2 发现协议、S3 LSP 模块扩展、S4 语义工具包、S5 语义查询与审查，含 JSON Schema 与示例 |
| [src/](src/) | 服务端，`src/<目录>/<名字>.cppm` 对应模块 `mcppls.<目录>.<名字>` |
| [os/](os/) | 按目标选择的 `mcppls.os` 平台常量包，代码中以 `if constexpr` 使用 |
| [tests/](tests/)、[testing/](testing/) | 单元测试与测试支持包 |
| [conformance/](conformance/) | 一致性测试用例与说明 |
| [editors/vscode/](editors/vscode/) | VS Code 扩展“C++ Modules”（`mcpp-community.mcpp-language-server`） |
| [editors/claude-code/](editors/claude-code/)、[editors/copilot-cli/](editors/copilot-cli/)、[editors/agents/](editors/agents/) | 编码代理接入：LSP 插件、MCP 配置与说明 |
| [bench/](bench/) | 代理任务基准与审查夹具 |
| [model-gateway/](model-gateway/) | 模型网关参考实现 `mcppls-model`（独立的 mcpp 包） |
| [packaging/](packaging/) | clangd 与语义工具包 `mcppls-kit` 的锁文件与组装脚本 |
| [tools/lspgen/](tools/lspgen/) | LSP 3.18 meta model，协议模块由 `mcppls-lspgen` 生成 |
| [.agents/docs/](.agents/docs/) | 设计、调研、实验与实现计划 |

## 文档

- [总体设计：AI 时代的 C++ 语言服务引擎](.agents/docs/2026-09-15-mcppls-overall-design.md)（决策 D29–D40，取代 v1 的 D2、D19、D22）
- [v1 设计方案](.agents/docs/2026-09-13-cxx-modules-unified-lsp-design.md)（决策 D1–D28、生态问题记录 12.9）
- [实现计划](.agents/docs/2026-09-14-lsp-mcpp-v1-implementation-plan.md)
- [规范草案（中文原稿）](.agents/docs/2026-09-13-cxx-module-build-database-ide-profile-spec.md)
- [调研综述](.agents/docs/2026-09-13-cxx-modules-landscape-research.md)
- [本地实测](.agents/docs/2026-09-13-cxx-modules-lsp-experiments.md)

## 许可

Apache-2.0
