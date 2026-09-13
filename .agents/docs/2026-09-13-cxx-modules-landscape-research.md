# C++ Modules 工具链与 LSP 生态调研

日期：2026-09-13（2026-09-14 补充 P2977 标准化状态、mcpp Windows 工具链形态、编辑器体验对标）
关联文档：
- 设计方案：[2026-09-13-cxx-modules-unified-lsp-design.md](2026-09-13-cxx-modules-unified-lsp-design.md)
- 规范草案：[2026-09-13-cxx-module-build-database-ide-profile-spec.md](2026-09-13-cxx-module-build-database-ide-profile-spec.md)
- 本地实测：[2026-09-13-cxx-modules-lsp-experiments.md](2026-09-13-cxx-modules-lsp-experiments.md)

> 说明：本文结论来自 2026-09 的公开资料与本地实测。标注“未核实”的条目只有二手来源，落地前需复查。
> 本地实测过的结论统一标注“实测”，细节见实验文档。

---

## 1. 问题本质：语言层统一，物理层各自为政

C++20 把 named module 写进了语言标准，但标准只规定语义，不规定下面这些物理层细节：

| 物理层问题 | 现状 |
|---|---|
| BMI（Built Module Interface）格式 | GCC `.gcm`、Clang `.pcm`、MSVC `.ifc`，互不可读，且绑定编译器版本与部分编译选项 |
| 如何告诉编译器 BMI 在哪 | GCC module mapper / `gcm.cache`，Clang `-fmodule-file=`，MSVC `/reference` |
| 如何扫描模块依赖 | P1689 是共同格式，但三家实现字段不一致（实测） |
| 如何识别接口单元 | GCC 看内容，Clang 看扩展名或 `-x c++-module`，MSVC 看 `.ixx` 或 `/interface` |
| `import std` 从哪来 | libc++ / libstdc++ / MSVC STL 各自提供源码与清单，位置与发现方式不同 |
| 构建系统如何表达模块图 | CMake 用 `.ddi` + `.modmap`，mcpp 用自研扫描 + ninja dyndep，Bazel 用 `.CXXModules.json` |
| IDE 如何拿到模块信息 | `compile_commands.json` 没有模块图，且条目在模块场景下不可独立重放（实测） |

结果是：同一份模块化代码，换一个编译器，IDE 的跳转、补全、诊断就可能全部失效。
mcpp 自己的结论也印证了这一点：mcpp-vscode 在 GCC 工程里只能提示用户切换到 LLVM 工具链（见 `mcpp-vscode/.agents/docs/2026-05-24-mcpp-vscode-clangd-design.md`）。

---

## 2. 三大编译器差异矩阵

