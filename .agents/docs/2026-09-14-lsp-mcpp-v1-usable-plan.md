# lsp-mcpp 第一版真实可用方案

日期：2026-09-14
状态：**已实施**，执行记录见第 10 节
范围：PR #1 已交付的实现之上，第一版达到真实可用还缺的能力、验证与证据
关联：
- 设计：[2026-09-13-cxx-modules-unified-lsp-design.md](2026-09-13-cxx-modules-unified-lsp-design.md)（v0.4）
- 实现计划与执行记录：[2026-09-14-lsp-mcpp-v1-implementation-plan.md](2026-09-14-lsp-mcpp-v1-implementation-plan.md)
- 只记录未修复的事项：Sunrisepeak/lsp-mcpp-private#2
- mcpp 功能需求：mcpp-community/mcpp#636
- 可行性探测（一次性分支，结论见第 3.2 节）：run 34801841804、34802193118、34804663328、34805949880、34806131901、34806339903、34806554496

## 目录

0 概述 · 1 本轮决定 · 2 可用的判定场景 · 3 实测基线与差距 · 4 工作项 · 5 验证体系 · 6 顺序与依赖 · 7 风险 · 8 不在本方案内的事项 · 9 需要 review 的要点 · 10 执行记录

## 0. 概述

| 方面 | 内容 |
|---|---|
| 现状 | PR #1 的 14 个 CI 任务全部通过。端到端运行过的组合：P1、P2、P3、P4、不使用 `std` 的 P7，以及三个平台的语义工具包 |
| 可用的判定 | 第 2 节的十个场景 U1–U10，在用户实际的环境中通过：Windows 不带开发者环境、没有编译器的机器、没有 Command Line Tools 的 macOS、真实规模的仓库，并且以 VSIX 形态安装 |
| 探测得到的主要事实 | 见第 3.2 节，要点如下 |
| | MSVC STL 的 `import std` 在 P5、P6、P7 上全部失败，原因有三：清单形状不被识别；cl 模式无法把 `.ixx` 当作模块接口；clangd 23.1.0 在 MSVC STL 上报 `align_val_t` 二义性，clang 22.1.8 没有。三者都有已验证的解法：clangd 23.1.0 在不带开发者环境的机器上构建出 `std`、`std.compat` 与用户模块，0 个错误（E1、E7–E10） |
| | mcpp 在 Windows 上的两种工具链都能构建 `import std` 工程，服务端却找不到 `std`；lsp-mcpp 自己的仓库也因 `std` 来自运行时包而失败（E5、E18） |
| | 没有编译器的 Linux 容器、隐藏了 Visual Studio 的 Windows 上，语义工具包 16 项检查全部通过；移走 Command Line Tools 的 macOS 上状态仍为 `ready`，请求一直返回空结果（E13–E15） |
| | mcpp 仓库自举 7 项检查通过，首次跨模块跳转 95.4 秒（E17）；小工程冷启动到首次跳转 4.6–12.0 秒，温启动没有测量 |
| 工作项 | W1 MSVC 家族与 `import std`；W2 Visual Studio 自动切换；W3 mcpp 生产方与 S2 0.2；W4 CMake 构建数据库；W5 干净机器与首次使用；W6 零打扰断言与 VSIX 形态；W7 性能；W8 自举；W9 服务端完备项；W10 规范与一致性对应；W11 交叉构建产物的原生验证 |
| 顺序 | 阶段 A：W1、W2 → B：W5、W6 → C：W3、W4、W10 → D：W7、W8、W9 → E：W11 与收口 |
| 交付与验证 | 继续在 PR #1 上提交，每个阶段结束时 CI 全部通过；Windows 与 macOS 上的能力只能在 CI 中验证，本地覆盖 Linux 与 Wine |

## 1. 本轮决定

| 编号 | 决定 | 来源 |
|---|---|---|
| D26 | clang++ 构建 MSVC ABI（P5）、clang-cl（P6）与 MSVC STL 的 `import std`（P5、P6、P7 都要）是第一版必须项。每一项都要有 windows-2022 上端到端运行的一致性夹具 | 2026-09-14 review |
| D27 | 保留设计 9.3 节第 5 条：工作区没有构建系统、机器装有 Visual Studio 时，自动改用 MSVC 语义 | 2026-09-14 review |
| D28 | 暂不发布预发布版本。可用性以本地与 CI 验证为准；Marketplace、Open VSX、GitHub Releases 与 xim-pkgindex 在验证完成后另行决定 | 2026-09-14 review |
| D29 | mcpp 生产方输出单个 S1 文档，放在 mcpp 机器输出协议 v1 的信封里，不写工程目录；S2 升到 0.2，增加单文档模式 | 本方案提议（W3） |
| D30 | 所有 Windows 夹具的服务端都在普通环境中运行，不继承开发者命令行环境；开发者环境只用于夹具自身的构建步骤 | 本方案提议（W1） |

## 2. 可用的判定场景

现有夹具在三方面比用户的真实环境宽松：

- Windows 夹具在开发者命令行环境（`ilammy/msvc-dev-cmd`）里启动服务端，`INCLUDE`、`VCToolsInstallDir` 都已设好；用户从开始菜单启动 VS Code 时没有这些变量。
- macOS 机器装有 Command Line Tools 与 Xcode；Linux 与 Windows 的“无编译器”夹具只是用 `--no-discover` 让服务端忽略机器上的编译器。
- 工程只有两三个源文件，服务端的缓存目录每次都是新的，只测到冷启动。

因此第一版以下表十个场景为准。每个场景都用打包好的 VSIX 安装扩展（不用扩展开发路径），在注明的环境里通过，才算可用。

| 场景 | 环境 | 操作 | 通过条件 | 对应标准 |
|---|---|---|---|---|
| U1 | Linux，mcpp 工程（gcc 16、llvm 22）与 CMake 工程 | 打开工作区 | 跨模块跳转、补全、悬停、引用，包括 `std`；零弹窗、零常驻状态栏项、零自动面板；工程目录不新增文件 | SC1、SC3 |
| U2 | Windows，装有 Visual Studio 2022，VS Code 不在开发者环境中启动 | 打开 CMake + cl.exe 工程，源码使用 `import std` | 同 U1 | SC1（P7） |
| U3 | 同 U2 | 打开 clang-cl 构建的工程；打开 clang++ 以 MSVC ABI 构建的工程；两者都使用 `import std` | 同 U1 | SC1（P5、P6） |
| U4 | 同 U2 | 打开 mcpp 工程：Windows 默认工具链（llvm，目标 `x86_64-windows-msvc`）与 `msvc@system` | 同 U1 | SC1、SC6 |
| U5 | 同 U2，工作区只有模块源码 | 打开 | 使用 MSVC STL 语义，`import std` 可解析 | D27 |
| U6 | 没有编译器的 Linux 容器；没有 Visual Studio 与任何编译器的 Windows；只装 Command Line Tools 的 macOS | 只装扩展，不做设置 | 语义工具包下 U1 的功能全部可用 | SC2 |
| U7 | 没有 Command Line Tools 与 Xcode 的 macOS | 打开 | 只出现一次安装 Command Line Tools 的询问；安装后无需重启即恢复 | SC2；设计 1.2 节第 3 条 |
| U8 | lsp-mcpp 仓库与 mcpp 仓库 | 打开 | 跨包导航可用；冷启动与温启动达到设计 1.3 节的指标 | SC4、SC7 |
| U9 | 多根工作区；同一文件属于两个 target | 打开、切换上下文 | 每个根各自建模；切换上下文后语义随之变化 | 设计 13.4 节 |
| U10 | 同时装有微软 C/C++ 扩展或 clangd 扩展 | 打开 | 只询问一次；同意后只改工作区设置中的对应项 | SC3；设计 16.5 节 |

## 3. 实测基线与差距

### 3.1 PR #1 的 CI 已证明的内容

依据：PR #1 的 run 34798097333，14 个任务全部通过。

| 组合 | 端到端证据 | 与真实环境的差距 |
|---|---|---|
| P1 gcc，Linux | `mcpp-gcc` | 无 |
| P2 MinGW-w64 gcc | `mingw`（Linux 与 Windows 主机） | 无 |
| P3 llvm，Linux | `mcpp-llvm`、`cmake-clang`，以及经符号链接到达的工作区 | 无 |
| P4 llvm，macOS | `mcpp-llvm`（macOS 主机） | 机器装有 Command Line Tools |
| P5 clang++，MSVC ABI | 无 | 无夹具 |
| P6 clang-cl | 无，只有参数翻译的单元测试 | 无夹具 |
| P7 cl.exe | `cmake-msvc` | 源码没有 `import std`；服务端运行在开发者环境中 |
| K1、K2、K3 语义工具包 | `inferred`、`untrusted`（三个主机） | `--no-discover` 忽略了机器上的编译器；macOS 装有 Command Line Tools |

冷启动耗时取自同一次运行的一致性日志。“到首次跳转”是 `initialize`、状态进入 `ready`、首个文件诊断发布、首次跳转四段之和；每个夹具使用新的缓存目录。

