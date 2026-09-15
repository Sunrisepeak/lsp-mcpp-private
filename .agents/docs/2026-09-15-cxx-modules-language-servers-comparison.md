# C++ 模块语言服务方案深度对比：lsp-mcpp、clangd、clice 与主流开源、闭源方案

日期：2026-09-15
关联文档：
- 调研综述：[2026-09-13-cxx-modules-landscape-research.md](2026-09-13-cxx-modules-landscape-research.md)（本文更新了其中第 5 节的引擎现状）
- 设计方案：[2026-09-13-cxx-modules-unified-lsp-design.md](2026-09-13-cxx-modules-unified-lsp-design.md)
- 第一版可用方案与执行记录：[2026-09-14-lsp-mcpp-v1-usable-plan.md](2026-09-14-lsp-mcpp-v1-usable-plan.md)

> 标注说明：
> - **实测**：本文第 6 节在同一台开发机上的对照运行，或 lsp-mcpp 的 CI 与 nightly 运行结果。
> - **文档**：官方文档、发布说明、源码或 issue 原文，来源列在附录 A。
> - **未核实**：只有二手来源，或无法取得原文。
>
> 数据截至 2026-09-15。报告作者同时是 lsp-mcpp 的实现者；对 lsp-mcpp 的判断以实测为主，但实测夹具与运行器也出自本项目，第 9 节列出了这一偏向。

---

## 0. 结论先行

1. **目前没有方案能在“三大编译器 × 三个平台 × 两类工程形态 × 零配置”上完整可用，差距主要不在语义前端，而在交给前端的数据。** 构建系统给出的编译数据库不含 `std` 源码，带着只对构建编译器有效的 BMI 参数，GCC、MSVC 的命令 Clang 前端也执行不了。实测中原生 clangd 23.1 与 clice nightly 在 mcpp 工程上都因此失败。把 lsp-mcpp 规范化后的数据库交给它们，同一工程的核心导航随即可用。
2. **核心维度：全 `.cppm` 工程与接口/实现分离工程。** lsp-mcpp 在两类工程上都零配置通过全部检查：接口/实现分离工程 23 项，全 `.cppm` 工程 18 项（含 initialize、状态与工作区检查）；覆盖 LLVM、GCC、MSVC 三种工具链，三个主机逐次 CI，本地连续 5 轮共 15 次运行无失败。原生 clangd 需要正确的数据加额外参数，才能通过接口/实现分离工程 20 项中的 17 项；clice 在同样的数据上只通过 10 项：没有推送诊断，在实现单元里针对实现分区的跳转、悬停与补全失败，引用结果缺少声明所在文件。
3. **clangd** 是开源生态的共同底座，2026 年进展最快：持久化 BMI 缓存、同名模块、编译数据库之外的发现、`import` 上跳转。但它仍需 `--experimental-modules-support`，没有默认开启的计划，模块相关 open issue 共 73 个。数据不对时常表现为请求无应答（实测 45 秒超时）。
4. **clice** 在工程设计上比 clangd 更适合 IDE：多进程隔离、未解析导入不挂起、按配置隔离 PCM 缓存，并提供 6 个平台目标。但它从未发布稳定版，模块支持自评为部分完成；实测中不推送诊断，默认把缓存写进工作区。
5. **闭源方案里**：Visual Studio 与 VS Code C/C++ 扩展的模块 IntelliSense 七年仍标实验性，只服务 MSVC；CLion 以自有扫描加内置 clangd 提供模块功能，官方已测组合不含 GCC；ReSharper C++、Visual Assist 各自支持模块但只在 Visual Studio 中；SonarQube 的模块分析仍为实验，CodeQL 明确不支持；Xcode 没有 C++20 命名模块的文档。
6. **构建系统侧**：除 mcpp 的 S1 输出外，没有构建系统能同时给出“模块图 + `std` 源码 + 与前端无关的命令”。CMake 的构建数据库仍为实验特性且没有消费者，P2977 自 2024-10 以来没有进展。
7. **lsp-mcpp 的路线是合理的，上限受引擎约束。** 它是目前唯一在 P1–P7 工具链组合与无编译器机器上端到端验证过的开源方案。短板包括：clangd 的实验性与缺陷直接成为它的上限；GCC、MSVC 工程得到的是 Clang 语义；负载只有三个平台；只打包了 VS Code；项目尚未发布。
8. **建议**（第 8 节）：引擎层保持可替换，把 clice 列为第二引擎候选；向 clangd、clice 上游提交实测发现的问题；推动 S1/S2 进入 EcoStd；补齐 arm64 负载与其他编辑器；重新评估是否发布预览版。

---

## 1. 范围、维度与方法

### 1.1 对比对象

| 类别 | 方案 | 语义引擎 | 许可 |
|---|---|---|---|
| 本项目 | lsp-mcpp（PR #1，0.1.0 未发布） | 内置 clangd 23.1.0 + 规范化层 | Apache-2.0 |
| 开源语言服务器 | clangd 23.1.0 / 23.1.1 | Clang | Apache-2.0 WITH LLVM-exception |
| 开源语言服务器 | clice v0.1.2026091407（nightly） | Clang 22.1.8（带补丁） | Apache-2.0 |
| 开源编辑器与 IDE | Qt Creator 20、Eclipse CDT LSP、GNOME Builder、Zed、Neovim、Helix、Emacs、Sublime、Kate、Code::Blocks | 均转交 clangd | 多为 GPL/MIT |
| 开源其他 | ccls、KDevelop、Eclipse CDT 经典索引器、Sourcetrail 分支、Doxygen、clang-uml、clang-tidy、Kythe、scip-clang | 各自 | 各自 |
| 闭源 IDE | Visual Studio 2022/2026、VS Code C/C++ 扩展（cpptools）、CLion 2026.2、ReSharper C++、Rider、Visual Assist、C++Builder 13、Xcode 26/27、Cursor、Windsurf | EDG、JetBrains 自研 + clangd、Visual Assist 自有解析器、Apple Clang 等 | 商业或专有 |
| 闭源分析工具 | SonarQube CFamily、PVS-Studio、CodeQL、Understand、Coverity、Klocwork 等 | 各自 | 商业 |

### 1.2 评判维度

| 维度 | 含义 | 权重 |
|---|---|---|
| **工程形态** | 全 `.cppm` 工程（模块单元全部为 `.cppm`，实现写在接口内）与接口/实现分离工程（接口在 `.cppm`，实现单元 `module M;` 在 `.cpp`，含实现分区）都能正确导航、补全、诊断 | 核心 |
| **稳定性** | 成熟度标注、已知崩溃与挂起、测试覆盖、重复运行的一致性 | 核心 |
| 跨平台 | 主机平台 × 编译器家族 × 标准库 | 高 |
| 易用性 | 用户必须做的配置、是否写工程目录、失败时是否说明原因 | 高 |
| 技术路线合理性 | 模块语义从哪里来，结构性限制是什么 | 高 |
| 性能 | 冷启动与首次跳转 | 中 |
| 开放性 | 许可、规范、可复用 | 中 |

### 1.3 实测方法

- **环境**：开发机 i9-13900K，Linux 6.8；mcpp 2026.9.15.1，构建编译器 LLVM 22.1.8 与 GCC 16.1.0；clangd 23.1.0（clangd/clangd 发布，与 lsp-mcpp 负载中的相同）；clice 0.1.2026091407（官方 Linux x86_64 nightly）；lsp-mcpp 为 PR #1 的 dev 构建。
- **运行器与检查**：同一个 `lsp-mcpp-conformance` 运行器，同一批检查。原生 clangd 与 clice 通过包装脚本接入：忽略运行器传给 lsp-mcpp 的参数，按用户的常规方式启动。
  - clangd：`--experimental-modules-support`，另有一组加 `--compile-commands-dir`。
  - clice：`clice serve`。