| 维度 | GCC（15/16） | Clang（20–23） | MSVC（VS 2022 17.x / VS 2026） |
|---|---|---|---|
| 开启模块 | GCC 11–14 用 `-fmodules-ts`；GCC 15 起主参数为 `-fmodules`，`-fmodules-ts` 保留为别名（实测 13.3 仅识别旧名，15.1 与 16.1 两者均可） | `-std=c++20` 及以上即可；注意 Clang 的 `-fmodules` 是另一套 Clang header modules | `/std:c++20`（16.11 起）或 `/std:c++latest` |
| BMI 格式 | `.gcm`，官方文档称其为“可重建的缓存产物，不可分发” | `.pcm`；Reduced BMI 在 Clang 22 成为默认，会丢弃非 inline 函数体等信息 | `.ifc`，有公开 spec（microsoft/ifc-spec，仍为 draft）与实验性 SDK |
| 生产者与消费者需匹配的内容 | 编译器构建版本、方言、`-fmodules` | 编译器 commit 级版本、`-std` 等语言选项；源文件需在原路径 | 版本；`/std`、`/EH` 不一致只报 C5050 警告 |
| BMI 定位方式 | module mapper（`-fmodule-mapper=`，文件或进程协议），默认读写 cwd 下 `gcm.cache/` | `-fmodule-file=<name>=<path>`、`-fprebuilt-module-path=`、`-fmodule-output=` | `/reference name=path`、`/ifcSearchDir`、`/ifcOutput`、`/interface`、`/internalPartition` |
| 依赖扫描 | `-fdeps-format=p1689r5 -fdeps-file= -fdeps-target=`（GCC 14 起）；输出 `version: 0`、无 `source-path`（实测） | `clang-scan-deps -format=p1689`；输出 `version: 1`、带 `source-path`（实测） | `/scanDependencies` 输出 P1689；`/sourceDependencies:directives` 是私有格式 |
| 接口单元识别 | 看源码内容判定；但扩展名决定是否被当作 C++ 源（实测 GCC 13.3 把 `.cppm` 当作链接输入，15.1 可直接编译，14 未验证） | `.cppm/.ccm/.cxxm/.c++m` 或 `-x c++-module` | `.ixx` 或 `/interface /TP` |
| `import std` | libstdc++ 提供 `bits/std.cc` 与 `libstdc++.modules.json`；GCC 16 新增 `--compile-std-module` | libc++ 提供 `std.cppm` 与 `libc++.modules.json`，可用 `-print-library-module-manifest-path` 查询；Windows 上 Clang 对 MSVC STL 的 std 模块有 C++23 下限问题（mcpp#603、xmake#6181） | `$(VCToolsInstallDir)modules/` 下的 `std.ixx` 与 `modules.json`，需每个工程自行编译 |
| Header units | 标准库头不预置 header unit；分区可见性有已知缺陷 | 可用，但 header unit 的 BMI 不能产出目标文件；clang-scan-deps 不支持 header unit 扫描 | 三家中最成熟（`/exportHeader`、`/headerUnit`） |
| 语义分歧（影响 IDE 诊断可信度） | 实际上只有 GCC 严格执行 P1815 TU-local 规则，曾导致 fmt 在 GCC 15 编译失败；导出内部链接实体会报错 | 拒绝 `extern "C++" {}` 内的 `export`（llvm#60405、#76526）；有 ODR 误报（llvm#137533） | IFC 链接模型与 Itanium 混淆规则不同 |

对 IDE 的直接含义：

1. 不存在可共享的 BMI 中间表示，IDE 不能把“读取构建产出的 BMI”当作跨编译器方案。
2. P1689 是唯一跨三家的依赖描述，但字段有漂移，消费方必须容错。
3. `import std` 的发现路径是“编译器 + 标准库”二元组决定的，不能写死一条规则。
4. 同一份模块接口在不同编译器上的接受度不同，IDE 展示的诊断必须标注来源编译器。