| 主机 | 使用 `import std` 的夹具 | 不使用 `std` 的 CMake 夹具 |
|---|---|---|
| ubuntu-24.04 | `inferred` 4.7 秒，`mcpp-gcc` 9.2 秒，`mcpp-llvm` 9.4 秒（经符号链接再跑一次 4.8 秒），`mingw` 4.7 秒 | `cmake-clang` 1.5 秒 |
| macos-14 | `inferred` 6.8 秒，`mcpp-llvm` 7.7 秒 | 无 |
| windows-2022 | `inferred` 12.0 秒，`untrusted` 8.9 秒，`mingw` 8.5 秒 | `cmake-msvc` 1.7 秒 |

设计 1.3 节的冷启动目标是 5 秒以内，温启动 1 秒以内。CI 机器上使用 `std` 的夹具多数超过 5 秒，两个 CMake 夹具的差异说明主要耗时在构建 `std` 模块。温启动没有任何测量。

### 3.2 探测结果

以下探测运行在一次性分支上，都使用 PR #1 在 run 34798097333 中组装的负载（服务端、clangd 23.1.0、`lsp-mcpp-kit`）；分支与工作流不进入 PR。windows-2022 镜像实测为 Visual Studio 2022 Enterprise 17.14.39，MSVC 工具集 14.29.30133 与 14.44.35207，cl 19.44.35228，Visual Studio 自带 clang-cl 19.1.5，另有 LLVM 20.1.8，CMake 3.31.6。

| 编号 | 探测 | 结果 | 来源 |
|---|---|---|---|
| E1 | MSVC STL 的模块清单 | 14.44.35207 的 `modules` 目录有 `modules.json`、`std.ixx`、`std.compat.ixx`；14.29.30133 没有 `modules` 目录。清单内容是 `{"version": 1, "revision": 0, "library": "microsoft/STL", "module-sources": ["std.ixx", "std.compat.ixx"]}`，不是 P3286 形状 | run 34801841804 |
| E2 | CMake 3.31.6 + cl.exe + `import std` | 构建成功。编译数据库含 `std.ixx`、`std.compat.ixx` 两个条目（目标 `__cmake_cxx23`）；`.modmap` 中是 `-reference std=CMakeFiles\__cmake_cxx23.dir\std.ifc`；cl.exe 路径记录为短文件名 `C:\PROGRA~1\MICROS~2\2022\ENTERP~1\VC\Tools\MSVC\1444~1.352\bin\Hostx64\x64\cl.exe` | run 34801841804 |
| E3 | 服务端打开 E2 的工程（开发者环境与普通环境各一次） | 状态带问题项 `the standard library module manifest was not found (std.compat.ixx)`；clangd 日志 `Failed to build module std; due to Failed to compile ...\std.ixx`；诊断为空，跳转与悬停在 120 秒内一直返回空结果，只有补全可用 | run 34801841804 |
| E4 | cl.exe 构建、不使用 `std` 的 `cmake-msvc` 夹具，服务端在普通环境运行 | 9 项检查全部通过 | run 34801841804 |
| E5 | mcpp 2026.9.14.1 在 windows-2022 上构建 `import std` 工程 | `msvc@system` 与 `llvm@22.1.8`（目标 `x86_64-pc-windows-msvc`，标准库为 MSVC STL）都构建成功。`--configure-only` 在工程根目录写入 `compile_commands.json` 与 `target/`；编译数据库中没有 `std` 源文件，只有 `/reference std=...\std.ifc` 或 `-fmodule-file=std=...\std.pcm`。服务端打开这两个工程，全部单元因 `module std cannot be resolved` 被移出引擎数据库，8 项检查失败 | run 34801841804 |
| E6 | CMake 与 clang 家族（Visual Studio 自带的 clang 19.1.5） | clang-cl 的模块工程：CMake 3.31.6 与 4.1.3 都报 `the compiler does not provide a way to discover the import graph dependencies`。clang++（MSVC ABI）的模块工程：两个版本都构建成功，3.31.6 构建的夹具在服务端 9 项检查通过（开发者环境）。clang++ 的 `import std`：3.31.6 报 `Only libc++ is supported`，4.1.3 报 `__CMAKE::CXX23` 目标不可用。CMake 4.4.2 的 `Modules/Compiler/Clang-CXX.cmake` 为 clang-cl 19.1 及以上提供扫描规则，`import std` 只支持 libc++ 与 libstdc++（按代码，未在 Windows 上运行） | run 34801841804、34805949880；本地读取 CMake 4.4.2 |
| E7 | 以 clang-cl 与 clang++ 直接构建 MSVC STL 的 `std` 模块 | clang-cl 19.1.5 与 20.1.8：`/clang:-xc++-module` 对 `std.ixx` 不起作用，报 `'-x c++-module' after last input file has no effect` 与 `'linker' input unused`；把 `std.ixx` 复制为 `.cppm` 后，`std`、`std.compat`、导入它们的接口单元与普通单元全部编译成功。clang++ 19.1.5 与 20.1.8 以 `-x c++-module std.ixx` 编译成功 | run 34801841804、34804663328 |
| E8 | clangd 23.1.0 编译 MSVC STL 的 `std` 模块（`clangd --check`，cl 模式的 `.cppm` 副本与 GNU 模式的 `std.ixx` 各一次） | 两种模式、有无开发者环境都失败，诊断为 `in included file: reference to 'align_val_t' is ambiguous`（第 43、53、62、70 行）后 `too many errors emitted`。经服务端的端到端夹具结果与 E3 相同 | run 34804663328、34806131901 |
| E9 | clang 编译器编译 MSVC STL 的 `std` 模块（cl 模式 `.cppm` 副本与 GNU 模式 `std.ixx`） | clang 22.1.8 两种模式都成功。clang 23.1.0（与负载中 clangd 同一修订 `ea7d852a`）两种模式都失败：`vcruntime_new.h(97,12): error: reference to 'align_val_t' is ambiguous`，候选为 `vcruntime_new.h(27,33)` 的 `_VCRT_EXPORT_STD enum class align_val_t : size_t {};`。加 `/Zc:alignedNew-`（cl 模式）或 `-fno-aligned-allocation`（GNU 模式）后，clang 23.1.0 两种模式都成功，clangd 23.1.0 对 `std` 模块源文件的检查为 0 个错误 | run 34806339903 |
| E10 | clangd 23.1.0 在普通环境中构建完整的模块链：`std`、`std.compat`、导入 `std` 的接口单元，以及 `import greet; import std.compat;` 并调用 `std::println` 与 `::printf` 的 `main.cpp` | 两种写法都是 0 个错误：cl 模式（`.cppm` 副本、`/Zc:alignedNew-`、`/vctoolsdir`、`/winsdkdir`、`/winsdkversion`）构建前置模块 5.53 秒；GNU 模式（`-x c++-module std.ixx`、`-fno-aligned-allocation`、三个 `-Xmicrosoft-*` 参数）4.77 秒 | run 34806554496 |
| E11 | 显式工具集与 SDK 参数与 `INCLUDE` 的优先级 | clang 22.1.8：cl 模式的 `/vctoolsdir`、`/winsdkdir`、`/winsdkversion` 与 GNU 模式的 `-Xmicrosoft-visualc-tools-root`、`-Xmicrosoft-windows-sdk-root`、`-Xmicrosoft-windows-sdk-version` 都生效，并且在设置了 `INCLUDE` 时仍然优先 | 本地实测 |
| E12 | 装有 Visual Studio、没有构建系统的工作区（发现开启） | 使用 MinGW 语义工具包，16 项检查全部通过；没有切换到 MSVC 语义 | run 34802193118 |
| E13 | 隐藏 Visual Studio（改名 `vswhere.exe` 所在目录）、PATH 只保留系统目录的 Windows | 使用 MinGW 语义工具包，16 项检查全部通过，用时 13.2 秒 | run 34804663328 |
| E14 | 不装编译器的 `ubuntu:24.04` 容器 | 语义工具包，16 项检查全部通过，用时 9.3 秒 | run 34802193118 |
| E15 | 移走 Command Line Tools 与 Xcode 的 macOS（`xcrun --show-sdk-path` 失败） | 状态为 `ready`，问题项 `sdk-missing` 不带命令；`main.cpp` 报 `Module 'std' not found`；7 项检查失败，跳转、悬停、补全、引用在 300 秒内一直返回空结果；只有语法索引提供的模块名跳转、模块图与补全通过 | run 34802193118 |
| E16 | CMake 构建数据库 | CMake 3.31.6（CI）：不带正确 UUID 时生成步骤失败。CMake 4.4.2（本地）：开关为 `70ef007e-b743-492d-9407-e35eeac03a40`；默认目标不生成合并后的文件，需要构建 `build_database.json` 目标。把生成的文件放进带 `CMakeCache.txt` 的构建目录后，`lsp-mcpp model` 读出两个 set（`shapes@`、`app@`）、可见性、提供与导入关系，角色由扫描补全 | run 34802193118；本地实测 |
| E17 | 自举：用服务端打开 mcpp 仓库（b8d96844，gcc 16.1.0） | 引擎数据库 292 个条目，含 2 个 `std` 单元；7 项检查全部通过。首次跨模块声明跳转用时 95.4 秒，之后的请求都在 0.1 秒内返回 | run 34802193118 |
| E18 | 自举：用服务端打开 lsp-mcpp 仓库（60d03ba） | 109 个条目中 102 个因 `module std cannot be resolved` 被移出：lsp-mcpp 的 `std` 模块来自 openkal-llvm-runtime 包，编译数据库只有 `-fmodule-file=std=...pcm`，驱动查询也找不到清单。8 项检查失败 | run 34802193118 |
| E19 | Linux 上 `mcpp-llvm` 连续三次冷启动（每次新缓存目录） | 到首次跳转分别为 7.5、4.6、4.6 秒 | run 34802193118 |