- **数据准备**：
  - 原生 clangd 与 clice 由准备步骤生成编译数据库，这是用户能做的，例如 `mcpp build --configure-only`、`mcpp build`、`mcpp emit build-database --spec compile-commands`、CMake 配置并构建。
  - 对照组“规范化数据库”由 `lsp-mcpp model --export engine` 生成。
  - lsp-mcpp 不做任何准备。
- **时限与计数**：每项检查限时 20–45 秒，冷启动。统计只计标准 LSP 检查（诊断、跳转、声明、悬停、补全、引用，编号 C*）。lsp-mcpp 专有的模块层检查（模块名悬停文字、模块大纲、`unresolved-module` 诊断码，编号 M*）天然偏向 lsp-mcpp，单独列出，不计入对比。
- **CI 数据**：lsp-mcpp 另有三主机 CI（run 34894725998）与 nightly（run 34882966882）。

---

## 2. 核心维度：两类工程形态与稳定性

### 2.1 两类形态各自的难点

| 形态 | 结构 | 语言服务要处理的问题 |
|---|---|---|
| 全 `.cppm` | 主接口单元重导出多个接口分区与另一个模块，函数实现写在接口里，入口 `main.cpp` 是非模块单元 | 沿 `export import :partition;` 与 `export import other.module;` 的重导出链解析；分区跳转；BMI 依赖链较长，接口改动要传播到所有导入方 |
| 接口/实现分离 | `greet.cppm` 只有声明，`greet.cpp` 与 `format.cpp` 是同一模块的两个实现单元（`module hello.greet;`），`format.cppm` 是被主接口重导出的接口分区，`counter.cppm` 是实现分区（`module hello.greet:counter;`，不导出、可被模块内导入） | 实现单元隐式导入主接口，本身不产出 BMI；实现分区是可导入单元，不能按扩展名判断角色；“声明在接口、定义在实现”要求跳转能跨到 `.cpp`；实现单元内要能补全模块内部的非导出名字 |

两类工程的代码已由 mcpp 2026.9.15.1 在 LLVM 22.1.8 与 GCC 16.1.0 上实际构建运行（实测）。mcpp 给出的 S1 角色分别为 `module-interface`、`module-partition-interface`、`module-implementation`、`module-partition-implementation`，都通过了 S1 校验（实测）。

### 2.2 实测矩阵

标准检查通过数（C*）。接口/实现分离工程共 20 项，全 `.cppm` 工程共 15 项。lsp-mcpp 的夹具另含 initialize、状态与工作区三项，所以运行器输出的总数是 23 与 18。

| 方案与数据来源 | 接口/实现分离（LLVM） | 接口/实现分离（GCC） | 全 `.cppm`（LLVM） | 工程目录写入 |
|---|---|---|---|---|
| **lsp-mcpp，零配置** | **20/20** | **20/20** | **15/15** | 无 |
| clangd 23.1，用户用 `mcpp build --configure-only` 生成数据库 | 1/20：数据库指向 clang 22.1.8 构建的 `std.pcm`，报 “older format”，随后请求无应答；唯一通过的是不导入 `std` 的实现分区的诊断 | 未单独测（同 mcpp-gcc：0/10） | 0/15：同左 | 数据库、`target/`、`.cache/clangd` |
| clangd 23.1，lsp-mcpp 规范化数据库 + `--compile-commands-dir` | 17/20 | — | 11/15 | `.cache/clangd` |
| clice nightly，用户用 `mcpp build --configure-only` 生成数据库 | 0/20：`module 'std' not found`，跳过编译，不发布诊断 | 未单独测（同 mcpp-gcc：0/10） | 2/15：同左，只有重导出模块名上的跳转与一处悬停通过 | 数据库、`target/`、`.clice/` |
| clice nightly，lsp-mcpp 规范化数据库 | 10/20 | — | 12/15 | `.clice/` |

未通过项：

| 方案 | 形态 | 未通过的检查 | 说明 |
|---|---|---|---|
| clangd 23.1 + 规范化数据库 | 分离 | `import hello.greet` 上跳转、`export import :format` 上跳转、未保存编辑传播到导入方 | 23.1 不支持在 `import` 上跳转，返回了错误的位置；main 分支 2026-09-04 起支持。lsp-mcpp 由自己的模块索引回答这类请求，并把接口的未保存编辑传播给导入方 |
| clangd 23.1 + 规范化数据库 | 全 `.cppm` | 三处 `import`/`export import` 上跳转、未保存编辑传播 | 同上 |
| clice + 规范化数据库 | 分离 | 4 个文件均未推送诊断；实现单元内跳转到实现分区、悬停、补全 `detail::`；未保存编辑传播；引用缺少声明与定义所在文件；测试文件中跳转 | 失败集中在实现单元里使用实现分区 `:counter` 的位置（原因未查明）；引用请求带 `includeDeclaration` 时，结果缺少声明与定义所在文件 |
| clice + 规范化数据库 | 全 `.cppm` | 2 个文件未推送诊断；未保存编辑传播 | 导航、重导出链、补全全部通过 |

### 2.3 lsp-mcpp 的稳定性证据

| 证据 | 内容 | 结果 |
|---|---|---|
| 逐次 CI，三主机 | `mcpp-split`（Linux、macOS、Windows）、`mcpp-split-gcc`（Linux）、`mcpp-split-msvc`（Windows，`msvc@system`，服务端不带开发者环境）、`mcpp-all-cppm`（三主机） | run 34894725998 全部通过（实测） |
| 重复运行 | 本地 `mcpp-split`、`mcpp-split-gcc`、`mcpp-all-cppm` 连续 5 轮 | 15 次运行无失败，单次 11–26 秒（实测） |
| 生产方数据 | CI 每个主机校验 mcpp 为这些夹具输出的 S1 文档（Schema、语义规则、#636 契约） | 通过（实测） |
| 真实仓库，分离形态 | nightly `self-lsp-mcpp`：lsp-mcpp 仓库的固定提交 28ecd6e，75 个 `.cppm` 接口与 78 个 `.cpp`（以实现单元为主），依赖包提供 `std` | Linux、macOS 通过（nightly 34882966882） |
| 真实仓库，全 `.cppm` 形态 | nightly `self-mcpp`：mcpp 仓库的固定提交 b8d9684，主包 `src/` 为 120 个 `.cppm` 加 `main.cpp`，约 170 个模块 | Linux 通过（同上） |
| 整体覆盖 | 36 个一致性夹具覆盖 P1–P7 与 K1–K3；18 个 CI 任务；S1–S4 共 144 条规则有证据 | CI 持续通过 |

仍需说明的边界：两类形态的逐次 CI 夹具是小工程；大工程只在 nightly 覆盖，且 Windows 上没有自举（lsp-mcpp 在 Windows 需要 `--target x86_64-windows-gnu`，编辑器还不能把目标传给 mcpp）；header unit 不在范围内。

### 2.4 其他方案对两类形态的公开信息

| 方案 | 全 `.cppm` | 接口/实现分离 | 依据 |
|---|---|---|---|
| clangd | 数据正确时可用（实测）；`import` 上跳转已在 main 分支支持，尚未发布 | 数据正确时可用（实测）；用户报告跨模块重命名与引用不完整（clangd#2569） | 实测；clangd/clangd#2569 |
| clice | 导航与补全可用（实测） | 实现单元内对实现分区的处理失败（实测）；文档自述接口到实现只支持单向跳转 | 实测；`docs/en/features/navigation.md` |
| CLion | 按扩展名（`.ixx`、`.cppm`、`.mxx`）加 `export module` 识别接口单元并建立模块名映射，交给内置 clangd | 实现单元是普通 `.cpp`，是否参与模块内补全与跳转，文档未说明（未核实） | CLion 帮助文档（2026-09-09） |
| Visual Studio / cpptools | 实验性，官方已知问题“导入模块后 IntelliSense 可能停止工作” | 同左，未见专门说明 | Microsoft Learn |
| ReSharper C++、Visual Assist | 声称支持模块导入、查找引用、导航 | 未见专门说明（未核实） | 各自发布说明 |
| 其他 clangd 编辑器 | 等于用户配置的 clangd | 同左 | — |