主要来源：
[GCC Modules](https://gcc.gnu.org/onlinedocs/gcc/C_002b_002b-Modules.html)、
[GCC Module Mapper](https://gcc.gnu.org/onlinedocs/gcc/C_002b_002b-Module-Mapper.html)、
[GCC 16 changes](https://gcc.gnu.org/gcc-16/changes.html)、
[Clang Standard C++ Modules](https://clang.llvm.org/docs/StandardCPlusPlusModules.html)、
[MSVC /scanDependencies](https://learn.microsoft.com/en-us/cpp/build/reference/scandependencies)、
[microsoft/ifc-spec](https://github.com/microsoft/ifc-spec)。

---

## 3. 构建系统现状

| 构建系统 | 模块支持 | `import std` | 模块图如何表达 | 对 IDE 的输出 |
|---|---|---|---|---|
| CMake | `FILE_SET CXX_MODULES`，3.28 起稳定；仅 Ninja 与 VS 生成器 | `CMAKE_CXX_MODULE_STD` 自 3.30 起实验性，4.4 仍需实验门控 UUID，UUID 已变更 6 次以上 | `.ddi`（P1689）经 collate 生成按编译器区分的 `.modmap` 响应文件 | `compile_commands.json` 不含模块图；GCC 条目带 `-fmodules-ts -fmodule-mapper= -fdeps-format=`（实测）；`CMAKE_EXPORT_BUILD_DATABASE`（P2977）自 3.31 起实验性，仅 Ninja（实测 4.4.2） |
| mcpp | 自研行级扫描器为主，ninja dyndep 路径用 GCC `-fdeps` 或 `clang-scan-deps`；MSVC 扫描未实现 | 三家均支持，std BMI 按工具链身份全局缓存在 `$MCPP_HOME` | 内部 `Graph`/`SourceUnit`，无机器可读导出 | 只写 `compile_commands.json`；LLVM 条目带绝对路径 `-fmodule-file=std=`、`-fprebuilt-module-path=`；GCC 条目只有 `-fmodules`（实测） |
| xmake | 原生支持三家编译器 | 支持，Clang + MSVC STL 有已知问题 | 自有扫描与 modmap | 导出的 `compile_commands.json` 对模块参数处理有多个 issue（#5830、#4330） |
| build2 | 0.17.0（2024-06）自称三家完整支持 | 自称支持 | 自有依赖提取 | 与 clangd 配合长期有摩擦（clangd#758），未核实最新状态 |
| Meson | 1.10（2025-12）实验性支持 `import std`，全工程共享一个 std 模块 | 实验性 | 内部 dyndep | 无模块专用输出 |
| MSBuild | `.ixx` 自动识别，`ScanSourceForModuleDependencies`（17.6+） | `BuildStlModules` | 内部 P1689 | 无独立模块图产物 |
| Bazel | 9.0（2026-01）实验性 `--experimental_cpp20_modules`，每个 target 仅一个接口 | 未核实 | `.ddi` → `aggregate-ddi` → `.CXXModules.json` → `.modmap` | 需要 hedron 等第三方生成 `compile_commands.json` |

结论：没有任何一个构建系统的 IDE 输出能同时满足“跨编译器可用 + 带模块图 + 带 std 模块来源”。
CMake 的 P2977 构建数据库最接近，但仍是实验特性，也没有 IDE 消费者。

---

## 4. 标准与规范版图

| 规范 | 最新状态（2026-09） | 内容要点 | 对本项目的意义 |
|---|---|---|---|
| **P1689R5** 源文件依赖格式 | 2022-06 定稿，事实标准；尚未移植到 EcoStd | `rules[]` 内 `provides`/`requires`，字段 `logical-name`、`source-path`、`compiled-module-path`、`is-interface`、`lookup-method` | 依赖扫描层直接复用；需容忍 GCC `version: 0` 的差异 |
| **P2977R2** 构建数据库文件 | **提案，未进入任何标准。** 2024-03 SG15 看过 R1 后投票“有意把 build-database.json 纳入 Ecosystem IS”（强烈赞成 4、赞成 3、中立 0、反对 0、强烈反对 0）；2024-10 发布 R2 后无新版本；2024-12 Ecosystem IS 被撤回，P2977 失去原定载体；截至 2026-09 未移植到 EcoStd；论文跟踪 issue [cplusplus/papers#1702](https://github.com/cplusplus/papers/issues/1702) 仍为 open。实现只有 CMake 3.31+ 的实验特性，IDE 引擎尚无消费者 | `sets[]`（`name`、`family-name`、`visible-sets`、`baseline-arguments`）与 `translation-units[]`（`source`、`arguments`、`local-arguments`、`provides`、`requires`、`private`、`work-directory`） | 最接近“带模块图的编译数据库”。统一描述规范应参考并保持字段兼容，但不能依赖它的标准化进度 |
| **P3286R0** 预构建库模块元数据 | 2024 年 R0 后停滞；已由 Kitware 与 Bloomberg 作者移植为 **EcoStd RFC #3**（2025-10 起 active） | `modules[]`：`logical-name`、`source-path`、`is-std-library`、`local-arguments` | libc++ 与 libstdc++ 已按此形状发布清单（实测）；std 与外部库模块发现直接复用 |
| **P2717** Tool Introspection | WG21 R6 撤回；移植为 **EcoStd RFC #2**（2025-03 起 active） | 工具自描述能力的查询约定 | 工具链探测层对齐 |
| **P2581** BMI 互操作性说明 | R2 | 只提供“判断某 BMI 能否直接使用”的方法，不定义可移植格式 | 佐证“IDE 不应依赖构建 BMI” |
| **P1184** Module Mapper | R2（2020），GCC 通过 libcody 实现，仍称实验性 | 编译器与构建系统之间的逐行协议 | 仅作为 GCC 构建输入被解析，不在 IDE 契约中出现 |
| **C++ Ecosystem IS**（P2656、P3342） | P2656R4 于 2024-12 撤回，WG21 仓库归档，工作迁往 **EcoStd** | 原计划统一工具生态规范 | 行业规范的实际推进场所已变为 EcoStd |
| **EcoStd** | 独立组织，RFC 流程已定义（fork → PR → 标准措辞 → 审议组投票）；标准草案仍是骨架；审议组尚未组建 | 目前仅有 3 个 RFC：流程、Tool Introspection、Module Metadata | 本项目规范的提交目标；P1689 与 P2977 尚无人移植，存在空位 |
| **CPS**（Common Package Specification） | schema v0.15.0；CMake 4.3（2026-03）支持导入与导出 | 模块描述只引用一个 `cpp_module_metadata` 文件，即 P3286 文件 | 包管理层对齐 CPS，模块层不与之竞争 |
| **C++26 模块相关** | P3034R1（模块声明不能是宏）、P3618R0、P3868R1 已并入 | 细节修正，无新语义 | 扫描器需按 P3034 简化；允许模块声明前出现 `#line` |
| **文件扩展名** | 无标准；P1634 仅为命名建议 | `.cppm`、`.ixx`、`.ccm`、`.cxxm` 并存 | 规范中必须显式携带单元角色，不能靠扩展名推断 |
| **LSP** | 3.18 为当前版本，3.19 在推进 | 无模块相关扩展 | 模块特性需以自定义请求补充 |
| **BSP**（Build Server Protocol） | 2.1.x 稳定，2.2 仍为里程碑版本 | 自带 `cpp` 扩展仅为扁平选项包，维护者称“基本没人用” | 长期可作为动态协议载体，但模块图需要重新设计 |

结论：行业已经有了分层的零件（P1689 管依赖边，P2977 管构建数据库，P3286 管外部模块元数据，CPS 管包），但没有一份文档把它们组合成 IDE 可以直接消费的统一描述，也没有参考实现与一致性测试。这正是本项目可以填补的空位。

主要来源：
[P1689R5](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p1689r5.html)、
[P2977R2](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2977r2.html)、
[P3286R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p3286r0.pdf)、
[P2656R4（撤回）](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2656r4.html)、
[EcoStd RFCs](https://github.com/ecostd/rfcs)、
[CPS schema](https://cps-org.github.io/cps/schema.html)、
[CMAKE_EXPORT_BUILD_DATABASE](https://cmake.org/cmake/help/latest/variable/CMAKE_EXPORT_BUILD_DATABASE.html)、
[LSP 3.18](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.18/specification/)、
[BSP cpp 扩展](https://build-server-protocol.github.io/docs/extensions/cpp)。

---

## 5. C++ IDE 引擎的模块支持现状

| 引擎 | 前端 | 模块信息来源 | 跨编译器能力 | 状态 |
|---|---|---|---|---|
| **clangd 22.x** | Clang（库） | 扫描 CDB 中所有文件；`--experimental-modules-support` | 仅 Clang 兼容命令；GCC 条目报未知参数（实测）；std 模块源不在 CDB 中则失败（实测） | 实验性；BMI 写在临时目录 `/tmp/clangd/module_files`，正常退出时清理，不跨会话复用（实测每次会话都重新构建 std） |
| **clangd 23.1**（2026-09-02 发布） | 同上 | 新增 `CompileCommandsProjectModules`，直接读取命令中的 `-fmodule-file=` 映射，与扫描后端组合为 `CompoundProjectModules` | 同上 | 持久化 BMI 缓存位于编译数据库所在目录的 `.cache/clangd/modules`，温启动复用 std（实测 E12）；模块接口有无法解析的导入时，该文件请求可能挂起（实测 E13） |
| **clangd main** | 同上 | 2026-09-02 合入“发现 CDB 之外已打开的模块文件”；讨论中的 RFC 希望减少对 CDB 的依赖 | 同上 | P2977 构建数据库尚未被 clangd 消费，仅在 2026-05 的双周会上被提及 |
| **MS C/C++（VS 与 VS Code cpptools）** | EDG 前端 | 消费 MSVC 构建产出的 IFC | 只支持 MSVC；GCC/Clang 模块基本不支持 | VS 2026 中仍标为实验性 |
| **CLion** | Nova 使用 ReSharper 引擎，但模块功能走内置 clangd | 自行扫描 `.ixx/.cppm/.mxx` 建名字映射 | 继承 clangd 限制；官方测试组合不含 GCC | 部分未核实 |
| **ReSharper C++** | 自研前端 | 自研 | 不依赖任何 BMI | 2022.3 起支持，2026.1 可自动补 `import` |
| **Qt Creator** | 内置 clangd 22.1 | 同 clangd | 同 clangd | 补全不稳定 |
| **ccls / KDevelop / SourceKit-LSP** | libclang / kdev-clang / sourcekitd | — | 不支持 C++20 named modules | — |
| **clice**（clice-io/clice） | Clang 库，部分移植 clangd 组件 | `compile_commands.json` + `clice.toml` 规则；两级扫描（快速词法扫描 + 按需精确扫描） | 按编译器家族探测命令（GCC 用 `-dumpmachine`/`-print-search-dirs`，MSVC 用 `--driver-mode=cl`）；持久化 PCM 缓存 | Apache-2.0，约 1.3k star，仅有 v0.1.x nightly，自称 beta |

规律：

1. 现有引擎分三类：消费构建编译器自己的 BMI（MSVC IntelliSense）、内嵌 clangd 并继承其限制（Qt Creator、CLion 模块部分）、完全自研前端（ReSharper C++）。
2. 没有任何引擎声称真正实现了多编译器 BMI 互通，所有可行路线都是“IDE 用自己的前端，从源码重建模块语义”。
3. clangd 的模块支持在 2026 年进展很快（持久化缓存、读取命令中的映射），但它的数据来源仍是 CDB，这决定了“把正确的数据喂给 clangd”是 lsp-mcpp 的核心价值。

主要来源：
[ChuanqiXu：clangd 模块支持说明（2025-12）](https://chuanqixu9.github.io/c++/2025/12/03/Clangd-support-for-Modules.en.html)、
[clangd ProjectModules.cpp](https://github.com/llvm/llvm-project/blob/main/clang-tools-extra/clangd/ProjectModules.cpp)、
[clangd ModulesBuilder.cpp](https://github.com/llvm/llvm-project/blob/main/clang-tools-extra/clangd/ModulesBuilder.cpp)、
[RFC：发现 CDB 之外的模块接口（2026-08）](https://discourse.llvm.org/t/rfc-clangd-discover-c-module-interfaces-missing-from-the-compilation-database/91448)、
[clangd#1293](https://github.com/clangd/clangd/issues/1293)、
[CLion C++20 modules](https://www.jetbrains.com/help/clion/support-for-c-20-modules.html)、
[clice](https://github.com/clice-io/clice) 与其 [设计文档](https://github.com/clice-io/clice/tree/main/docs/en/design)。

---

## 6. 业界 LSP 的“工程模型边界”设计

| Server | 语义前端 | 工程模型来源 | 协议 / 格式 | 增量引擎 | 依赖与 sysroot 描述 |
|---|---|---|---|---|---|
| rust-analyzer | 自研 | `cargo metadata` 或 `rust-project.json`；非 Cargo 工程可配置 discover 命令 | 静态 JSON 文件 + 子进程 JSONL 发现协议 | salsa | sysroot 作为 crate 图子树 |
| zls | 复用 Zig 编译器（版本锁定） | 用 build runner 执行真实 `build.zig` 并收集 BuildConfig | 子进程一次性 JSON | 较弱 | 随编译器版本提供 std |
| Eclipse JDT LS | 内嵌 ECJ 编译器 | Maven、Gradle importer；新版 Gradle 走独立 BSP 进程 | 进程内 API 或 BSP | ECJ 增量编译 | 由真实构建工具解析 classpath |
| gopls | 自研（go/types） | `go list`，或外部 `GOPACKAGESDRIVER` | stdin/stdout JSON 请求与响应 | Snapshot 记忆化 | `ExportFile` 类似 BMI 引用 |
| SourceKit-LSP | 复用 sourcekitd 与 clang | SwiftPM、CDB 或 BSP | BSP + 自定义扩展 `textDocument/sourceKitOptions`、`buildTarget/prepare` | 显式区分 prepare 与 compile | prepare 先产出被导入模块的接口 |
| clangd | 复用 Clang（库） | `compile_commands.json`、`.clangd` | 静态文件 | preamble 缓存 + 后台索引 | `--query-driver` 执行真实 GCC 获取系统头 |
| Roslyn LSP | 复用 Roslyn | MSBuild design-time build，进程外 BuildHost | 进程内 API | 内存 Solution 图 | MSBuild 解析 SDK 与引用 |

可直接借鉴的经验：

1. **静态文件与动态发现协议并存。** 大多数工程只需静态文件；生成式或巨型仓库再走子进程发现协议（rust-analyzer、gopls、SourceKit-LSP 都是这样）。
2. **“准备”要成为一等动作。** SourceKit-LSP 的 `buildTarget/prepare` 只构建被导入模块的接口，不链接，可批量、可取消、需明确失败语义。这与“先构建 BMI 再提供语义”完全同构。
3. **工程模型是带产物引用的图，不是扁平的文件到参数表。** `compile_commands.json` 与 BSP `cpp` 扩展都停在扁平表，正是模块场景的缺口。
4. **发现协议用“子进程 + stdio JSON”，而不是库绑定。** 这是唯一在多厂商间真正普及的形式（rules_go、Bazel BSP、buck2）。
5. **工具链探测与逐文件参数分离。** `rustc --print sysroot` 与 `--query-driver` 都证明“询问编译器自身”可以独立标准化。
6. **(文件, 上下文) 是基本键。** 同一文件在不同 target 下参数不同，C++ 中非常普遍。
7. **隔离构建引擎进程。** rust-analyzer 的 proc-macro server、Roslyn 的 BuildHost、JDT LS 改用 BSP 都在避免把构建工具嵌进 LSP 进程。
8. **始终提供零配置降级层。** 没有构建信息时也要给出语法级功能，而不是整体失效。
9. **自研 C++ 全语义前端不可取。** 成功的自研前端都针对语义较简单的语言；C++ 场景下只有 ReSharper 这类长期商业投入做到。

主要来源：
[rust-analyzer 非 Cargo 工程](https://rust-analyzer.github.io/book/non_cargo_based_projects.html)、
[go/packages driver 协议](https://pkg.go.dev/golang.org/x/tools/go/packages#hdr-The_driver_protocol)、
[SourceKit-LSP BSP 扩展讨论](https://forums.swift.org/t/extending-functionality-of-build-server-protocol-with-sourcekit-lsp/74400)、
[BSP 规范](https://build-server-protocol.github.io/docs/specification)、
[clangd 配置](https://clangd.llvm.org/config.html)、
[Gradle Build Server](https://devblogs.microsoft.com/java/new-build-server-for-gradle/)。

---

## 7. mcpp 现状（作为首个数据提供方）

源码位置：`mcpp-community/mcpp`（版本 2026.9.13.1）。

| 方面 | 现状 | 位置 |
|---|---|---|
| 模块发现 | 自研行级扫描器，不处理 `#if` 包裹的 import，拒绝 header unit；可选 P1689 扫描后端（`MCPP_SCANNER=p1689`） | `src/modgraph/scanner.cppm`、`src/modgraph/p1689.cppm` |
| 模块图数据结构 | `SourceUnit`：`provides`、`providesInterface`（三态：接口 / 实现 / 未知）、`requires_`、逐单元参数；`Graph` 带拓扑排序 | `src/modgraph/graph.cppm` |
| BMI 规则 | `BmiTraits` 统一描述三家的目录、扩展名、参数拼写 | `modules/toolchain-model/src/model.cppm` |
| BMI 布局 | 工程内 `target/<triple>/<fingerprint>/{gcm,pcm,ifc}.cache`；跨工程 `$MCPP_HOME/build-cache/v1/pkg/...`；std 专用 `$MCPP_HOME/std/<key>/`（文档明确“不是接口”） | `src/bmi_cache.cppm`、`src/toolchain/stdmod.cppm` |
| 工具链指纹 | 11 个字段：编译器、版本、驱动、target、标准库、C++ 标准、参数哈希、mcpp 版本、锁文件哈希、std BMI 哈希、运行时绑定 | `modules/toolchain-model/src/fingerprint.cppm` |
| std 模块来源 | GCC 取 `bits/std.cc`；Clang 调 `-print-library-module-manifest-path`；MSVC 取 `modules/std.ixx` | `src/toolchain/{gcc,clang,msvc}.cppm` |
| Windows 上的工具链形态 | GCC 为 MinGW-w64（目标 `x86_64-windows-gnu`）；Clang 为 GNU 风格的 `clang++` 面向 `x86_64-pc-windows-msvc`，标准库为 MSVC STL，用 `-x c++-module` 编译 `std.ixx`；源码中未使用 clang-cl | `src/toolchain/clang.cppm`、`src/toolchain/provider.cppm`、`docs/20-toolchains.md` |
| IDE 输出 | 只生成 `compile_commands.json`；无模块图、无 P2977、无模块相关机器输出 kind | `src/build/compile_commands.cppm`、`docs/50-machine-output.md` |
| 依赖包的模块接口 | 永远以源码形式交付并由使用方编译，不分发 BMI | `docs/12-binary-distribution.md` |
| VS Code 插件 | 只配置 clangd 路径与 CDB 目录；GCC 工程提示切换 LLVM | `mcpp-vscode/src/extension.ts` |

对设计的启示：

1. mcpp 已经拥有统一描述所需的全部原始数据（模块图、单元角色、工具链身份、std 源位置），只缺一个标准化的导出。
2. mcpp 的 `BmiTraits` 与指纹字段可以直接映射到规范中的工具链描述与缓存键。
3. “依赖包模块接口一律以源码交付”与 IDE 从源码重建语义的思路天然一致。

---

## 8. 编辑器体验对标：其他语言在 VS Code 中的首次使用

调研日期 2026-09-14。扩展包体积为当天从 Marketplace 接口读取的实际值。

| 语言与扩展 | 用户还要装什么 | 语言服务器怎么来 | 首次打开的提示 | 工程发现 | 常驻界面 |
|---|---|---|---|---|---|
| Rust：rust-analyzer | rustup 与工具链，扩展会补装 `rust-src` | 按平台内置在扩展包，15–19 MB | 通常没有；平台不匹配时询问下载 | 打开含 `Cargo.toml` 的目录即可 | 状态栏进度，最近改为只在打开 Rust 文件时显示 |
| Go：vscode-go | Go 工具链 | 首次通过 `go install` 获取 gopls | 曾经在无关场景弹出“缺少分析工具”，后改为打开 Go 文件时才检查 | `go.mod` | 状态栏 |
| TypeScript：内置 tsserver | 无 | 随 VS Code 内置 | 无 | `tsconfig.json` 与 `node_modules` | 语言状态项 |
| Python：Python + Pylance | 解释器 | Pylance 内置，28 MB 通用包 | 找不到解释器时提示选择 | 自动发现虚拟环境与 conda | 可固定的语言状态项 |
| Java：Red Hat Java | 无，扩展内置 JRE 21 | 按平台内置，126–132 MB | 无，进度显示在状态文本 | `pom.xml`、`build.gradle` | 语言状态项 |
| C#：C# Dev Kit | .NET SDK；配套扩展自动安装 | 服务端内置，另由 .NET Install Tool 下载私有运行时 | 配套扩展安装通知 | `.sln` 自动打开或选择 | 解决方案资源管理器与状态栏 |
| Swift：Swift 扩展 | Swift 工具链，2025 年起可在编辑器内经 Swiftly 安装与切换 | 随工具链 | `.swift-version` 触发切换提示 | 自动发现已装工具链 | 状态栏版本选择 |
| Zig：vscode-zig | 无 | 扩展下载 zig 与 zls | 首次使用时下载 | `build.zig` | 很少 |
| C/C++：微软 C/C++ 扩展 | 编译器；CMake 工程还需 CMake Tools | 按平台内置，Linux 包约 134 MB | 需要配置 `compilerPath` 或选择 kit，状态栏常驻“未选择 kit” | 依赖配置文件或 CMake Tools | 状态栏多个项 |
| C/C++：clangd 扩展 | clangd，扩展可下载 | 下载，需要确认 | 下载提示；检测到微软 C/C++ 扩展时提示冲突 | 依赖 `compile_commands.json` | 无专门界面 |

### 8.1 可以直接采用的做法

1. **按平台发布完整扩展包。** Marketplace 没有公开的体积上限，微软 C/C++ 扩展约 134 MB、Red Hat Java 约 127 MB 已是先例。Open VSX 有社区报告的 512 MB 上传限制，未见官方文档。
2. **完整内置才能做到零提示。** rust-analyzer 与微软 C/C++ 扩展内置服务端，不需要首次下载；clangd 扩展、CodeLLDB 新版、vscode-zig 都需要首次下载与失败处理。
3. **状态用语言状态项，不占常驻状态栏。** TypeScript、Python、Java 都已迁移到 `languages.createLanguageStatusItem`，它只在打开对应语言文件时出现，支持忙碌、警告、错误三种状态与一个命令。
4. **懒激活。** 只在打开 C++ 文件或工作区含工程标志时激活，不用 `onStartupFinished`。
5. **不自动打开引导页。** rust-analyzer、gopls、tsserver 都没有首次引导页。
6. **主动处理扩展冲突。** clangd 扩展的 `clangd.detectExtensionConflicts` 会检测微软 C/C++ 扩展并提议关闭其智能感知，这是现成先例；关闭方式是把 `C_Cpp.intelliSenseEngine` 等设置为 `disabled`。
7. **明确声明工作区信任能力。** 在 `package.json` 中把 `capabilities.untrustedWorkspaces` 声明为 `limited`，受限模式下不执行外部程序。
8. **内置的基线服务端与项目真实工具链分开。** Rust 的扩展内置 rust-analyzer，rustup 负责项目工具链；对应到本项目，扩展内置 clangd 与语义工具包作为基线，xlings 负责用户真实的编译器。
9. **保持 Open VSX 兼容。** 微软 C/C++、Pylance、C# Dev Kit 因许可限制不在 Open VSX；lsp-mcpp 使用开源许可，可以覆盖 VSCodium、Cursor 等编辑器。

### 8.2 主要来源

- [VS Code 发布扩展与平台包](https://code.visualstudio.com/api/working-with-extensions/publishing-extension)
- [Marketplace 体积上限询问（无公开数字）](https://github.com/microsoft/vsmarketplace/issues/1541)
- [Open VSX 上传限制报告](https://github.com/EclipseFdn/open-vsx.org/issues/2300)
- [VS Code API：Language Status Item](https://code.visualstudio.com/api/references/vscode-api)
- [工作区信任扩展指南](https://code.visualstudio.com/api/extension-guides/workspace-trust)
- [通知与状态栏 UX 指南](https://code.visualstudio.com/api/ux-guidelines/notifications)
- [rust-analyzer VS Code 说明](https://rust-analyzer.github.io/book/vs_code.html)
- [vscode-go 延迟工具检查](https://github.com/golang/vscode-go/issues/3038)
- [Red Hat Java 迁移到语言状态项](https://github.com/redhat-developer/vscode-java/pull/2363)
- [C# Dev Kit FAQ](https://code.visualstudio.com/docs/csharp/cs-dev-kit-faq)
- [Swiftly 集成进 VS Code](https://www.swift.org/blog/gsoc-2025-showcase-swiftly-support-in-vscode/)
- [vscode-clangd 冲突检测](https://github.com/clangd/vscode-clangd/pull/141)

## 9. 调研结论汇总

1. **跨编译器的 IDE 契约只能是“源码 + 模块图 + 语义选项”，不能是 BMI。** 三家 BMI 互不可读，且都绑定版本，行业共识（P2581、GCC 文档）也是如此。
2. **语义引擎应当是 IDE 自己控制的单一前端**，由它从源码重建 BMI。现实可选项是 clangd（生态最大、进展快）或 clice（设计更激进、尚未稳定）。
3. **缺口不在前端，而在数据。** clangd 已经能处理模块，但喂给它的 CDB 在 GCC/MSVC 场景下是错误的，在 Clang 场景下也会让它读取陈旧的构建 BMI（实测）。
4. **标准零件齐全但未组合。** P1689、P2977、P3286、CPS 各管一层，EcoStd 已成为实际推进场所，P2977 与 P1689 的移植仍是空位。
5. **架构上应借鉴 rust-analyzer 与 SourceKit-LSP**：静态描述加动态发现，准备动作一等化，(文件, 上下文) 为键，可降级。