### 3.3 差距清单

| 标准 | 已达成的部分 | 差距 | 工作项 |
|---|---|---|---|
| SC1 | P1–P4、不使用 `std` 的 P7、K1–K3 | P5、P6 没有夹具；P5、P6、P7 的 `import std` 全部失败（E3、E5、E8）；没有构建系统时不切换到 MSVC 语义（E12） | W1、W2 |
| SC2 | 一致性层面：不装编译器的 Linux 容器、隐藏了 Visual Studio 的 Windows（E13、E14） | 没有端到端证据；没有 Command Line Tools 的 macOS 失败，也没有询问（E15） | W5 |
| SC3 | 端到端断言通知数为零 | 常驻状态栏项、自动面板、工作区写入、冲突处理没有断言；打开 mcpp 工程会写入 `compile_commands.json` 与 `target/`（E5） | W3、W6 |
| SC4 | 无 | 温启动没有测量；冷启动 4.6–12.0 秒，高于设计 1.3 节的 5 秒（第 3.1 节、E19） | W7 |
| SC5 | 已达成（C7 检查，三个平台） | 无 | 无 |
| SC6 | mcpp 与 CMake 来源都是等级 2 | mcpp 没有构建数据库命令；CMake 构建数据库没有端到端夹具，私有配置不带实验开关（E16） | W3、W4 |
| SC7 | mcpp 仓库 7 项检查通过（E17） | 首次跳转 95.4 秒；lsp-mcpp 仓库找不到 `std`（E18） | W3、W7、W8 |
| SC8 | 交叉构建任务检查产物格式 | Windows 与 macOS 负载中的服务端是原生构建的 | W11 |

成功标准之外，设计中已有、实现中缺少或未验证的项：多根工作区只用第一个文件夹；客户端不支持动态注册时没有文件监视回退；运行时不校验负载；没有引擎接口与 clangd 能力表；`specs/README.md` 声称 CI 校验示例而 CI 中没有这一步；没有规则到用例的对应表。它们归入 W9 与 W10。

## 4. 工作项

### W1 MSVC 家族与 MSVC STL 的 `import std`

**目标：** D26。在不带开发者环境的 Windows 上，P5、P6、P7 都能使用 `import std`，对应夹具全部通过。

**现状（按代码与第 3.2 节探测）：**

| 编号 | 缺口 | 依据 |
|---|---|---|
| G1 | 标准库清单解析只认 P3286 形状（`modules` 数组），MSVC STL 自带的 `modules.json` 是另一种形状（`library`、`module-sources`），解析失败，服务端无法注入 `std` | `src/spec/metadata.cpp`；探测 E1 |
| G2 | 引擎参数中没有 Visual Studio 工具集与 Windows SDK 的路径。`translate_msvc` 的两个路径参数在调用处传空（`src/normalize/plan.cpp`），clangd 只能依赖开发者环境中的 `INCLUDE` | 代码；E10、E11 |
| G3 | cl 模式下为模块单元追加的 `/clang:-xc++-module` 不起作用：clang-cl 把 `/clang:` 参数排在所有输入之后，扩展名为 `.ixx` 的 `std.ixx` 被当成链接器输入。`.cppm` 文件按扩展名识别，不受影响，所以现有 `cmake-msvc` 夹具通过 | E3、E7 |
| G4 | clang++ 以 MSVC ABI 构建时，探测不到标准库：`-print-library-module-manifest-path` 答不出 MSVC STL 的清单 | `src/toolchain/probe.cpp` 的 `probe_clang`；E5 |
| G5 | clang-cl 的工具集目录由驱动路径向上推四级得到，这只对 cl.exe 成立；LLVM 或 Visual Studio 自带的 clang-cl 得到错误目录 | `src/toolchain/probe.cpp` 的 `probe_msvc` |
| G6 | clangd 23.1.0 编译 MSVC STL 的 `std` 模块报 `reference to 'align_val_t' is ambiguous`，clang 22.1.8 没有这个问题 | E8、E9 |
| G7 | clangd 构建模块失败、或 SDK 缺失时，服务端既不报告对应问题，也不停止把相关文件交给 clangd：诊断为空，跳转、悬停等请求一直返回空结果；macOS 上状态仍是 `ready` | E3、E15 |

**方案：**

1. **Visual Studio 事实**（新模块 `lspmcpp.toolchain.msvc`）。输出安装根目录、工具集版本与目录、cl.exe 路径、`modules/` 是否存在、Windows SDK 根目录与版本。查找顺序：
   - 环境变量 `VCToolsInstallDir`、`WindowsSdkDir`、`WindowsSDKVersion`，存在即采用；
   - 构建记录的 cl.exe 路径，先转成长文件名（CMake 记录的是 `C:\PROGRA~1\...\1444~1.352\bin\Hostx64\x64\cl.exe`），再推出工具集目录；
   - `vswhere.exe -products * -prerelease -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json`，取最新实例中带 `modules\std.ixx` 的最新工具集；
   - Windows SDK 取 `Windows Kits\10` 下同时具备 `Include\<版本>\ucrt\corecrt.h` 与 `Lib\<版本>\um\x64\kernel32.lib` 的最新版本，与 mcpp 的规则一致。

   结果按 vswhere 输出与目录修改时间缓存在 `toolchains/probe.json`。
2. **标准库清单的两种形状。** `spec::read_module_metadata` 同时接受 P3286 形状与 MSVC STL 形状。MSVC 形状的 `module-sources` 中每个文件做一次词法扫描，得到 `std`、`std.compat` 两个模块。S1 6.1 节与 S4 同步说明两种形状。
3. **引擎参数显式给出工具集与 SDK。** 显式参数优先于 `INCLUDE`（E11），并且足以让 clangd 在普通环境中工作（E10），因此总是传入：
   - cl 模式（P6、P7、W2）：`/vctoolsdir`、`/winsdkdir`、`/winsdkversion`；
   - GNU 模式、MSVC 目标（P5）：`-Xmicrosoft-visualc-tools-root`、`-Xmicrosoft-windows-sdk-root`、`-Xmicrosoft-windows-sdk-version`。
4. **`std` 单元的注入：** 来源是第 2 条解析出的清单条目，参数沿用现有做法取上下文中一个代表单元的参数，另外：
   - cl 模式（P6、P7、W2）：把 `std.ixx`、`std.compat.ixx` 复制到服务端缓存目录的 `stdlib/msvc-stl-<工具集版本>/std.cppm` 与 `std.compat.cppm`，按源文件的大小与修改时间刷新，引擎数据库引用副本。`.cppm` 扩展名让驱动按模块接口单元处理（G3、E7）；副本中的 `#include` 仍解析到工具集的原始头文件，所以跳转落在原始头文件上。
   - GNU 模式（P5）：直接引用原路径，加 `-x c++-module`。
   - 两种模式都加 `-Wno-reserved-module-identifier` 与 `-Wno-include-angled-in-module-purview`，与 mcpp 构建 MSVC STL `std` 模块时的参数一致。
   - 上下文中所有使用 MSVC STL 的单元都关闭对齐分配，而不只是 `std` 单元：cl 模式 `/Zc:alignedNew-`，GNU 模式 `-fno-aligned-allocation`。这是 G6 的绕过办法，关闭后 clangd 23.1.0 在普通环境中构建出完整模块链（E10）。按 clang 19 的 `LangOptions.def`，`AlignedAllocation` 是普通语言选项，导入模块时要求一致，所以整个上下文统一加。语义上的代价是过对齐类型的 `new` 表达式不再选择带 `std::align_val_t` 的重载。是否需要这一选项记在 W9.5 的能力表中，按 clangd 版本决定。
5. **构建记录中已有的 `std` 单元。** CMake 的 `import std` 会把 `std.ixx`、`std.compat.ixx` 作为 `__cmake_cxx23` 目标的单元写进编译数据库（E2）。服务端丢弃这些条目，统一按第 4 条从清单注入，避免同一模块两个提供者，也避免依赖构建系统的写法。
6. **clang++ 以 MSVC ABI 构建（P5）。** 目标三元组以 `-windows-msvc` 结尾时，标准库固定为 `msvc-stl`，清单来自第 1 条的工具集；mcpp 在 Windows 上的默认工具链正是这种组合（E5）。
7. **模块构建失败的处理（G7）：**
   - 服务端读取 clangd 日志中的 `Failed to build module <名字>`，把该模块及传递依赖它的文件记入问题项 `module-build-failed`，状态降为 `degraded`，问题项附带“显示日志”命令；
   - 这些文件从引擎数据库中移出，请求就地应答，与导入无法解析时的处理一致；
   - 转发请求的截止时间从 60 秒降到 10 秒（跳转、悬停、补全），索引类请求保持 60 秒。