---

## 3. 技术路线：模块语义从哪里来

模块把 C++ 工具链里原本分散的物理细节集中暴露给了 IDE：
- 接口单元要先编译成 BMI，导入方才能分析；
- BMI 与编译器版本、部分选项绑定，三家互不可读；
- `import std` 的源码位置由“编译器 + 标准库”二元组决定；
- 构建系统给 IDE 的编译数据库既没有模块图，又常带着只对构建编译器有效的 BMI 参数。

各方案的差别，首先是“模块语义从哪里来”这一选择。

| 路线 | 代表 | 模块语义的来源 | 对构建数据的要求 | 跨编译器能力 | 主要风险 |
|---|---|---|---|---|---|
| R1 独立的 IDE 前端 | Visual Studio IntelliSense、VS Code C/C++ | EDG 前端自行解析；构建另由 MSVC 产出 IFC | MSBuild 工程或 MSVC 参数 | 只面向 MSVC | IDE 前端与编译器语义分叉；模块 IntelliSense 长期实验性 |
| R2 自研非编译器前端 | ReSharper C++、Rider、Visual Assist | 自研解析器直接从源码理解模块，不需要 BMI | IDE 工程模型 | 与编译器无关，但只在各自宿主 IDE 中 | 闭源、商业；追赶标准需要长期大投入 |
| R3 编译器库前端，从源码自建 BMI | clangd、clice；内嵌 clangd 的 Qt Creator、Zed、Neovim 等；CLion 的模块功能（自有扫描 + 内置 clangd） | Clang 前端按编译数据库中的命令构建自己的 BMI | 数据库必须列出全部模块单元（含 `std`），命令必须是 Clang 能执行的 | 只有 Clang 语义；GCC、MSVC 工程取决于命令能否被 Clang 接受 | 数据不对就整体失效；BMI 构建与校验开销；前端自身的实验性缺陷 |
| R4 数据规范化层 + R3 引擎 | lsp-mcpp | 与 R3 相同（内置 clangd 23.1），交给引擎的数据由本层从构建描述重建 | 能读 S1 构建数据库、mcpp、CMake、编译数据库，或者只有源码 | GCC、MinGW、Clang、clang++（MSVC ABI）、clang-cl、cl.exe 的命令翻译为 Clang 命令；`std` 按标准库清单或依赖包单元注入 | 两层叠加的复杂度；上限受引擎约束；Clang 语义与 GCC、MSVC 的差异仍在 |

### 3.1 各路线是否合理

**R1 的问题出在结构上。** IntelliSense 用的 EDG 前端与 MSVC 编译器是两套实现，模块语义要在两边各做一遍。微软维护者在 cpptools #13987（2025-11）中说明，模块支持取决于 EDG 的排期；Visual Studio 2026 18.10 仍提示模块 IntelliSense 为实验性。这条路线在 MSVC 专属场景内合理，但无法成为跨编译器方案。

**R2 在技术上可行，在生态上封闭。** 自研前端不依赖 BMI，天然与编译器无关。代价是只有长期商业投入才能维持，而且只存在于各自的 IDE 中。JetBrains 在 CLion 的模块功能上也借助了内置 clangd，而不是完全依靠自研前端。

**R3 是开源世界的主流，语义来源正确。** 语义直接来自真实编译器前端，BMI 由 IDE 自己构建、自己缓存，不读构建产物；P2581 与 GCC 文档都不建议复用构建 BMI。它的软肋是“数据”：编译数据库必须完整，命令必须能被 Clang 执行，`std` 源码必须在库里。clangd 维护者 Chuanqi Xu 在 2025-12 的文章中，把“同一模块多套配置”“同名模块”列为需要 P2977 这类更好的构建数据库才能解决的问题。clice 在 R3 内部做了更好的工程设计，但数据问题同样存在。

**R4 把数据问题独立成一层。** lsp-mcpp 不自研前端，而是把 R3 的数据前提变成可规范、可测试的一层：
- **规范：** S1 描述工程模型，S2 描述发现协议，S3 描述编辑器扩展，S4 描述无编译器时的语义工具包，并以 144 条规则与一致性夹具约束实现。
- **引擎：** 引擎位于 L4 适配层之后，可以替换。

它是否合理取决于两点：一是规范化层能否覆盖真实工程的多样性，第 2 节与第 6 节给出了当前覆盖范围；二是引擎的上限，clangd 的实验性与缺陷会直接成为 lsp-mcpp 的上限。

### 3.2 为什么模块让“数据”成为主要矛盾

实测中，原生 clangd 23.1 与 clice 在 mcpp 工程上失败，原因都与前端能力无关：

1. **`std` 不在编译数据库里。** mcpp 与多数构建系统在编译数据库之外构建 `std`。两个前端都报 `module 'std' not found`；clice 日志为 `BuildPCM failed ... module 'std' not found`，随后“跳过编译”。
2. **BMI 参数只对构建编译器有效。** mcpp 的编译数据库带 `-fmodule-file=std=<构建产出的 std.pcm>`。clangd 23.1 读取这个映射后报告该 BMI 由 clang 22.1.8 产出，“uses an older format that is no longer supported”。改用 `mcpp emit build-database --spec compile-commands` 生成的数据库时，映射指向尚未构建的缓存路径，报 “not found”。
3. **编译数据库的查找范围。** 把 `std.cppm` 加进工程的编译数据库后，clangd 仍报 `module 'std' not found`。原因是 `std.cppm` 位于工程目录之外，clangd 按文件所在目录查找数据库，日志为 “Failed to find compilation database for .../std.cppm”。加上 `--compile-commands-dir` 后才通过。
4. **GCC、MSVC 的参数。** GCC 的 `-fmodules -fmodule-mapper=`、MSVC 的 `/reference`、`/interface` 都不是 Clang 能执行的命令。clangd 不做翻译，clice 只做了驱动探测与 `--driver-mode=cl`。

这四点在 GCC、MSVC 工程与 Windows 主机上会叠加。lsp-mcpp 的 L2 与 L3 正是逐条处理：
- 读取生产方的模块图与标准库信息；
- 删除 BMI 参数；
- 按清单注入 `std`；
- 翻译方言；
- 以 `--compile-commands-dir` 与模块提示，把整个数据库交给 clangd。

---

## 4. 逐项分析

### 4.1 lsp-mcpp（本项目）

| 方面 | 现状 |
|---|---|
| 形态 | C++23 模块编写的 LSP 服务端：102 个 `.cppm`/`.cpp`，约 1.4 万行，无头文件与宏；另有 19 个测试程序。VS Code 扩展“C++ Modules”按平台内置 clangd 23.1.0 与语义工具包 `lsp-mcpp-kit` |
| 工程模型来源 | 用户指定的 S1 数据库；mcpp `emit build-database`（2026.9.15.1 起；旧版回退 `--configure-only`）；CMake 构建数据库或编译数据库（展开 `@modmap`）；普通编译数据库加扫描；只有源码时推断；Windows 上自动发现 Visual Studio |
| 规范化 | 方言翻译为 clangd 可执行的命令；删除 BMI 参数；按标准库清单或依赖包单元注入 `std`；写出模块提示，跳过 clangd 的全库扫描；有无法解析导入的单元不交给引擎，避免挂起；并行预构建模块 |
| 模块层功能 | 模块名跳转、悬停、大纲、`import` 补全；未解析、二义、越界分区诊断；模块图；多上下文切换（S3）；接口未保存编辑传播给导入方 |
| 平台 | Linux x64、macOS arm64、Windows x64；VSIX 31.3 / 28.1 / 36.4 MB |
| 验证 | 两类工程形态逐次三主机 CI（第 2.3 节）；36 个一致性夹具；18 个 CI 任务；VS Code 端到端测试断言零弹窗、零写入工程目录；nightly 计时、自举与最新 mcpp |
| 性能 | 见第 5.5 节与第 6.4 节 |