8. **夹具**（Windows 普通环境）：
   - `cmake-msvc-std`：P7，CMake 3.31 + cl.exe + `import std`；
   - `mcpp-msvc`：mcpp `msvc@system`；
   - `mcpp-llvm-msvc`：P5，mcpp 在 Windows 上的默认 llvm 工具链；
   - `compdb-clangxx-msvc-std`：P5，编译数据库由夹具生成；
   - `compdb-clang-cl-std`：P6，编译数据库由夹具生成；
   - 现有 `cmake-msvc` 改为普通环境运行。

   P6 暂无 CMake 夹具：CMake 3.31.6 与 4.1.3 不支持 clang-cl 的模块扫描（E6）。CMake 4.4 按代码支持 clang-cl 19.1 及以上，实施时在 windows-2022 上安装 CMake 4.4 实测，通过则增加 `cmake-clang-cl`。P5 与 P6 的 `import std` 都不能经 CMake 构建，因为 CMake 的 clang `import std` 只支持 libc++ 与 libstdc++；这两项由 `compdb-*` 夹具在准备步骤中直接调用编译器构建（E7 已实测这些命令），再写出编译数据库。

**改动位置：** `src/toolchain/msvc.cppm/.cpp`（新增）、`src/toolchain/probe.cpp`、`src/spec/metadata.cpp`、`src/normalize/msvc.cpp`、`src/normalize/gnu.cpp`、`src/normalize/plan.cpp`、`src/server/session.cpp`、`specs/s1-build-database.md`、`specs/s4-semantic-kit.md`、`conformance/fixtures/`、`tests/test_normalize.cpp`、`tests/test_toolchain.cpp`、`tests/test_spec.cpp`。

**验证：**

- 单元测试覆盖三部分：两种清单形状；Visual Studio 事实的查找顺序，用临时目录搭出假的安装树与 SDK 树；P5、P6、P7 三种参数翻译，包括 `std` 单元。
- 第 8 条的夹具在 windows-2022 上以普通环境运行。

**退出标准：** 上述夹具全部通过；`cmake-msvc-std` 与 `mcpp-msvc` 的状态为 `ready`，问题项为空。

### W2 Visual Studio 自动切换

**目标：** D27 与 U5。

**现状：** 装有 Visual Studio、没有构建系统的工作区在 windows-2022 上使用 MinGW 语义工具包（E12）。设计 14.3 节的实现补充写明，没有构建系统时 MSVC 不参与选择；这与设计 9.3 节第 5 条不一致，D27 决定按 9.3 节实现。

**方案：**

1. **选择顺序：** 推断来源（没有构建系统）在 Windows 上依次尝试：
   - `lspMcpp.compiler` 设置；
   - 第 W1.1 条找到的、带 `modules\std.ixx` 的 Visual Studio 工具集；
   - 其余发现的编译器中带模块清单的；
   - 语义工具包。
2. **语义配置：** `profile.kind` 为 `build-toolchain`，`compiler` 为 `msvc 19.44.35228`，`stdlib` 为 `msvc-stl 14.44.35207`，target 为 `x86_64-pc-windows-msvc`。引擎参数取 cl 模式：`/std:c++latest /EHsc /permissive-`，加上 W1.3 的路径参数与 W1.4 的对齐分配参数；`std` 单元按 W1.4 注入。
3. **回退：** Visual Studio 没有 C++ 工具集，或工具集没有 `modules\std.ixx` 时，使用语义工具包。语言状态项的悬停写明原因，例如“Visual Studio 2022 的 MSVC 14.29 没有 std 模块，需要 14.38 或更新”；这种情况不算降级。
4. **不受信任的工作区：** 不运行 `vswhere.exe`，直接使用语义工具包，与设计 18 节“执行外部程序只在受信任工作区”一致。
5. **覆盖：** `lspMcpp.compiler` 设为 `kit` 时强制使用语义工具包，不新增设置项。

**验证：** 单元测试覆盖选择顺序；夹具 `inferred-msvc`（windows-2022 普通环境，断言 `profile.compiler` 以 `msvc` 开头且 `import std` 可解析）；W5 的无 Visual Studio 模拟中，同一工作区回退到语义工具包。

**退出标准：** 两个环境的结果都符合预期。

### W3 mcpp 生产方与 S2 0.2

**目标：** mcpp 工程的模型达到 S1 等级 3，并且服务端不再为 mcpp 工程写工程目录（SC6 的 mcpp 部分，设计 1.2 节“不留痕”）。

**现状：**

- 服务端先运行 `mcpp emit build-database --format jsonl`，失败后运行 `mcpp build --configure-only`，读取它写在工程根目录的 `compile_commands.json`（`src/project/mcpp.cpp`）。状态中的等级为 2。
- mcpp 没有 `emit build-database`；`emit` 只有 `xpkg`。`--configure-only` 按 mcpp 文档是配置操作，会写 `compile_commands.json`、构建目录与锁文件元数据。
- mcpp 已有机器输出协议 v1（mcpp#385，文档 `docs/50-machine-output.md`）：`--format json` 输出信封，`ndjson` 保留给流式场景；`mcpp --protocol-version` 声明每个命令的效应（effects），客户端据此在执行前判断信任。S2 0.1 的“stdin 请求 + JSONL 消息 + 生产方写数据库文件”与这套约定不一致。

**方案：**

1. 向 mcpp 提功能需求（mcpp-community/mcpp#636），契约如下，细节以 issue 为准：
   - 命令 `mcpp emit build-database --format json`，信封 `kind` 为 `mcpp.build-database`，`data` 为 `{ database, watch, inputs-fingerprint }`；`database` 是完整的 S1 等级 3 文档。
   - 解析过程与 `mcpp build --configure-only` 相同，可以联网、写全局缓存、运行 `build.mcpp`，这些效应在 `--protocol-version` 中如实声明；**不写工程目录**，也就是不含 `write-project` 效应，不写 `compile_commands.json`、`target/` 与 `mcpp.lock`。
   - 数据库只出现在标准输出中，由消费方决定写到哪里。
   - 依赖包提供的 `std` 模块（例如 openkal-llvm-runtime 的 `llvm-generated/std.cppm`）没有清单文件，作为翻译单元输出（E18）。
   - 第二阶段启用 `--format ndjson`：若干进度信封，最后一个是 `mcpp.build-database` 信封。
2. mcpp 维护者确认命令名与字段后，由本项目按 mcpp 贡献规范实现并提 PR（issue、分支、单元测试与 e2e、`docs/50` 的 kind 章节），CI 通过、发布、mcpp-index 收录后，lsp-mcpp 升级 CI 中的 `MCPP_VERSION`。
3. lsp-mcpp 消费端：
   - 先读 `mcpp --protocol-version`（按 mcpp 可执行文件的路径、大小、修改时间缓存），`kinds` 中有 `mcpp.build-database` 时运行新命令，从信封取 `data.database` 与 `data.watch`，写入服务端缓存目录。
   - 没有该 kind 时保留 `--configure-only` 回退，并在状态中增加一个不降级的提示项 `producer-writes-project`，说明这个 mcpp 版本会写 `compile_commands.json`。
4. S2 升到 0.2：新增单文档模式，发现命令的标准输出是一个信封，数据库内联在 `data.database` 中；0.1 的 JSONL 模式保留给其他生产方。Schema 与示例同步更新。

**改动位置：** mcpp 仓库（新命令、S1 写出、测试与文档）；lsp-mcpp 的 `src/project/mcpp.cpp`、`src/spec/discovery.cppm/.cpp`、`specs/s2-discovery.md` 与 Schema、`conformance/fixtures/mcpp-*`。

**验证：**

- 一致性运行器新增检查种类 `workspace-unchanged`：准备步骤之后记录工作区的文件清单与内容哈希，场景结束时比对。
- `status` 检查支持 `level`。`mcpp-gcc`、`mcpp-llvm`、`mcpp-msvc`、`mcpp-llvm-msvc` 断言等级 3 与工作区不变。
- 服务端单元测试覆盖信封解析、kind 缺失时的回退与提示项。

**退出标准：** 三个主机上的 mcpp 夹具断言等级 3 且工作区不变并通过；使用的是 mcpp 正式发布的版本。

### W4 CMake 构建数据库

**目标：** CMake 自身导出的 `build_database.json` 经适配达到等级 2（SC6 的 CMake 部分）。

**现状：**

- 识别逻辑优先读取构建目录中的 `build_database.json`（`src/project/detect.cpp`）。本地以 CMake 4.4.2 生成的文件测试，`lsp-mcpp model` 能读出 set、可见性与模块关系（E16）；没有端到端夹具。
- CMake 私有配置传了 `-DCMAKE_EXPORT_BUILD_DATABASE=ON`（`src/project/cmake.cpp`），没有传实验特性开关，也不构建 `build_database.json` 目标。E16 实测：开关的 UUID 随 CMake 版本变化，UUID 不对时生成步骤失败；默认目标不生成合并后的文件。

**方案：**

1. 私有配置按 `cmake --version` 查表传入 UUID（先收录 4.4.2 的 `70ef007e-b743-492d-9407-e35eeac03a40`，其余版本在 CI 中逐个测得后补入），配置后构建 `build_database.json` 目标；版本不在表中时不传开关，退回编译数据库与 `.modmap`。
2. 适配层把 CMake 的 P2977 形状文档读成等级 2 模型：`provides`、`requires` 直接取用，角色由源码扫描补全，工具链事实由探测补全。
3. 新增夹具 `cmake-clang-bdb`（Linux）与 `cmake-msvc-bdb`（Windows 普通环境），使用 CMake 4.4，断言来源为构建数据库、等级 2，检查项与 `cmake-clang`、`cmake-msvc` 相同。CI 的 Linux 与 Windows 机器上 CMake 为 3.31.6，夹具任务先安装 CMake 4.4。

**退出标准：** 两个夹具在对应主机通过；CMake 版本不在对应表中时，私有配置不传开关，并有单元测试覆盖。

### W5 干净机器与首次使用

**目标：** U6、U7。设计 20.2 节要求干净机器测试，SC2 要求端到端证据。

**现状：** 没有干净机器任务，也没有 Command Line Tools 询问。一致性层面，不装编译器的 Linux 容器与隐藏了 Visual Studio 的 Windows 全部通过（E13、E14）；移走 Command Line Tools 的 macOS 上状态仍为 `ready`，`sdk-missing` 不带命令，7 项检查失败（E15）。

**方案：**

1. **Linux：** 在 `ubuntu:24.04` 容器中运行（不装任何编译器，只装 VS Code 运行所需的图形库与 `xvfb`）：一致性夹具 `inferred-discover`（发现开启，不带 `--no-discover`），以及 VSIX 形态的端到端测试。
2. **Windows：** GitHub 的 Windows 机器无法卸载 Visual Studio，采用隐藏的方式模拟：改名 `Microsoft Visual Studio\Installer`（`vswhere.exe` 所在目录），PATH 只保留 `System32` 等系统目录，再运行同样的夹具与端到端测试。任务开头断言 `vswhere.exe`、`cl`、`clang-cl`、`clang++`、`g++` 都不可见。
3. **macOS：** 以 `sudo mv` 移走 `/Library/Developer/CommandLineTools` 与 `/Applications/Xcode*.app`，执行 `xcode-select --reset`，断言 `xcrun --show-sdk-path` 失败，再运行夹具与端到端测试。
4. **Command Line Tools 询问（U7）：**
   - 找不到 SDK 时状态降为 `degraded`，使用 `std` 的文件按 W1.7 的方式移出引擎数据库并就地应答，不再一直返回空结果。
   - 服务端的 `sdk-missing` 问题项带命令 `lspMcpp.installCommandLineTools`。
   - 扩展收到该问题项时询问一次（记在 `globalState`，每台机器一次）；用户同意后运行 `xcode-select --install`。
   - 服务端每 30 秒检查一次 SDK 目录，出现后重新探测并刷新模型，不需要重启。
   - 端到端测试在隐藏了 Command Line Tools 的机器上断言：询问恰好一次，第二次激活不再询问。

**退出标准：** 三个干净机器任务在每次提交时运行并通过。

### W6 零打扰断言与 VSIX 安装形态

**目标：** SC3 与 U10 有端到端证据，并且测的是用户实际安装的包。

**现状：** 端到端测试只断言通知数为零，冲突检查在测试模式下跳过（`editors/vscode/test/suite/modules.test.ts`）；扩展以开发路径加载，没有测过 VSIX 安装后的形态。

**方案：**

1. **界面计数：** 测试模式下，扩展导出的测试接口统计扩展自己发起的 `show*Message`、`createStatusBarItem`、`OutputChannel.show`、`createWebviewPanel`、`showTextDocument` 调用次数。端到端测试断言全部为零，`createLanguageStatusItem` 恰好一次。
2. **工作区不变：** 打开前记录工作区文件清单与哈希，场景结束时比对。W3 完成前，mcpp 夹具的例外只有 `compile_commands.json` 与 `target/`，写在测试里；W3 完成后删除例外。
3. **VSIX 形态：** 端到端任务先打包 VSIX，再以 `--install-extension <vsix> --extensions-dir <临时目录>` 装入全新的 VS Code；测试代码放在一个只含测试的扩展里加载。
4. **冲突处理（U10）：**
   - 构造两个只有 `package.json` 的替身扩展，ID 分别为 `ms-vscode.cpptools` 与 `llvm-vs-code-extensions.vscode-clangd`，声明对应的设置项，打包后安装。
   - 测试接口替换询问框的应答。
   - 断言询问一次、只改 `C_Cpp.intelliSenseEngine` 与 `clangd.enable` 两项工作区设置；重新激活后不再询问；拒绝的应答被记住。

**退出标准：** 三个主机上的端到端任务以 VSIX 形态运行，上述断言全部通过。

### W7 性能

**目标：** 设计 1.3 节的冷启动与温启动指标有测量值；SC4（温启动不重建 `std`）有断言。

**现状：** 第 3.1 节与 E19 的冷启动数据来自一致性日志，小工程到首次跳转 4.6–12.0 秒；mcpp 仓库的首次跨模块跳转 95.4 秒（E17）。运行器每次创建新缓存目录，没有温启动测量，也没有基准任务。

**方案：**

1. **测量：**
   - 一致性运行器新增 `--cache-dir <目录>`，跨次运行复用服务端缓存。
   - 新增 `--measure <文件>`，写出 JSON 计时：`initialize`、首次 `ready`、首个诊断、首次跳转。
   - SC4 的断言：温启动前后，引擎模块缓存目录中 `std` 的 BMI 修改时间不变。
2. **优化，按测量结果决定是否需要：**
   - 模型加载完成后，立即在后台为每个语义配置预构建 `std`，不等第一个文件打开。做法是在引擎数据库中加入缓存目录里的一个 `import std;` 单元并打开它。
   - 大工程上按模块图的拓扑顺序在后台预构建被导入最多的模块，目标是把 E17 的 95.4 秒移到打开文件之前，并在语言状态项上显示进度。
   - 同一语义配置在不同工作区之间共享 `std` 的 BMI。clangd 23.1 的持久化模块缓存以完整命令为键（实验记录 `2026-09-13-cxx-modules-lsp-experiments.md` 的 E12），先实验确认共享目录的可行性，再实现。
3. **门槛：**
   - 每次提交的 CI 只拦严重退化：冷启动超过 15 秒、温启动超过 2 秒即失败。
   - nightly 在三个主机各跑三次，记录中位数。
   - 设计指标（冷启动 5 秒、温启动 1 秒）在本地开发机上测量，写进执行记录，并注明硬件。

**退出标准：** nightly 有连续三次的计时记录；SC4 断言在三个主机通过；本地测量达到设计指标，或者在设计文档中修订指标并写明依据。

### W8 自举与大工程

**目标：** SC7 与 U8。

**现状：** mcpp 仓库 7 项检查通过，首次跳转 95.4 秒（E17）；lsp-mcpp 仓库失败，`std` 来自 openkal-llvm-runtime 包，服务端找不到它的源文件（E18）。

**方案：**

1. 新增夹具 `self-lsp-mcpp` 与 `self-mcpp`，沿用探测时的写法：准备步骤按固定提交获取仓库（`git fetch --depth 1 <仓库> <提交>`）。检查项覆盖：
   - `src/main.cpp` 无诊断；
   - 跨目录、跨包的声明跳转与模块名跳转；
   - 悬停、补全、引用；
   - 模块图包含主要模块。
2. lsp-mcpp 仓库的 `std` 由 W3 解决：mcpp 把运行时包的 `std.cppm` 作为翻译单元输出。W3 落地前，服务端从 mcpp 构建目录中构建 `std.pcm` 的命令读取源文件路径作为过渡，是否可行以 mcpp 的实现为准，实施前先确认。
3. nightly 在 Linux 上运行两个夹具，在 macOS 上运行 `self-lsp-mcpp`，并记录 W7 的计时。
4. Windows 主机上自举 lsp-mcpp 需要 `--target x86_64-windows-gnu`，而 IDE 目前没有渠道把目标传给 mcpp。第一版不在 Windows 上自举，记为限制，mcpp issue 中已列为后续需求。

**退出标准：** nightly 中两个夹具通过，计时有记录；mcpp 仓库首次跳转的时间在 W7 的优化后重新测量并写进执行记录。

### W9 服务端完备项

| 编号 | 事项 | 现状 | 方案 | 验证 |
|---|---|---|---|---|
| W9.1 | 多根工作区（U9） | 只使用第一个工作区文件夹（`src/server/session.cpp` 的 `workspace_root_`） | 每个根一个工程模型与一个 clangd 进程；请求按文档路径的最长根前缀路由；处理 `workspace/didChangeWorkspaceFolders`。S3 的 `cxxModules/status` 改为每个根发送一条，`project.root` 区分，这是向后兼容的补充 | 运行器支持多个文件夹；夹具 `multi-root`（`inferred` 与 `mcpp-llvm` 两个根） |
| W9.2 | 上下文与多 target（U9） | `cxxModules/setContext` 已实现，没有夹具覆盖两个 set 共享同一文件的情形 | 保持现有实现，补测试 | 夹具 `s1-two-sets`：工作区自带 S1 文档，两个 set 以 `-DVARIANT=1` 与 `-DVARIANT=2` 编译同一文件；运行器新增检查种类 `set-context`，切换后悬停内容随之改变 |
| W9.3 | 文件监视回退 | 只向客户端动态注册监视；客户端不支持动态注册时，构建描述文件的变化无人通知（设计 12.3 节第 5 条要求低频检查） | 客户端不声明 `didChangeWatchedFiles.dynamicRegistration` 时，工作线程每 2 秒比较监视列表中文件的大小与修改时间，变化进入主线程消息队列，与编辑器通知走同一处理 | 运行器选项去掉该能力；新增检查种类 `write-file`，写入新的模块文件后模块图在 10 秒内包含它 |
| W9.4 | 负载完整性（设计 18 节） | 运行时不校验负载 | `payload.json` 增加 `files`，列出 clangd 可执行文件与工具包清单的大小与 sha256，由 `assemble_payload.py` 生成。服务端启动时比较大小；sha256 按（路径、大小、修改时间）只算一次并缓存。不一致时状态为 `error`，问题项 `payload-corrupt` 带“重新安装扩展”的说明 | 单元测试；夹具 `payload-corrupt` 复制负载并截断 clangd |
| W9.5 | 引擎接口与能力表（设计 15.1 节） | 只有 `lspmcpp.engine.clangd`，启动参数写死 | 新增 `lspmcpp.engine` 接口：启动、推送数据库、转发 LSP、能力查询。能力表按 clangd 版本列出 `--experimental-modules-support`、`--use-dirty-headers`、持久化模块缓存，以及 MSVC STL 上下文是否需要关闭对齐分配（W1.4） | 路由与会话的单元测试改用假引擎，不再依赖真实 clangd |