**优势：**
- 两类工程形态与已测工具链组合上零配置可用，不写工程目录；
- GCC、MSVC 工程与不带开发者环境的 Windows 主机也能跨模块导航；
- 数据契约有规范与一致性测试，mcpp 已按 S1 实现生产方，并在三个主机上闭环验证；
- 引擎可替换。

**短板与风险：**
- **上限受 clangd 约束。** clangd 的实验性缺陷只能绕过，例如 MSVC STL 上的 `align_val_t` 回归、未解析导入导致的挂起、大工程温启动时的串行 BMI 校验。负载固定在 clangd 23.1.0，修复回归的 23.1.1 没有可移植的构建。
- **Clang 语义的偏差。** GCC、MSVC 工程得到的是 Clang 语义，只被 GCC 或 MSVC 接受的代码会误报。S3 要求诊断标注语义来源，但无法消除差异。
- **覆盖面有限。** 负载只有三个平台；只打包了 VS Code，服务端可作为通用 LSP 使用，但其他编辑器未打包、未测试。
- **项目阶段。** 尚未发布，主要由一人开发；依赖 openkal 运行时链，第一版中向上游修复了 14 项缺陷，维护面较宽。
- **范围与规模。** header unit 不在范围内；大工程冷启动仍是分钟级（CI 上 mcpp 仓库首次跳转 90–125 秒）。

### 4.2 clangd（LLVM）

| 方面 | 现状 |
|---|---|
| 状态 | 仍需 `--experimental-modules-support`，默认关闭；官网写明 “still in experimental stage”；未找到默认开启的计划（`ClangdMain.cpp` 中 `init(false)`） |
| 演进 | 19（2024-09）引入实验支持；20（2025-03）增加模块符号补全与可复用的模块构建器；22（2026-02）尝试复用构建产出的合适 BMI。23.1（tag 2026-08-25，clangd/clangd 发布于 2026-09-02）首次单列模块章节：跨会话持久化 BMI 缓存（数据库旁的 `.cache/clangd/modules`）、从命令读取映射以支持同名模块、模块单元跳过 preamble、新增导入时失效 preamble。main 上 2026-09-02 可发现编译数据库之外已打开或被监视的模块文件，09-04 支持在 `import` 上跳转 |
| 数据前提 | 数据库必须列出全部可导入模块单元，包括 `std`；只接受 Clang 命令，没有 GCC、MSVC 参数翻译（clangd#2477、llvm-project#214657） |
| 已知问题 | llvm-project 中带 `clangd` 标签且涉及模块的 open issue 23 个，clangd/clangd 中 50 个。例如：构建失败后复用陈旧 PCM（#213499）；重建后仍用旧模块信息（#188919）；新增导入需要重建（#126350）；跨模块重命名与引用不完整（clangd#2569）；导入声明拖慢解析（#219730）；`import` 之后的模块名补全不完整（#221762）。另外，同一模块多套配置只能保留一份 BMI |
| 分发 | clangd/clangd 发布 Linux（glibc 2.18 起）、macOS、Windows 的 x64 构建，每个大版本只发 x.1.0 与一个较晚的补丁版，没有 23.1.1。LLVM 官方包有 Linux、Windows 的 x64 与 arm64，23.1.0 没有 macOS 包 |
| 维护 | 模块工作以 Chuanqi Xu 为主，2026 年新增 Berkay Sahin 等贡献者；2026 年第二季度提交最多 |

**评价：** 语义来源正确，2026 年进展是所有方案中最快的，也是整个开源生态的共同底座。短板集中在数据前提与失败方式：用户要自己保证数据库正确、完整、与 clangd 版本匹配，否则模块功能整体失效，且常表现为请求无应答。Windows 与 MSVC STL 仍是二等路径。

### 4.3 clice（clice-io/clice）

| 方面 | 现状 |
|---|---|
| 状态 | 自称 beta；22 个 tag 全是 v0.1.x nightly，从未发布稳定版；功能总览页以“模块支持”作为 Partial（关键子系统缺失）的示例 |
| 社区 | Apache-2.0；1,323 star，约 23–26 名贡献者，670 次提交；2026 年 7 月 64 次、8 月 47 次提交；85 个 open issue |
| 架构 | 固定 LLVM 22.1.8 预编译包，带少量补丁；多进程：主进程只做调度，有状态 worker 持有 AST，无状态 worker 构建 PCH、PCM 与索引，worker 崩溃可恢复；自研协程框架 kota；LMDB 索引；header context 与构建配置切换 |
| 命令处理 | 用 Clang `OptTable` 分类参数；GCC 通过 `-dumpmachine`、`-print-search-dirs` 探测后转换；MSVC 与 clang-cl 用 `--driver-mode=cl`；不识别 ccache、sccache |
| 模块（已实现） | 两级扫描；PCM 作为任务图节点缓存在自己的目录，保存时级联失效，检测环；分区；`import` 名跳转、跨模块跳转、悬停、模块名补全、语义高亮；未解析导入成为哨兵节点，不阻塞其他文件；按构建配置隔离 PCM |
| 模块（未实现或部分） | header unit 未实现；带点模块名跳转与接口到实现的反向跳转为部分支持；`import std` 没有针对三种标准库的专门处理 |
| 平台与分发 | 每个 nightly 有 Linux、macOS、Windows 的 x64 与 arm64 共 6 个二进制及对应 VSIX；VS Code 扩展在 Marketplace（347 次安装），不在 Open VSX；仓库内带 Neovim、Zed 集成；默认把缓存与日志写到工作区 `.clice/` |
| 稳定性 | CI 覆盖三主机单元、集成与编辑器端到端测试；open issue 中有 Windows lint 崩溃（#677）、索引挂起（#549）、初始化非法指令（#469）、Windows 管道模式访问冲突（#352） |

**评价：** 在 R3 路线内，clice 的崩溃隔离、未解析导入不挂起、按配置隔离缓存、6 个平台目标都比 clangd 更好。但它仍是 beta，模块支持自评为部分完成；数据前提与 clangd 相同。实测中它在 mcpp 工程上同样找不到 `std`，失败不以诊断告知用户，实现单元内对实现分区的处理不完整。

### 4.4 Visual Studio 与 VS Code C/C++ 扩展（EDG）

| 方面 | 现状 |
|---|---|
| 引擎 | EDG 前端做 IntelliSense，MSVC 负责构建 |
| 状态 | Visual Studio 2022 17.14 与 2026 18.10 仍提示模块 IntelliSense 为实验性；官方已知问题：导入模块或 header unit 后 IntelliSense 可能停止工作或提示错误过多。MSVC 编译器自身在 2026 年仍在修模块相关的 ICE |
| 构建系统 | MSBuild 自动识别 `.ixx`；CMake 的模块在 VS 生成器下可用，但 `import std` 只支持 Ninja 生成器（cmake-cxxmodules(7)） |
| VS Code | cpptools 1.35.0（2026-09-10）；“为模块导入添加 IntelliSense”的 #6302 自 2020-10 开放至今；Linux、macOS 上的 GCC、Clang 模块基本不可用 |
| 平台与许可 | Visual Studio 仅 Windows（Community 免费）；cpptools 限定微软产品使用，不在 Open VSX |

**评价：** 在 MSBuild + MSVC 的 Windows 工程里开箱即用；模块语义质量受 EDG 进度限制，七年未脱离实验状态，不覆盖 GCC、Clang。

### 4.5 JetBrains：CLion、ReSharper C++、Rider