**退出标准：** 表中的验证全部进入 CI 并通过。

### W10 规范与一致性对应

| 编号 | 事项 | 方案 |
|---|---|---|
| W10.1 | `specs/README.md` 声称 CI 校验每个示例，实际没有这一步 | CI 增加 `spec-schemas` 任务，用 Python `jsonschema` 校验 `specs/examples/` 下的全部示例；脚本入库为 `specs/tools/validate.py` |
| W10.2 | 设计 1.3 节要求每条规范规则至少一个用例，目前没有对应表 | S1 至 S4 中每条 MUST、SHOULD 规则编号，例如 `S1-6.1-2`。`conformance/traceability.json` 把规则编号映射到夹具检查或单元测试名；CI 脚本检查没有遗漏的规则，也没有指向不存在的检查 |
| W10.3 | 本方案带来的规范改动 | S1 6.1 节：`module-metadata` 可以指向 MSVC STL 自带的 `modules.json`（W1），`std` 也可以由数据库中的翻译单元提供，此时省略 `module-metadata`（W3、W8）；S2 0.2 单文档模式（W3）；S3 多根状态（W9.1）；S4 不变 |

**退出标准：** `spec-schemas` 与对应表检查在 CI 中通过。

### W11 交叉构建产物的原生验证

**目标：** SC8，即服务端全部平台的二进制由一台 Linux 主机交叉构建，并在各平台原生通过测试。

**现状：** `cross-build` 任务只检查产物格式；Windows 与 macOS 负载中的服务端是在各自主机上原生构建的。

**方案：**

1. `cross-build` 以 release 配置构建 `lsp-mcpp` 与 `lsp-mcpp-conformance`，作为产物上传。
2. win32-x64 与 darwin-arm64 的负载任务改为使用这些产物，不再在本机构建服务端。
3. 一致性、干净机器与端到端任务因此运行的都是 Linux 交叉构建的二进制。
4. `build-test` 任务保留原生构建与两种配置的单元测试，用来发现只在原生构建中出现的问题。
5. 负载组装时比对服务端的 sha256 与交叉构建产物一致。

**退出标准：** 三个平台的负载都来自 Linux 交叉构建，下游任务全部通过。

## 5. 验证体系

### 5.1 夹具矩阵

“普通环境”指服务端不继承开发者命令行环境（D30）。“模拟”指第 W5 项中隐藏编译器、Visual Studio 或 Command Line Tools 的做法。

| 夹具 | 覆盖 | Linux | macOS | Windows | 状态 |
|---|---|---|---|---|---|
| `inferred` | K1–K3，`--no-discover` | 运行 | 运行 | 运行 | 已有 |
| `inferred-discover` | 发现开启时的选择 | 干净容器 | 模拟无 Command Line Tools | 模拟无 Visual Studio | 新增 |
| `inferred-msvc` | U5，D27 | — | — | 普通环境 | 新增 |
| `untrusted` | 受限模式 | 运行 | 运行 | 运行 | 已有 |
| `mcpp-gcc` | P1，等级 3 | 运行 | — | — | 已有，加断言 |
| `mcpp-llvm` | P3、P4，等级 3 | 运行 | 运行 | — | 已有，加断言 |
| `mcpp-msvc` | mcpp 的 `msvc@system` | — | — | 普通环境 | 新增 |
| `mcpp-llvm-msvc` | mcpp 的 Windows 默认工具链（P5） | — | — | 普通环境 | 新增 |
| `mingw` | P2 | 运行 | — | 运行 | 已有 |
| `cmake-clang` | CMake + Clang | 运行 | — | — | 已有 |
| `cmake-clang-bdb` | CMake 构建数据库 | 运行 | — | — | 新增 |
| `cmake-msvc` | P7 | — | — | 普通环境 | 已有，改环境 |
| `cmake-msvc-std` | P7 + `import std` | — | — | 普通环境 | 新增 |
| `cmake-msvc-bdb` | CMake 构建数据库 | — | — | 普通环境 | 新增 |
| `cmake-clangxx` | P5，CMake + clang++（MSVC ABI） | — | — | 普通环境 | 新增 |
| `cmake-clang-cl` | P6，CMake 4.4 + clang-cl | — | — | 普通环境 | 新增，CMake 4.4 实测通过后才加入（W1.8） |
| `compdb-clangxx-msvc-std` | P5 + `import std`，不经 CMake | — | — | 普通环境 | 新增 |
| `compdb-clang-cl-std` | P6 + `import std`，不经 CMake | — | — | 普通环境 | 新增 |
| `s1-two-sets` | 上下文切换 | 运行 | — | — | 新增 |
| `multi-root` | 多根工作区 | 运行 | 运行 | 运行 | 新增 |
| `watch-polling` | 监视回退 | 运行 | — | — | 新增 |
| `payload-corrupt` | 负载完整性 | 运行 | — | — | 新增 |
| `self-lsp-mcpp`、`self-mcpp` | SC7 | nightly | nightly（仅 `self-lsp-mcpp`） | — | 新增 |

### 5.2 CI 布局

**`ci.yml`（每次提交）：**

| 任务 | 主机 | 内容 | 变化 |
|---|---|---|---|
| build and unit tests | 三个 | 原生构建，dev 与 release 两种配置的单元测试 | 不变 |
| cross-build | ubuntu-24.04 | 交叉构建 Windows 与 macOS 的 release 服务端与运行器并上传 | 上传产物（W11） |
| spec-schemas | ubuntu-24.04 | 示例校验与规则对应表检查 | 新增（W10） |
| payload | 三个 | 组装负载；Windows 与 macOS 使用交叉构建的服务端 | 改来源（W11） |
| conformance | 三个 | 第 5.1 节中标为运行、普通环境的夹具 | 新增夹具；Windows 服务端改为普通环境 |
| clean machine | 三个 | 干净容器或模拟环境中的 `inferred-discover` 与端到端子集 | 新增（W5） |
| VS Code end to end | 三个 | VSIX 形态、界面计数、工作区不变、冲突处理 | 扩充（W6） |

**`nightly.yml`（每晚与手动触发）：** 自举夹具（W8）；三个主机的冷启动与温启动计时，各三次，结果作为产物上传（W7）；用 mcpp 最新发布版运行 mcpp 夹具，提前发现生产方的不兼容。

### 5.3 本地验证

本地开发机是 Linux。

| 范围 | 命令或做法 |
|---|---|
| 构建与单元测试 | `mcpp build`；`mcpp test`；`mcpp test --profile release` |
| Windows 平台层 | `mcpp build --target x86_64-windows-gnu` 后在 Wine 下运行单元测试与 `inferred` 夹具（Wine 的短名形如 `PROG~FBU`） |
| Linux 夹具 | `lsp-mcpp-conformance run --server <服务端> --payload editors/vscode/payload --fixture conformance/fixtures/<名字>` |
| 干净机器 | 与 CI 相同的 `docker run ubuntu:24.04` 命令 |
| 规范 | `python3 specs/tools/validate.py` |
| MSVC 家族与 macOS | 本地无法运行；在 windows-2022 与 macos-14 上以一次性分支运行，做法与第 3.2 节的探测相同 |

## 6. 顺序与依赖

| 阶段 | 工作项 | 依赖 | 退出标准 |
|---|---|---|---|
| A | W1、W2，以及 W10.3 中 S1 的改动 | 无 | Windows 普通环境中 MSVC 家族夹具与 `inferred-msvc` 全部通过 |
| B | W5、W6 | W2（Windows 干净机器任务要区分有无 Visual Studio） | 三个主机的干净机器任务与 VSIX 形态端到端通过 |
| C | W3、W4，以及 W10 其余部分 | W3 依赖 mcpp 维护者确认 issue，并需要一次 mcpp 发布 | SC6 |
| D | W7、W8、W9 | W7 的优化可能用到 W9.5 的能力表 | SC4、SC7；W9 的夹具全部通过 |
| E | W11，以及收口 | 以上全部 | SC8；设计文档、执行记录与 issue #2 更新；CI 全部通过后汇报 |

阶段 C 中 mcpp 的部分与 lsp-mcpp 并行：mcpp PR 合入发布前，lsp-mcpp 的消费端先用本地构建的 mcpp 验证；合入发布后再切换 CI 使用的版本。

交付方式沿用最初目标：继续在 PR #1 上提交，每个阶段结束时 CI 全部通过。

## 7. 风险

| 风险 | 影响 | 应对 |
|---|---|---|
| GitHub 的 Windows 镜像升级 Visual Studio。mcpp 文档记录了 clang 与 MSVC STL 14.51 组合下 `std::find` 编译失败（microsoft/STL#6294） | P5、P6 夹具在新镜像上失败，而原因不在 lsp-mcpp | 固定 `windows-2022`；夹具日志记录工具集版本；新镜像作为 nightly 的单独一行 |
| 用隐藏代替卸载模拟无 Visual Studio 的机器，注册表与环境变量中可能留有痕迹 | 模拟不完全 | 任务开头断言 `vswhere.exe` 与所有编译器不可见；GitHub Actions 没有真正干净的 Windows 机器，接受这一限制并写进执行记录 |
| mcpp 维护者选择的命令形态与第 W3 项不同 | 消费端返工 | 消费端的差异集中在 `spec::discovery`，按确认后的契约调整 |
| CI 机器比开发机慢，计时波动大 | 性能门槛误报或漏报 | 每次提交只拦严重退化；设计指标在本地测量 |
| Windows 上构建 `std` 的夹具多，CI 时间变长 | 反馈变慢 | 夹具默认检查超时从 180 秒降到 60 秒，服务端按 W1.7 尽快应答；自举等重的夹具放到 nightly |
| clangd 23.1.0 与 MSVC STL 的 `align_val_t` 二义性（E8、E9） | 关闭对齐分配是绕过办法；以后的 MSVC STL 或 clangd 补丁版本可能改变行为 | W9.5 的能力表按 clangd 版本决定是否加这一选项；Windows 夹具在 nightly 中加一行最新镜像；最小复现已经具备（E9 的两条命令），向 LLVM 报告的时间见第 9 节 |

## 8. 不在本方案内的事项

- 预发布与正式发布（D28）：Marketplace、Open VSX、GitHub Releases，以及 xim-pkgindex 描述文件（issue #2）。
- mcpp-vscode 声明依赖 lsp-mcpp 扩展（设计 16.6 节）：要等扩展发布之后。
- Linux 与 Windows 的 arm64、Apple clang、header units、clice（设计第 21 节“之后”）。
- openkal 体系中只记录的事项：K2、K5、K8，以及需求 K3、K4（issue #2）。它们不在任何场景 U1–U10 的路径上。
- 向 clangd 上游贡献（D14）。

## 9. 需要 review 的要点

1. **可用的判定标准（第 2 节）。** 十个场景 U1–U10 取代“夹具在准备好的环境中通过”。关键变化：Windows 夹具的服务端改为不带开发者环境运行（D30）；端到端测试改为安装 VSIX；增加干净机器与自举。
2. **MSVC STL `import std` 的做法（W1）。**
   - S1 与服务端同时接受 MSVC STL 自带的清单形状；
   - cl 模式把 `std.ixx` 复制为缓存目录中的 `.cppm` 副本；
   - 引擎参数显式带工具集与 SDK 路径，不依赖开发者环境；
   - MSVC STL 上下文统一关闭对齐分配，绕过 clangd 23.1.0 的二义性，代价是过对齐 `new` 的重载选择；
   - 构建系统记录的 `std` 单元一律丢弃，由清单注入。
   需要确认：可以接受关闭对齐分配这一语义代价。
3. **P6 的夹具形态（W1.8）。** CMake 3.31 与 4.1 不支持 clang-cl 模块，P6 以夹具自行构建并写出的编译数据库为主；CMake 4.4 实测通过后再加 CMake 夹具。需要确认：P6 以编译数据库夹具作为第一版的验收证据。
4. **Visual Studio 自动切换（W2）。** 没有构建系统时，Windows 上 Visual Studio 优先于其他发现的编译器；工具集没有 `std.ixx` 时回退语义工具包，不算降级；不受信任的工作区不运行 `vswhere.exe`；`lspMcpp.compiler` 取值 `kit` 强制使用语义工具包，不新增设置项。
5. **mcpp 契约（W3，issue mcpp-community/mcpp#636）。**
   - 命令不写工程目录，可能联网、写全局缓存、运行 `build.mcpp`，这些效应如实声明；
   - 数据库只出现在标准输出；
   - 依赖包提供的 `std` 以翻译单元输出。
   需要确认：由我们实现 mcpp 侧的 PR。
6. **规范改动（W10.3）。** S1 6.1 节接受 MSVC STL 清单形状，并允许 `std` 由库内翻译单元提供；S2 0.2 增加单文档模式；S3 的状态通知改为每个工作区根一条。
7. **性能门槛（W7）。** 每次提交的 CI 只拦严重退化（冷启动 15 秒、温启动 2 秒），设计指标在本地测量；大工程首次跳转（95.4 秒）以后台预构建模块解决。需要确认：接受“CI 门槛宽于设计指标”。
8. **干净机器用模拟（W5）。** Windows 隐藏 Visual Studio，macOS 移走 Command Line Tools 与 Xcode，不使用真正干净的虚拟机。
9. **clangd 回归的上报时机。** D14 把上游贡献放到最后；这个回归直接影响 D26，最小复现已经具备。建议 W1 完成后立即向 LLVM 报告，作为 D14 的例外。
10. **交付方式。** 继续在 PR #1 上按阶段 A→E 提交，每个阶段结束时 CI 全部通过；不做预发布（D28）。

## 10. 执行记录

交付于 Sunrisepeak/lsp-mcpp-private#1（分支 `feat/lsp-mcpp-v1`），2026-09-14。按目标“先用模拟数据保证全部实现”，mcpp 一侧的 `emit build-database` 由 `lsp-mcpp-mock-mcpp` 按 mcpp-community/mcpp#636 的契约模拟；其余工作项都在 lsp-mcpp 中实现，由 CI 在三个主机上验证。

### 10.1 工作项结果

| 工作项 | 结果 | 证据 |
|---|---|---|
| W1 | 完成：两种标准库清单形状；显式传入 Visual Studio 工具集与 Windows SDK；cl 模式的 `.cppm` 副本；MSVC STL 上下文关闭对齐分配；模块构建失败时降级（`module-build-failed`）；交互请求 10 秒应答 | windows-2022 上服务端不带开发者环境运行：`cmake-msvc`、`cmake-msvc-std`、`cmake-clangxx-msvc`、`cmake-clang-cl`、`compdb-clangxx-msvc-std`、`compdb-clang-cl-std`、`mcpp-msvc`、`mcpp-llvm-msvc` |
| W2 | 完成；工具集没有 `std` 模块时状态带提示 `msvc-without-std-module`，不降级 | `inferred-msvc`；单元测试 |
| W3 | 消费端与 S2 0.2 单文档模式完成，生产方由模拟数据验证 | `mcpp-emit`、`mcpp-emit-package-std`（等级 3，工作区不变）、`mcpp-emit-broken`、`mcpp-emit-watch` |
| W4 | 完成：CMake 4.4.2 的开关 UUID 表，私有配置构建 `build_database.json` | `cmake-clang-bdb`（Linux）、`cmake-msvc-bdb`（Windows） |
| W5 | 完成，三个干净机器任务每次提交运行 | Linux：`ubuntu:24.04` 容器中的 `inferred-discover` 与容器中的端到端测试；Windows：隐藏 Visual Studio 后同样两项；macOS：移走 Command Line Tools 与 Xcode 后的 `inferred-no-sdk` 与“只询问一次”的端到端测试 |
| W6 | 完成：VSIX 形态、界面计数、工作区不变、冲突处理 | 三个主机的 VS Code 端到端任务 |
| W7 | 完成：模块准备、模块提示、冷温启动计时与 SC4；门槛与测量见第 10.4 节 | `timing` 夹具（每次提交），nightly 三次中位数 |
| W8 | 完成过渡方案：`std` 取自 mcpp 的 std 构建记录 | `self-lsp-mcpp`、`self-mcpp`（nightly） |
| W9 | W9.1–W9.5 完成 | `multi-root`（三个主机）、`s1-two-sets`、`watch-polling`、`payload-corrupt`；假引擎单元测试 |
| W10 | 完成：示例校验，S1–S4 共 144 条规则编号并有证据 | `specifications` 任务 |
| W11 | 完成：Windows 与 macOS 负载使用 Linux 交叉构建的服务端，组装时比对 sha256 | `payload` 任务及其下游任务 |

### 10.2 与方案不同的做法