| 方面 | 现状 |
|---|---|
| CLion 引擎 | 2026.2（2026-07）起 Classic 引擎移出 IDE，Nova 成为默认引擎。模块功能按帮助文档（2026-09-09）的描述，是 CLion 扫描 `.ixx`、`.cppm`、`.mxx` 建立模块名映射，再由内置 clangd 提供补全、导航与查找用法 |
| CLion 已测组合 | CMake + Ninja + MSVC；CMake + Visual Studio 生成器；CMake + 显式参数的 Clang。GCC 不在列表中 |
| CLion 功能 | 2026-06 起可自动补 `import`，限于主接口与分区直接导出的符号；跨模块重构标为早期功能，只作用于已打开的文件；`import std`、header unit 文档未提 |
| ReSharper C++ | 2024.1 重做模块导出的内部表示，支持内部分区；2025.3–2026.2 的发布说明未再出现模块条目；仅 Visual Studio |
| Rider | ReSharper C++ 引擎，C++20 为“部分支持”，没有模块文档；完整 C++ 仅 Windows |
| Fleet | 2025-12-22 停止下载 |
| 许可 | 商业订阅 |

**评价：** 闭源方案里跨平台与集成度最好的一类，但模块能力以 CMake 为中心，GCC 缺席，公开资料对 `import std` 与实现单元没有说明，深度无法从外部核实。

### 4.6 其他闭源工具

| 工具 | 引擎 | 模块支持 | 平台与许可 |
|---|---|---|---|
| Visual Assist（Embarcadero） | 自有解析器，2026.4 说明中提到“更新 Clang” | 2025.1 起支持模块的导入、查找引用与导航，识别 `.ixx`、`.cppm`；`import std` 未提 | Windows（VS 插件）；商业 |
| C++Builder / RAD Studio 13 | 新 64 位编译器 BCC64X 基于 Clang 20，其余编译器停留在 Clang 3.3–5.0；IDE 代码洞察默认用 Visual Assist，Clang LSP 默认关闭 | 文档只覆盖到 C++17，模块未见说明（未核实） | Windows；商业（有免费社区版） |
| Xcode 26 / 27 | Apple Clang（约 LLVM 20/21） | 发布说明中的 “module” 均指 Swift 或 Clang header modules，没有 C++20 命名模块的说明 | macOS；免费 |
| Cursor | 自带的 `anysphere.cpptools`，内部使用 clangd | 取决于 clangd | 商业订阅 |
| Windsurf | 扩展包，打包 vscode-clangd、CodeLLDB、CMake Tools | 取决于 clangd | 商业订阅 |
| SonarQube CFamily | Clang 19 | 实验性，需开启 `sonar.cfamily.enableModules`，要求包含 BMI 构建步骤的完整编译数据库；支持 `import std`，不支持 header unit；SonarLint 不支持 | 服务器与云；商业 |
| PVS-Studio | 自研分析器 | 2025-08 起实现“C++20 模块的模块级查找” | 三平台；商业 |
| GitHub CodeQL | 第三方 C++ 前端 | 文档明确“C++20 modules are not supported”，无时间表（#20674） | 开源项目免费 |
| SciTools Understand | Strict 模式用 Clang 前端 | 可选 C++20 标准，模块未见说明（未核实） | 三平台；商业 |
| SlickEdit、Coverity、Klocwork、Parasoft C/C++test、Helix QAC | 各自 | 未找到模块相关文档（未核实） | 商业 |

### 4.7 其他开源工具

| 工具 | 引擎 | 模块支持 | 活跃度与许可 |
|---|---|---|---|
| Qt Creator 20、Eclipse CDT LSP、GNOME Builder、Zed、Neovim、Helix、Emacs（lsp-mode、eglot）、Sublime LSP-clangd、Kate、Code::Blocks clangd_client、VS Code clangd 扩展 | 转交 clangd | 客户端都没有模块开关，Qt Creator 甚至没有通用的附加参数项；能力等于用户配置的 clangd | 活跃 |
| ccls | libclang 自有索引 | 无；#798（2021 年起）没有维护者回应 | 仍有提交；Apache-2.0 |
| cquery、rtags、cxxd、CodeCompass | libclang 或 Clang 前端 | 无 | cquery 已归档 |
| KDevelop | libclang | 无；KDE bug 406842（2019）开放，2024 年评论指出后端固定为 Clang，GCC 模块工程找不到模块 | 活跃 |
| Eclipse CDT 经典索引器 | 自有解析器 | 无；概念、`consteval` 等 C++20 基础特性仍有开放 issue | 活跃 |
| Sourcetrail 社区分支 | 内置 Clang 11 | 无（#58 开放） | 维护放缓 |
| Doxygen | 自有解析器 | 2022 年起支持模块，2026 年仍在修正确性问题（#12293 开放） | 活跃 |
| clang-uml | Clang LibTooling | 已实现，可按模块生成包图 | 活跃 |
| clang-tidy | Clang 前端 | 实验参数 `--enable-module-headers-parsing`；段错误（#221463）等问题开放 | LLVM |
| include-what-you-use | Clang LibTooling | 无 | 活跃 |
| Kythe、scip-clang | 自有抽取器 / Clang 21 | Kythe 的模块请求 #5543 开放；scip-clang 未声明支持 | 活跃 |
| GCC | — | 没有基于 GCC 的语言服务器；`g++-mapper-server` 只是构建期的 BMI 映射服务 | — |

开源工具里，模块支持集中在非导航类工具（Doxygen、clang-uml）；通用编辑器全部是 clangd 的外壳，没有一家在客户端处理编译数据库、`std` 或方言问题。

### 4.8 构建系统侧：各方案拿到的数据

| 构建系统 | 面向 IDE 的输出 | 模块图 | BMI 路径 | `std` 源码 | clangd 能否直接使用 |
|---|---|---|---|---|---|
| CMake 4.4.3 | 编译数据库带 `-fmodule-mapper` 或 `@modmap`；File API 的 `fileSets` 只给出文件角色；`CMAKE_EXPORT_BUILD_DATABASE` 仍为实验、仅 Ninja，门控 UUID 在 4.0–4.4 间至少变过两次 | 仅构建数据库有 | 仅构建数据库有 | 都没有 | 编译数据库需要规范化；构建数据库没有消费者 |
| mcpp 2026.9.15.1 | `emit build-database`：S1 等级 2，另可输出编译数据库 | 有 | 有 | 有（`mcpp:std`） | 编译数据库里的 BMI 参数属于构建编译器（实测失败）；S1 文档目前只有 lsp-mcpp 消费 |
| xmake 3.1.1 | `xmake project -k compile_commands --lsp=clangd` | 无 | 只有原始参数 | 无 | GCC 工具链写出 clangd 无法解析的参数（#5830）；2025 年仍有条目缺失问题（#6600、#6544） |
| build2 0.18.1 | 0.18.0 起原生生成编译数据库，模块接口编译默认不写入 | 无 | 需显式开启 | 无 | 否（clangd#758 以“无法处理”关闭） |
| MSBuild | 无导出物，内部做 P1689 扫描 | 无 | 无 | 无 | 不适用 |
| Meson 1.12.0 | 无；`cpp_importstd` 仍为实验 | 无 | 无 | 无 | 不适用 |
| Bazel 9.2.0 | 无原生输出；模块只支持 Clang，GCC 支持于 2026-01 回退 | 无 | 无 | 无 | 未发现可用案例 |
| Premake | 无模块规则（#2767 开放） | 无 | 无 | 无 | 不适用 |

标准化方面，2026-09-01 之后没有新进展：
- P2977 自 2024-10 的 R2 以来没有活动；
- EcoStd 的 RFC #2（工具自省）与 RFC #3（模块元数据）仍是未合并的 PR，RFC #3 作者计划下一步移植 P1689；
- CPS 0.15.0 已有指向 P3286 文件的 `cpp_module_metadata` 字段；
- BSP 的 `cpp` 扩展仍没有模块字段。