| 方案 | 实际 | 原因 |
|---|---|---|
| W1.3：cl.exe 与 clang-cl 的命令以 clangd 的 cl 驱动模式交给引擎 | MSVC 家族的命令统一翻译为 GNU 模式的 clang++ 命令，显式传入工具集、SDK 与 `-fms-compatibility-version` | cl 模式下 `/clang:` 参数排在输入之后，`.ixx` 无法标为模块单元；clangd 的 CommandMangler 还会丢掉未知的 `-x`（E7） |
| W3 退出标准：使用 mcpp 正式发布的版本 | 生产方由 `lsp-mcpp-mock-mcpp` 模拟；`mcpp-gcc`、`mcpp-llvm`、`mcpp-msvc`、`mcpp-llvm-msvc` 仍经真实 mcpp 的 `--configure-only` 得到等级 2，状态带提示 `producer-writes-project` | 本轮目标要求先用模拟数据；mcpp#636 尚未实现 |
| W5.1：Linux 干净机器全部在 `ubuntu:24.04` 容器中运行 | 一致性夹具在 `ubuntu:24.04`；端到端测试在 `node:22-bookworm-slim`，同样不带编译器（任务中断言） | 端到端测试需要 Node；apt 安装图形库时 shared-mime-info 的 `update-mime-database` 逐个文件同步写盘，容器内曾卡满整个任务时限，关闭同步后 15 秒装完 |
| W7：后台预构建 `std`，按拓扑顺序预构建被导入最多的模块 | 每个模块一个 `import M;` 准备单元，导入就绪即打开，等待链最长的先开，准备好的单元保持打开到空闲；引擎数据库写出模块提示，跳过 clangd 对整个数据库的串行扫描；clangd 缓存中已有的模块不再准备；准备单元为每个等待模块的文件保留一个核心，另外总为请求保留一个 | 读 clangd 23.1 源码并实测：全局扫描每个工作线程数秒；已构建的模块只在有打开文件持有时保留；准备单元与打开的文件、编辑与补全共用 clangd 的工作线程，温启动时只会争抢（第 10.4 节） |
| W7：同一语义配置跨工作区共享 `std` 的 BMI | 暂缓，记入 issue #2 | clangd 的持久化缓存以模块源路径与“工作目录 + 完整命令”的哈希为键，`std` 条目继承工程参数，只有参数完全相同的工程才能命中 |
| W7.3：CI 温启动门槛 2 秒 | 5 秒，三个主机相同；冷启动仍为 15 秒 | CI 机器（3–4 个虚拟核心）nightly 三次温启动中位数 Linux 1.96 秒、macOS 1.85 秒、Windows 2.88 秒；重建 `std` 这一严重退化由 SC4 断言拦截 |
| W8：`std` 由 mcpp 以翻译单元输出（W3 之后） | 过渡方案：服务端沿编译数据库中的 `std.pcm` 找到 `build.ninja` 与 mcpp std 构建缓存中的 `std-module.json`（schema 1），把记录的 `std`、`std.compat` 源文件与命令作为单元加入 | 方案要求实施前确认可行性；本地确认 mcpp 2026.9.14.1 写出该记录 |
| 5.2：nightly 以 mcpp 最新发布版运行 mcpp 夹具 | 未做，nightly 使用固定版本，记入 issue #2 | 固定版本之外的一行需要单独维护 mcpp 安装步骤，本轮未排入 |
| 第 7 节：夹具默认检查超时从 180 秒降到 60 秒 | CI 仍传 `--timeout 180`，需要更短时限的检查在场景中写 `"timeout"` | 超时只影响失败时的等待；Windows 上构建 `std` 的夹具首个检查接近 20 秒 |
| 第 9 节第 9 条：W1 完成后向 LLVM 报告 `align_val_t` 回归 | 未提交，记入 issue #2 | 属于对外提交，需先确认作为 D14 的例外 |

### 10.3 方案之外补充的内容

| 内容 | 说明 | 证据 |
|---|---|---|
| S2-5-1 生产方列出的监视输入 | 模型的 `watch` 条目向编辑器动态注册（支持相对模式时用相对模式），轮询回退读取同样的条目；条目变化时重新加载模型；结果与当前模型相同时只替换模型，不重建索引与计划 | `mcpp-emit-watch`，并以 `@polling` 在不支持动态注册的客户端模式下再运行一次 |
| S2-5-9 保留上次成功的模型 | 生产方这次失败时不回退到扫描源码，状态降为 degraded 并带 `model-stale`，说明 mcpp 自己的诊断；再次成功后恢复 ready。能生成数据库的 mcpp 报错时直接报告该错误，不再改用会写工程目录的 `--configure-only` | `mcpp-emit-watch`、`mcpp-emit-broken`（模拟生产方的 `build` 会留下 `compile_commands.json`，工作区检查因此能发现误用） |
| S3 规则编号与两条实现 | S3 的 7 条规则编号；状态中状态不变的更改每 250 毫秒合并发送一次（S3-4-2）；扩展只向声明了 `experimental.cxxModules` 的服务端发 `cxxModules/` 请求（S3-3-2） | `conformance/traceability.json` |
| 多根工作区的两处缺陷 | `cxxModules/status` 的 `project.root` 改为客户端发来的文件夹 URI（macOS 的 `/private/var` 与 Windows 的短文件名使规范化路径与之不同）；`workspace/didChangeWatchedFiles` 的各项按路径分给所属的根（此前 nlohmann 花括号初始化把 URI 变成数组，所有变化都交给了第一个根） | `multi-root` 在三个主机通过，新增的检查在第二个根中写文件并断言该根重新加载 |
| 交互请求等待模块准备 | 文件的模块仍在准备、且最近 10 秒内有模块完成时，跳转、悬停、补全继续等待，每次 5 秒，至多到 60 秒 | 单元测试；mcpp 仓库首次跳转只应答一次（第 10.4 节） |
| 扩展显示提示项 | 语言状态项的悬停在没有问题项时显示第一条提示（`notices`），此前提示不显示 | 代码 `editors/vscode/src/status.ts` |
| 一致性运行器的补全检查 | 带 `insert` 的补全检查用 `split_lines` 切分一个临时字符串，切出的视图悬空，插入后的文本偶尔含 NUL 并被截断；clangd 对错乱的缓冲区作答，检查一直重试到超时。self-lsp-mcpp 的 C6 因此时而耗时数分钟，先前记录的“冷启动约 5 分钟”包含这一部分 | self-lsp-mcpp 连续四次冷启动 C6 在 0.1–0.2 秒内通过 |
| mcpp#636 契约补充 | 失败时的信封、`watch` 的语义与“不写监视中的文件”、重复运行的耗时、路径写法一致、`requires` 的准确性、依赖包 `std` 的临时读取方式，以及用 `mcpp-emit` 夹具验收真实 mcpp 的做法 | mcpp-community/mcpp#636 的评论 |
| macOS 开发者工具占位程序 | 没有 Command Line Tools 时，`/usr/bin` 下的 clang++、c++、xcrun 会弹出安装对话框；发现流程与 SDK 检查改为只看文件系统 | macOS 干净机器任务 |

### 10.4 测量

小工程为 `timing` 夹具（`inferred` 工程，打开即跳转）；“冷”为新缓存目录，“温”为同一工作区与缓存的再次启动；数值为从 `initialize` 到首次跳转应答。自举夹具的首次跳转排在状态进入 ready、两个文件发布诊断之后，包含 mcpp 配置工程的时间。

| 场景 | 机器 | 冷启动 | 温启动 |
|---|---|---|---|
| 小工程 | 开发机 i9-13900K（24 核 32 线程），dev 构建，限定 2 个物理核心 | 2.35–2.49 秒（调整前 2.51） | 0.80 秒（调整前 1.07） |
| 小工程 | 同上，限定 4 个物理核心或不限定 | 2.12–2.14 秒 | 0.81 秒 |
| 小工程 | 同上，限定 1 个物理核心的 2 个线程（clangd `-j=1`） | 4.09 秒（调整前 4.43） | 1.00 秒（调整前 1.81） |
| 小工程 | CI nightly，ubuntu-24.04（4 线程）、macos-14（3 核）、windows-2022（4 线程），三次中位数 | 5.52 / 4.40 / 8.50 秒 | 1.96 / 1.85 / 2.88 秒 |
| 小工程 | 同上机器，调整前（提交 072c5a8）单次 | 5.4 / 3.4 / 9.1 秒 | 2.1 / 1.6 / 4.3 秒 |
| mcpp 仓库（171 个模块，gcc 16） | 开发机，32 线程 | 24.9–26.3 秒；不做模块准备 38.0 秒；调整前 25.6–26.2 秒；E17 为 95.4 秒 | 4.9–5.0 秒（调整前 5.5–5.6） |
| mcpp 仓库 | CI nightly，ubuntu-24.04（4 线程） | 首次跳转 125 秒（其中声明跳转等待 110 秒） | 未测 |
| lsp-mcpp 仓库（依赖 openkal-llvm-runtime） | 开发机，32 线程 | 整个场景 14.5–14.9 秒（不含引用检查），其中 mcpp 配置约 6 秒 | 未测 |
| lsp-mcpp 仓库 | CI nightly，ubuntu-24.04 / macos-14 | 首次跳转 31.6 / 50.0 秒，其中 mcpp 配置与状态进入 ready 25.5 / 29.8 秒 | 未测 |

“调整”指提交 473f082 与 56c1b31：clangd 缓存中已有的模块不再准备；准备单元为每个等待模块的文件保留一个核心，另外总为请求保留一个。在 CI 的三个主机上以 `--log-level debug` 记录的时间线显示，温启动的主要耗时在 clangd 为每个翻译单元扫描所导入模块的源文件（`std.cppm` 包含全部标准库头文件）并校验缓存的 BMI；这部分在 clangd 内部串行进行，服务端只能避免重复（issue #2）。

设计指标（冷启动 5 秒、温启动 1 秒）在开发机上的小工程达到；CI 机器与大工程的实测值写入设计文档第 1.3 节。

### 10.5 上游改动

| 仓库 | 改动 | 版本 |
|---|---|---|
| openkal-musl | #34：Windows 上 mmap 模拟按整页分配（K14：musl mallocng 使用单独映射的大块内存直到页尾，按字节分配时越界写入） | 0.13.5 |
| openkal-llvm-runtime | #21：带上 openkal-musl 0.13.5 | 0.9.6 |
| mcpp-index | #424、#425：收录上述两个版本 | — |
| mcpp | #636：`emit build-database` 功能需求，尚未实现 | — |

此前各轮的上游改动（K1–K13）见设计文档第 12.9 节；只记录、未修复的事项与暂缓工作集中在 issue #2。