---

## 5. 其他维度对比

评级只在同一张表内相对比较：高、较高、中、较低、低。

### 5.1 跨平台

| 方案 | 主机平台 | 编译器与标准库 | 评级 |
|---|---|---|---|
| lsp-mcpp | Linux x64、macOS arm64、Windows x64 | GCC、MinGW、Clang（libc++、libstdc++）、clang++ MSVC ABI、clang-cl、cl.exe 与 MSVC STL 的 `import std`；无编译器时用语义工具包；三主机 CI 实测 | 较高（缺 arm64 与 x64 macOS） |
| clangd 23.1 | Linux、macOS、Windows x64；LLVM 官方另有 arm64 | 只接受 Clang 命令；Windows 与 MSVC STL 有未解决问题 | 中 |
| clice nightly | Linux、macOS、Windows 的 x64 与 arm64 | GCC、MSVC 经驱动探测交给 Clang；`import std` 无专门处理 | 较高（目标多，未经稳定版验证） |
| Visual Studio / cpptools | VS 仅 Windows；cpptools 三平台 | 实际只服务 MSVC | 低 |
| CLion | 三平台 | 已测组合为 CMake 下的 MSVC 与 Clang | 较高 |
| ReSharper C++、Visual Assist | Windows | MSVC、clang-cl 工程 | 低 |
| clangd 系编辑器 | 三平台 | 同 clangd | 中 |

### 5.2 易用性

| 方案 | 用户要做的事 | 工程目录写入 | 失败时的可见性 | 评级 |
|---|---|---|---|---|
| lsp-mcpp | 安装扩展，打开文件夹；无必需设置 | 无（实测断言） | 状态项给出问题项与修复命令，例如 mcpp 或 xlings 自己的说明 | 高 |
| clangd 23.1 | 生成完整且与 clangd 版本匹配、含 `std` 单元的数据库；加 `--experimental-modules-support`；`std` 在工程外时加 `--compile-commands-dir` | `.cache/clangd` 写在数据库旁；生成数据库往往先要配置或构建 | 常表现为请求无应答（实测） | 低 |
| clice nightly | 提供编译数据库 | 默认 `.clice/` 写在工作区（实测） | 依赖准备失败时跳过编译，不发布诊断（实测） | 较低 |
| Visual Studio | MSBuild 工程开箱可用 | 构建产物（正常行为） | 已知问题：IntelliSense 停止工作 | 较高（仅 MSVC 场景） |
| cpptools | 没有能让模块跳转工作的配置 | — | — | 低 |
| CLion | CMake 工程，扩展名符合约定，工具链在已测组合内 | CMake 构建目录 | 商业 IDE 常规体验 | 较高 |
| clangd 系编辑器 | 同 clangd，且多数没有模块开关 | 同 clangd | 同 clangd | 低 |

### 5.3 稳定性

| 方案 | 成熟度 | 已知问题规模 | 测试与验证 | 评级 |
|---|---|---|---|---|
| lsp-mcpp | 未发布 0.1.0 | 限制记录在 issue #2；clangd 缺陷以绕过方式处理 | 两类形态逐次三主机 CI、5 轮重复无失败、36 个夹具、nightly 自举 | 中偏高（验证充分，缺真实用户与发布历史） |
| clangd 23.1 | 实验性，默认关闭 | 模块相关 open issue 73 个，含断言崩溃、陈旧 PCM | LLVM 单元测试，无跨编译器一致性测试 | 中偏低 |
| clice nightly | beta，无稳定版 | 85 个 open issue，含崩溃与挂起 | 三主机单元、集成、编辑器端到端测试 | 较低 |
| Visual Studio / cpptools | 实验性（七年） | 官方已知问题；#6302 开放六年 | 商业 QA | 中偏低（模块部分） |
| CLion | 正式产品；模块部分功能为早期 | 公开资料少 | 商业 QA | 较高 |

### 5.4 技术路线合理性

| 方案 | 路线 | 合理之处 | 结构性限制 | 评级 |
|---|---|---|---|---|
| lsp-mcpp | R4 | 语义来自真实编译器前端；数据问题规范化、可测试；引擎可替换；与 mcpp 形成了标准化契约 | 双层复杂度；上限等于引擎；Clang 语义覆盖不了 GCC、MSVC 独有行为 | 较高（在“跨编译器零配置”目标下最合适） |
| clangd | R3 | 语义正确，生态底座，持续演进 | 数据正确性完全交给用户与构建系统；实验性长期未解除 | 较高 |
| clice | R3 | 为 IDE 设计的多进程、哨兵、缓存隔离 | 同样依赖数据；新引擎要重新积累稳定性 | 较高 |
| Visual Studio / cpptools | R1 | 与 MSBuild 深度集成 | 双前端分叉，进展受第三方前端制约 | 低 |
| CLion、ReSharper C++、Visual Assist | R2 或混合 | 不依赖 BMI | 闭源、商业，深度无法外部核实，只在各自宿主中 | 中 |

### 5.5 性能与开放性

| 方案 | 性能（模块工程） | 开放性 |
|---|---|---|
| lsp-mcpp | 小工程首次跳转 CI 冷启动中位数 Linux 6.29 s、macOS 4.31 s、Windows 8.53 s，温启动 2.11 / 1.82 / 2.93 s（nightly 2026-09-15）；开发机冷 2.1–2.5 s、温 0.8 s；171 个模块的仓库开发机冷 25 s、温 4.9 s | Apache-2.0；S1–S4 规范公开；mcpp 已实现 S1 生产方 |
| clangd | 与 lsp-mcpp 同一引擎；全库扫描与逐 TU 串行校验是已知瓶颈 | Apache-2.0 WITH LLVM-exception；被大多数开源编辑器复用 |
| clice | 未发布模块相关基准；作者称扫描器约 2 万文件/秒 | Apache-2.0 |
| 闭源方案 | 未公开 | 各自产品内 |

---

## 6. 实测细节（Linux 开发机）

### 6.1 同一批工程，用户各自能做的配置

| 工程 | lsp-mcpp（零配置） | clangd 23.1 + 用户生成的数据库 | clice nightly + 用户生成的数据库 |
|---|---|---|---|
| mcpp + LLVM 22 + `import std`（`mcpp-llvm`，10 项） | 10/10 | 0/10：`mcpp build --configure-only` 与 `mcpp build` 生成的数据库都指向 clang 22.1.8 的 `std.pcm`，报 “older format”；`--spec compile-commands` 的数据库指向尚未构建的 BMI，报 “not found”；随后跳转请求 45 秒无应答 | 0/10：`module 'std' not found`，跳过编译，不发布诊断；只有 `import` 补全可用 |
| mcpp + GCC 16 + `import std`（`mcpp-gcc`，10 项） | 10/10 | 0/10：`Module 'std' not found`，随后请求无应答 | 0/10：`module 'std' not found`（日志确认） |
| CMake + Clang 22，不用 `import std`（`cmake-clang`，7 项） | 7/7 | 5/7：两处 `import` 上跳转不支持（main 分支已支持） | 6/7：未推送诊断，其余通过 |
| 只有源码，无构建系统（`inferred`，9 项） | 9/9（语义工具包） | 0/9：没有数据库时按默认标准解析，报 “Unknown type name 'import'” | 0/9 |

模块层检查 M1–M5 由 lsp-mcpp 专有功能回答：lsp-mcpp 在 mcpp 与只有源码的工程上均 4/4 通过；clangd 全部未通过；clice 在有编译数据库的工程上只通过 `import` 补全。

### 6.2 对照组：同一个 mcpp 工程，换成规范化数据

| 数据 | clangd 23.1 | clice nightly |
|---|---|---|
| lsp-mcpp 规范化数据库（`lsp-mcpp model --export engine`） | 1/10：`std.cppm` 在工程目录外，找不到它的命令（日志 “Failed to find compilation database”） | 6/10：跳转、悬停、`std` 跳转、分区跳转、补全通过；未推送诊断、未保存编辑传播、引用缺少声明、测试文件跳转失败 |
| 上述数据库 + `--compile-commands-dir` | 8/10：只有 `import` 上跳转（23.1 不支持）与未保存编辑传播失败 | —（clice 读工作区数据库，无需此参数） |

结论：同一个前端，数据从“构建系统原样输出”换成“规范化后的完整数据”，mcpp 工程从 0 项通过变为 8 项（clangd）与 6 项（clice）通过。剩余差距主要来自 lsp-mcpp 在引擎之外补充的功能：模块索引回答 `import` 上跳转，以及接口未保存编辑的传播。

### 6.3 两类工程形态

见第 2.2 节。

### 6.4 首次跳转时间

`mcpp-llvm` 工程，冷启动，去掉诊断检查后连续 3 轮的中位数：

| 方案 | 首次跳转 | 说明 |
|---|---|---|
| lsp-mcpp | 3.03 s | 从 `initialize` 开始计时，包含调用 mcpp、建模型、构建 `std` |
| clangd 23.1 + 规范化数据库 + `--compile-commands-dir` | 2.05 s | 数据库在准备步骤中生成，不计时 |
| clice + 规范化数据库 | 4.58 s | 同上 |

lsp-mcpp 比“数据已备好的原生 clangd”多约 1 秒，这是零配置建模的代价：mcpp `emit build-database` 约 0.6 秒，外加协议查询与工具链探测。clice 在同样的数据上慢于 clangd。

---

## 7. lsp-mcpp 的定位

1. **不与前端竞争，做数据层与交付层。** 第 6.2 节的对照组说明，原生 clangd 与 clice 在同一 mcpp 工程上的失败来自数据，换成规范化数据后核心导航随即可用。lsp-mcpp 的价值是“让 Clang 前端在任意构建系统、任意编译器家族与两类工程形态下拿到正确的模块数据”，再加上零配置交付：内置引擎与语义工具包，不写工程目录，用语言状态项呈现状态。
2. **与 clangd、clice 是上下游关系。** clangd 是当前引擎；clice 在工程设计上更适合 IDE，可以作为第二引擎候选。两者的改进都会直接抬高 lsp-mcpp 的上限。
3. **规范是长期护城河，也是最大的外部依赖。** S1 已被 mcpp 实现并闭环验证；CMake 的构建数据库仍无消费者，P2977 停滞，EcoStd 的相关 RFC 尚未合并。S1/S2 进入 EcoStd，其他构建系统才有实现的理由。

---

## 8. 建议（需要 review）

| 编号 | 建议 | 理由 | 代价 |
|---|---|---|---|
| A1 | 引擎适配层保持可替换，把 clice 列为第二引擎候选。评估清单：推送诊断；未保存编辑传播到导入方；实现单元内的实现分区；引用包含声明；缓存不写工作区；Windows 稳定性；稳定版发布 | 实测中 clice 在规范化数据上，全 `.cppm` 工程 12/15、分离工程 10/20；其多进程隔离与哨兵节点正对应 lsp-mcpp 为 clangd 做的绕过 | 需要映射 clice 的扩展协议 |
| A2 | 向 clangd 上游推动：按标准库清单（P3286 形状）发现 `std`；未解析导入以诊断结束而不是挂起；工程外模块单元的命令查找；逐 TU 串行校验 BMI 的性能 | 分别对应 lsp-mcpp 当前的注入、排除、参数与温启动瓶颈，修在上游可删掉绕过 | 按 D14 属于最后阶段，需要确认优先级 |
| A3 | 向 clice 反馈实测问题：诊断未推送、实现单元中使用实现分区的位置无法导航与补全、引用缺少声明、`.clice/` 默认写工作区、依赖准备失败不提示 | 有可复现的夹具 | 小 |
| A4 | S1/S2 提交 EcoStd，与 RFC #3 作者协调 P1689、P2977 的移植顺序；邀请 CMake、xmake、build2 实现 S1 生产方 | 目前只有 mcpp 实现；CMake 构建数据库缺消费者，S1 正好补上 | 规范工作周期长 |
| A5 | 补齐负载平台（Linux arm64、Windows arm64、macOS x64）；为 Neovim、Zed、Helix 提供配置与端到端测试；发布到 Open VSX | clice 已覆盖 6 个目标与 3 种编辑器；服务端本身就是通用 LSP | CI 时间与负载体积 |
| A6 | 重新评估 D28“不做预发布”，以 0.1 预览版收集真实工程反馈 | 稳定性维度的主要缺口是没有真实用户与发布历史 | 需要决定 |
| A7 | 为 Linux 自建可移植（glibc 2.18 基线）的 clangd 23.1.1 或更新构建，结束对 23.1.0 的固定与 `align_val_t` 绕过 | clangd/clangd 不发 23.1.1，LLVM 官方 Linux 包要求 glibc 2.34 | 新增 LLVM 构建流水线 |
| A8 | 两类工程形态扩到更多构建系统（CMake 的分离工程、MSBuild 工程）与更大的真实仓库（Windows 自举） | 目前两类形态的逐次 CI 只覆盖 mcpp；大工程只在 nightly 覆盖 Linux 与 macOS | 夹具与 CI 时间 |
| A9 | GCC、MSVC 独有语义的诊断差异：继续标注语义来源，并评估合并构建编译器自身的诊断 | R3/R4 路线的结构性限制 | 需要从构建日志取诊断，范围较大 |

---

## 9. 需要 review 的要点

1. 评级是否公允。报告作者就是 lsp-mcpp 的实现者，实测夹具与运行器也出自本项目：
   - 标准检查（C*）按 LSP 标准行为设计；
   - 模块层检查（M*）偏向 lsp-mcpp，已单独列出；
   - 首次跳转计时中，原生 clangd 与 clice 的数据由 lsp-mcpp 预先生成，没有计入它们的时间。
2. 两类工程形态的判定标准是否符合预期：第 2.1 节的两个工程，以及第 2.2 节的检查项。
3. 是否把 clice 列为第二引擎候选（A1），以及评估清单。
4. 上游工作的优先级（A2、A3）是否提前于 D14 的“最后阶段”。
5. 是否启动 S1/S2 的 EcoStd 提交（A4）。
6. 负载平台、其他编辑器与预览版（A5、A6），以及是否投入自建 clangd 构建（A7）。

---

## 附录 A 主要来源

**clangd**
- [clangd features：Experimental C++20 Modules Support](https://clangd.llvm.org/features)
- [LLVM 23.1.0 Extra Clang Tools 发布说明](https://releases.llvm.org/23.1.0/tools/clang/tools/extra/docs/ReleaseNotes.html)
- [llvm-project#193883 持久化 BMI 缓存](https://github.com/llvm/llvm-project/pull/193883)、[#193158 从命令读取模块映射](https://github.com/llvm/llvm-project/pull/193158)、[#218958 发现数据库之外的模块文件](https://github.com/llvm/llvm-project/pull/218958)、[#219839 import 上跳转](https://github.com/llvm/llvm-project/pull/219839)
- [llvm-project#218152 align_val_t 回归](https://github.com/llvm/llvm-project/issues/218152)、[#213499](https://github.com/llvm/llvm-project/issues/213499)、[#188919](https://github.com/llvm/llvm-project/issues/188919)、[#126350](https://github.com/llvm/llvm-project/issues/126350)、[#214657](https://github.com/llvm/llvm-project/issues/214657)、[#219730](https://github.com/llvm/llvm-project/issues/219730)、[#221762](https://github.com/llvm/llvm-project/issues/221762)
- [clangd/clangd#2569 模块下的重命名与引用](https://github.com/clangd/clangd/issues/2569)、[#2589 import std](https://github.com/clangd/clangd/issues/2589)、[#2477 MinGW GCC](https://github.com/clangd/clangd/issues/2477)
- [Chuanqi Xu：C++20 Modules Support in Clangd（2025-12-03）](https://chuanqixu9.github.io/c++/2025/12/03/Clangd-support-for-Modules.en.html)
- [RFC：发现编译数据库之外的模块接口](https://discourse.llvm.org/t/rfc-clangd-discover-c-module-interfaces-missing-from-the-compilation-database/91448)
- [clangd/clangd releases](https://github.com/clangd/clangd/releases)

**clice**
- [clice-io/clice](https://github.com/clice-io/clice)（HEAD 3aaef8d，2026-09-14）及其 `docs/en/design`、`docs/en/features`
- [clice-io/clice-llvm](https://github.com/clice-io/clice-llvm)
- [v0.1.2026091407 发布](https://github.com/clice-io/clice/releases/tag/v0.1.2026091407)
- [VS Code Marketplace：clice-io.clice](https://marketplace.visualstudio.com/items?itemName=clice-io.clice)
- issues [#677](https://github.com/clice-io/clice/issues/677)、[#549](https://github.com/clice-io/clice/issues/549)、[#469](https://github.com/clice-io/clice/issues/469)、[#352](https://github.com/clice-io/clice/issues/352)、[#601](https://github.com/clice-io/clice/issues/601)

**微软**
- [What's new for C++ in Visual Studio（已知问题）](https://learn.microsoft.com/en-us/cpp/overview/what-s-new-for-visual-cpp-in-visual-studio)
- [Visual Studio 2026 18.7–18.10 更新](https://devblogs.microsoft.com/cppblog/whats-new-for-c-developers-in-visual-studio-2026-18-7-18-10/)
- [MSVC Build Tools 2026-09 预览说明](https://devblogs.microsoft.com/cppblog/msvc-build-tools-preview-updates-september-2026/)
- [vscode-cpptools#6302](https://github.com/microsoft/vscode-cpptools/issues/6302)、[#13987](https://github.com/microsoft/vscode-cpptools/issues/13987)
- [cmake-cxxmodules(7)](https://cmake.org/cmake/help/latest/manual/cmake-cxxmodules.7.html)

**JetBrains**
- [CLion：Support for C++20 Modules（2026-09-09）](https://www.jetbrains.com/help/clion/support-for-c-20-modules.html)
- [CLion 2026.2 发布](https://blog.jetbrains.com/clion/2026/07/2026-2-release/)
- [CLion 现代 C++ 支持（2026-06）](https://blog.jetbrains.com/clion/2026/06/modern-cpp-support/)
- [ReSharper C++ 2024.1](https://blog.jetbrains.com/rscpp/2024/04/09/resharper-cpp-2024-1/)
- [Fleet 的未来](https://blog.jetbrains.com/fleet/2025/12/the-future-of-fleet/)

**其他闭源**
- [Visual Assist What's New](https://www.wholetomato.com/features/whats-new)
- [SonarQube：C++20 模块支持](https://community.sonarsource.com/t/c-20-modules-support/123537)
- [CodeQL 支持的编译器（“C++20 modules are not supported”）](https://github.com/github/codeql/blob/main/docs/codeql/reusables/supported-versions-compilers.rst)、[github/codeql#20674](https://github.com/github/codeql/issues/20674)
- [isocpp.org 产品新闻（PVS-Studio 2025-08）](https://isocpp.org/blog/category/product-news)
- [Understand 支持的语言](https://support.scitools.com/support/solutions/articles/70000582794-supported-languages)
- Xcode 26/27 发布说明（developer.apple.com 的 DocC 数据接口）
- C++Builder Florence 文档（archive.org 快照）

**开源工具**
- [ccls#798](https://github.com/MaskRay/ccls/issues/798)
- [KDE bug 406842](https://bugs.kde.org/show_bug.cgi?id=406842)
- [Sourcetrail 分支 #58](https://github.com/petermost/Sourcetrail/issues/58)
- [Doxygen#12293](https://github.com/doxygen/doxygen/issues/12293)
- [clang-uml#101](https://github.com/bkryza/clang-uml/issues/101)
- [llvm-project#221463 clang-tidy](https://github.com/llvm/llvm-project/issues/221463)
- [Kythe#5543](https://github.com/kythe/kythe/issues/5543)
- [Qt Creator clangd 设置](https://doc.qt.io/qtcreator/creator-preferences-cpp-clangd.html)
- [Eclipse CDT LSP](https://github.com/eclipse-cdt/cdt-lsp)
- [Zed C++ 文档](https://github.com/zed-industries/zed/blob/main/docs/src/languages/cpp.md)

**构建系统与标准**
- [CMAKE_EXPORT_BUILD_DATABASE](https://cmake.org/cmake/help/latest/variable/CMAKE_EXPORT_BUILD_DATABASE.html)
- [xmake#5830](https://github.com/xmake-io/xmake/issues/5830)、[#6600](https://github.com/xmake-io/xmake/issues/6600)、[#6544](https://github.com/xmake-io/xmake/issues/6544)
- [clangd#758（build2）](https://github.com/clangd/clangd/issues/758)
- [Bazel#28190 回退 GCC 模块](https://github.com/bazelbuild/bazel/pull/28190)、[#30023](https://github.com/bazelbuild/bazel/issues/30023)
- [Premake#2767](https://github.com/premake/premake-core/issues/2767)
- [cplusplus/papers#1702（P2977）](https://github.com/cplusplus/papers/issues/1702)
- [EcoStd RFC #2](https://github.com/ecostd/rfcs/pull/2)、[#3](https://github.com/ecostd/rfcs/pull/3)
- [cps-org/cps#95](https://github.com/cps-org/cps/pull/95)

**lsp-mcpp**
- [Sunrisepeak/lsp-mcpp-private#1](https://github.com/Sunrisepeak/lsp-mcpp-private/pull/1)
- CI [run 34894725998](https://github.com/Sunrisepeak/lsp-mcpp-private/actions/runs/34894725998)
- nightly [run 34882966882](https://github.com/Sunrisepeak/lsp-mcpp-private/actions/runs/34882966882)
- [mcpp-community/mcpp#636](https://github.com/mcpp-community/mcpp/issues/636)、[#639](https://github.com/mcpp-community/mcpp/pull/639)

## 附录 B 实测复现

1. **构建：** 用 `mcpp build` 构建 lsp-mcpp，得到 `lsp-mcpp` 与 `lsp-mcpp-conformance`。
2. **两类工程形态：** 仓库中的 `conformance/fixtures/mcpp-split`、`mcpp-split-gcc`、`mcpp-split-msvc`、`mcpp-all-cppm`，用 `lsp-mcpp-conformance run --server <lsp-mcpp> --payload <payload> --fixture <dir>` 运行。
3. **原生 clangd：** 用包装脚本启动 `clangd --experimental-modules-support`，对照组再加 `--compile-commands-dir="$PWD"`。
4. **clice：** 用包装脚本启动 `clice serve`（Linux x86_64 nightly v0.1.2026091407）。
5. **夹具副本：** 在临时目录中复制上述夹具与 `mcpp-llvm`、`mcpp-gcc`、`cmake-clang`、`inferred`，去掉状态检查，按第 1.3 节加入准备步骤：
   - `mcpp build --configure-only`、`mcpp build`，或 `mcpp emit build-database --spec compile-commands -o compile_commands.json`；
   - 规范化数据库：`lsp-mcpp model --root . --export engine > compile_commands.json`。
6. **运行：** 用同一个运行器分别以 `--server <包装脚本>` 运行，`--timeout` 取 20–45 秒，`--measure` 记录时间线。
