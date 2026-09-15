# C++ 代码提示架构简报：五层分工、复杂度、lsp-mcpp 的定位与 mcpp 生态联动

日期：2026-09-15
关联文档：
- 深度对比（实测矩阵与来源）：[2026-09-15-cxx-modules-language-servers-comparison.md](2026-09-15-cxx-modules-language-servers-comparison.md)
- 调研综述：[2026-09-13-cxx-modules-landscape-research.md](2026-09-13-cxx-modules-landscape-research.md)
- 设计方案：[2026-09-13-cxx-modules-unified-lsp-design.md](2026-09-13-cxx-modules-unified-lsp-design.md)
- 可用方案与执行记录：[2026-09-14-lsp-mcpp-v1-usable-plan.md](2026-09-14-lsp-mcpp-v1-usable-plan.md)

> 口径：代码规模为各仓库 2026-09-15 源码的非空行数，不含测试，方法见附录；通过项与耗时引自深度对比第 2、5、6 节与 lsp-mcpp 的 CI。本文作者也是 lsp-mcpp 的实现者。

---

## 0. 要点

1. **主流 C++ 代码提示分五层：编辑器 → 语言服务引擎 → 构建数据 → 构建工具 → 编译器。** 编辑器只是 LSP 客户端；补全、跳转、诊断由引擎内嵌的编译器前端计算；引擎靠构建数据知道每个文件怎么编译。编辑时，构建用的编译器只被查询，不参与语义计算。
2. **复杂度集中在两端，中间的契约最薄。** Clang 约 171 万行，clangd 约 6.9 万行；而头文件时代的主要契约 `compile_commands.json` 只是“文件 → 目录与参数”的扁平表。
3. **模块让中间层成为主要矛盾。** 翻译单元不再独立，引擎要先从源码构建 BMI，需要模块图、`std` 源码和自己能执行的命令，现有构建工具给不全。原生 clangd 23.1 在 mcpp 工程上 0/10，同一工程换成规范化数据后 8/10。
4. **lsp-mcpp 补的是缺失的“工程模型与数据规范化层”，并负责零配置交付。** 它不自研前端、不做构建：对编辑器是 LSP 服务端，对内置 clangd 是客户端，对构建工具是 S1/S2 的消费方。服务端约 1.2 万行，约为 clangd 核心的六分之一。
5. **与 mcpp 生态联动，是把五层纵向打通。** mcpp 是目前唯一实现 S1 的构建工具（`mcpp emit build-database`，不写工程目录）；xlings 负责工具链与分发；openkal 让一台 Linux 主机交叉构建全部平台。整体形态接近 Rust 的 cargo + rustup + rust-analyzer，但 lsp-mcpp 不要求工程使用 mcpp。
6. **上限与风险：** 引擎上限就是 clangd；GCC、MSVC 工程得到的是 Clang 语义；S1/S2 只有 mcpp 一个生产方；项目尚未发布。

---

## 1. 五层架构

### 1.1 总览

```text
① 编辑器层  VS Code、Neovim、Emacs、Zed、Helix、Qt Creator；一体化 IDE：Visual Studio、CLion、Xcode
    │  LSP（JSON-RPC）：didOpen / didChange、completion、definition、references、诊断推送
    ▼
② 引擎层    clangd、clice、ccls；闭源：EDG IntelliSense、ReSharper C++ / CLion Nova、Visual Assist
    │  读取：每个文件的编译参数；另读工具链的头文件与 std 源码
    ▼
③ 数据层    compile_commands.json、.clangd、P1689、P2977 构建数据库、P3286 标准库清单、S1/S2
    ▲  导出：配置或规划阶段写出（或由 Bear 等工具拦截构建命令生成）
    │
④ 构建层    CMake + Ninja、MSBuild、xmake、build2、Meson、Bazel、mcpp
    │  调用：编译、依赖扫描（clang-scan-deps、-fdeps、/scanDependencies）、工具链查询
    ▼
⑤ 编译器层  GCC + libstdc++、Clang + libc++、MSVC + MSVC STL
```

| 层 | 职责 | 编辑时 |
|---|---|---|
| ① 编辑器 | 文本缓冲区；渲染补全、诊断与跳转结果；启动与配置语言服务器 | 参与 |
| ② 引擎 | 解析与语义分析、补全、导航、索引；模块工程还要自己构建 BMI | 参与，语义都在这里计算 |
| ③ 数据 | 描述每个翻译单元怎么编译；模块时代还要描述模块图与 `std` 来源 | 只被读取 |
| ④ 构建 | 扫描依赖、排定编译顺序、产出 BMI 与目标文件、链接；导出 ③ | 不参与，只在导出数据时运行 |
| ⑤ 编译器 | 定义语义；产出 BMI 与目标文件；提供标准库头文件、`std` 模块源码与清单 | 不参与；引擎只查询驱动、读取头文件与 `std` 源码 |

关键点：**编辑时有两个“编译器”。** 构建用的是 ⑤，引擎里内嵌的是另一个前端（clangd 用 Clang，Visual Studio 用 EDG）。头文件时代两者只需共享参数与头文件；模块时代它们本该共享 BMI，但三家的 BMI 互不可读且绑定编译器版本，引擎只能自己从源码重建。

### 1.2 一次补全请求经过什么（以 clangd 为例）

1. 编辑器发送 `didChange` 与 `textDocument/completion`。
2. clangd 从文件所在目录向上找 `compile_commands.json`，取出参数；库里没有的文件（例如头文件）借用相近文件的参数；`--query-driver` 允许它执行真实的 GCC 以取得系统头文件路径。
3. 文件开头的 `#include` 部分编成 preamble（类似 PCH）并缓存；模块工程还要先按导入关系构建所需模块的 BMI（`ModulesBuilder`）。
4. 在当前缓冲区上运行 Clang Sema 的补全，得到光标处可见的候选。
5. 与打开文件的动态索引、全工程后台索引合并排序，返回编辑器。跳转与引用同样依赖 AST 与索引。

语义质量取决于前端，能不能工作取决于第 2、3 步拿到的数据。头文件时代参数不全，常见后果是局部误报；模块时代数据不全，所需 BMI 建不出来，导入的声明全部不可见，clangd 23.1 甚至会停止应答（实测）。

### 1.3 引擎层的四条路线

| 路线 | 代表 | 语义从哪来 | 对数据层的要求 | 跨编译器 |
|---|---|---|---|---|
| 独立 IDE 前端 | Visual Studio、VS Code C/C++ 扩展 | EDG 前端，与 MSVC 编译器是两套实现 | MSBuild 工程或 MSVC 参数 | 只服务 MSVC |
| 自研非编译器前端 | ReSharper C++、Visual Assist | 自研解析器，不依赖 BMI | IDE 自有工程模型 | 与编译器无关，但只在各自 IDE 中 |
| 编译器库前端 | clangd、clice、CLion 的模块功能、内嵌 clangd 的各编辑器 | Clang 前端从源码自建 BMI | 数据完整，命令能被 Clang 执行 | 只有 Clang 语义；GCC、MSVC 工程取决于参数能否被接受 |
| 数据规范化层 + 编译器库前端 | lsp-mcpp | 同上，内置 clangd 23.1 | 由本层从任意构建来源重建 | GCC、MinGW、Clang、clang-cl、cl.exe 的命令翻译为 Clang 命令 |

---

## 2. 各层工具、作用与复杂度

### 2.1 ① 编辑器层

| 工具 | 作用 | 复杂度 |
|---|---|---|
| VS Code + C++ 扩展 | 编辑器自带通用 LSP 客户端；扩展负责获取服务端、写配置、处理冲突 | 低。扩展本体千行级：vscode-clangd 1,638 行，lsp-mcpp 的“C++ Modules” 1,152 行，mcpp-vscode 761 行 |
| Neovim、Emacs、Zed、Helix、Sublime、Kate、Qt Creator | 内置 LSP 客户端，转交 clangd | 低，几行配置；都没有模块相关开关 |
| Visual Studio、CLion、Xcode | 编辑器、工程系统、引擎、调试器同属一个产品 | 高，闭源；工程模型与引擎深度耦合 |

这一层难在体验，不在代码：零配置、状态可解释、不与其他 C++ 扩展冲突。

### 2.2 ② 引擎层

| 工具 | 作用 | 复杂度 |
|---|---|---|
| clangd 23.1 | 开源主流；以库的方式使用 Clang：preamble、AST、补全、后台索引、模块 BMI 构建 | 核心 68,968 行，测试另有 57,842 行。模块支持（`ModulesBuilder`、`ProjectModules`）只有 1,803 行，建在 Clang 的 BMI 读写之上；仍需 `--experimental-modules-support`，数据不对时请求无应答 |
| clice nightly | 新一代 Clang 引擎：多进程 worker、PCM 任务图、LMDB 索引 | 62,057 行，另有自研协程框架 kota；基于带补丁的 Clang 22.1.8；未发布稳定版 |
| ccls | libclang 索引型服务器 | 不支持命名模块 |
| EDG IntelliSense | Visual Studio 与 VS Code C/C++ 扩展的语义引擎 | 闭源；模块 IntelliSense 七年仍为实验性 |
| ReSharper C++ / CLion Nova、Visual Assist | 自研前端；CLion 的模块功能借助内置 clangd | 闭源，长期商业投入 |

引擎是 IDE 一侧最复杂的部分：它是为交互优化的编译器前端，要在编辑中的不完整代码上快速响应，并管理并发、缓存与失效。

### 2.3 ③ 数据层

| 格式或协议 | 内容 | 状态与缺口 |
|---|---|---|
| `compile_commands.json` | 每个文件的目录、参数、输出 | 事实标准。没有模块图与 `std` 源码；带着构建编译器的 BMI 参数；GCC、MSVC 参数 Clang 执行不了 |
| `.clangd`、`c_cpp_properties.json` | 引擎私有的参数修补 | 各引擎一套，手工维护 |
| P1689R5 | 依赖扫描结果（`provides`、`requires`） | 事实标准，三家实现字段不一致；只有边，没有命令 |
| P2977R2 构建数据库 | 集合、翻译单元与模块关系 | 提案，2024-10 后无进展；CMake 实验实现，没有消费者；不含 `std` 与工具链描述 |
| P3286 标准库模块清单 | `std` 源码位置与参数 | libc++、libstdc++ 已按此发布；MSVC 的 `modules.json` 形状不同 |
| CMake `.modmap`、File API | 模块映射响应文件；目标与文件角色 | CMake 私有，信息分散 |
| S1/S2（lsp-mcpp） | 模块图、单元角色、工具链、`std` 来源、结构化语义选项；子进程发现协议与监视列表 | 规范 S1–S4 共 144 条规则有一致性证据；mcpp 已实现生产方 |

S1 分四级：1 Graph（模块关系）、2 IDE（工具链、单元角色、全部翻译单元）、3 Structured（结构化语义选项）、4 Live（发现命令与原子更新）。

这一层的格式都很简单（JSON，十几个字段），难在语义正确：参数方言、响应文件、BMI 参数属于哪个编译器、同一文件在不同目标下参数不同、`std` 在哪。标准化没有推进者，是五层中最缺人负责的一层。

### 2.4 ④ 构建层

| 工具 | 模块支持与 IDE 输出 | 复杂度 |
|---|---|---|
| CMake 4.4 + Ninja | `FILE_SET CXX_MODULES` 3.28 起稳定，`import std` 仍为实验；输出编译数据库（带 `@modmap`），构建数据库为实验且仅 Ninja | `Source/` 约 35.1 万行，模块相关顶层文件约 3,300 行；扫描 → 汇总 → 动态依赖 |
| MSBuild | 自动识别 `.ixx`，内部做 P1689 扫描；没有导出物 | 闭源 |
| xmake、build2、Meson、Bazel | 各自支持，成熟度不一；没有模块图输出 | xmake 为 GCC 写出的参数 clangd 不能解析（xmake#5830）；Bazel 回退了 GCC 模块支持 |
| mcpp | 自研扫描 + ninja 后端，覆盖 GCC、Clang、MSVC 与 `import std`；`emit build-database` 输出 S1 等级 2，不写工程 | `src/` 与 `modules/` 约 9.1 万行，其中构建数据库导出 591 行 |

模块把构建变成两段：先扫描依赖，再按依赖顺序编译；`std` 模块还要按工具链各构建一次。

### 2.5 ⑤ 编译器层

| 编译器与标准库 | BMI 与定位方式 | `std` 模块来源 | 复杂度 |
|---|---|---|---|
| Clang + libc++ | `.pcm`，`-fmodule-file=<名>=<路径>` | `std.cppm` + `libc++.modules.json` | Clang 的 `lib` 与 `include` 约 171 万行，其中语义分析 Sema 约 29.3 万行、BMI 读写（Serialization）约 3.7 万行 |
| GCC + libstdc++ | `.gcm`，module mapper | `bits/std.cc` + `libstdc++.modules.json` | 模块实现集中在 `gcc/cp/module.cc`，单文件 20,846 行 |
| MSVC + MSVC STL | `.ifc`，`/reference`、`/interface` | `std.ixx` + `modules.json` | 闭源 |

三种 BMI 互不可读且绑定编译器版本，IDE 不能复用构建产出的 BMI；P2581 与 GCC 文档也不建议这样做。

### 2.6 复杂度汇总

| 层 | 代表规模 | 复杂度来自 | 模块带来的新增难度 |
|---|---|---|---|
| ① 编辑器 | 扩展千行级 | 体验与配置 | 低：显示状态与进度 |
| ② 引擎 | 十万行级，依赖百万行级前端 | 交互式编译、并发、缓存、索引 | 高：自建 BMI，处理依赖顺序与失效 |
| ③ 数据 | 格式百行级，规范千行级 | 语义正确性、方言、多配置 | 最高：扁平表不够，需要模块图、角色与 `std`；标准停滞 |
| ④ 构建 | 十万行级 | 依赖、增量、跨平台 | 高：扫描、动态依赖、`std` 构建 |
| ⑤ 编译器 | 百万行级 | 语言本身 | 高但各家已实现；问题在物理层互不兼容 |

---

## 3. 模块时代的断层

| | 头文件时代 | 模块时代 |
|---|---|---|
| 翻译单元 | 相互独立，任一文件可单独解析 | 导入方依赖被导入模块先构建成 BMI |
| 引擎需要的数据 | 每个文件的参数 | 参数 + 模块图 + 单元角色 + `std` 源码 + 引擎能执行的命令 |
| 与构建共享什么 | 头文件是文本，天然共享 | BMI 不能共享，只能从源码重建 |
| 数据不对的后果 | 局部误报 | 导入的声明全部不可见，可能无应答 |

实测（Linux 开发机，同一个一致性运行器；规范化数据指 `lsp-mcpp model --export engine` 的输出，clangd 另加 `--compile-commands-dir`）：

| 工程 | clangd 23.1 + 用户生成的数据 | clangd 23.1 + 规范化数据 | clice nightly：用户数据 → 规范化数据 | lsp-mcpp 零配置 |
|---|---|---|---|---|
| mcpp + LLVM 22 + `import std`（10 项） | 0/10 | 8/10 | 0/10 → 6/10 | 10/10 |
| 接口/实现分离工程（20 项） | 1/20 | 17/20 | 0/20 → 10/20 | 20/20 |
| 全 `.cppm` 工程（15 项） | 0/15 | 11/15 | 2/15 → 12/15 | 15/15 |

失败原因都在数据层：
- `std` 不在编译数据库里；
- 数据库带着 clang 22.1.8 构建的 `std.pcm`，clangd 23.1 报格式过旧；
- `std.cppm` 在工程目录外，clangd 找不到它的命令；
- GCC、MSVC 参数 Clang 执行不了。

规范化之后剩下的差距（`import` 语句上的跳转、未保存编辑传播到导入方），由 lsp-mcpp 在引擎之外补上。

---

## 4. lsp-mcpp 的定位

### 4.1 在五层中的位置

```text
① 编辑器层：VS Code 扩展“C++ Modules”，或任意 LSP 客户端
    │  LSP + S3 模块扩展（cxxModules/status、graph、moduleInfo、contexts、setContext）
    ▼
lsp-mcpp（插在 ① 与 ② 之间，代替用户整理 ③）
    ├─ 路由：转发、拦截、合并；语法模块索引回答模块名上的请求
    ├─ 引擎适配：clangd 进程、看门狗与重启、按模块图并行准备 BMI
    ├─ 归一化：方言翻译、删除 BMI 参数、注入 std、可解析性检查
    └─ 工程模型：读取 ③ 建 S1 模型；探测 ⑤；没有编译器时用语义工具包
    │  LSP
    ▼
② 引擎层：clangd 23.1（随扩展内置，版本锁定）

lsp-mcpp 读取的 ③：mcpp emit build-database（S1）、CMake 构建数据库或编译数据库、
                   任意 compile_commands.json、仅源码（扫描推断）
```

| 关系 | 定位 |
|---|---|
| 与编辑器 | 通用 LSP 服务端；VS Code 扩展是薄客户端，负责内置负载、语言状态项与冲突处理 |
| 与 clangd、clice | 上下游，不竞争。clangd 是当前引擎；引擎接口只有启动、推送数据库、转发 LSP、能力查询四项，clice 是第二引擎候选 |
| 与构建工具 | 数据消费方。mcpp 是参考生产方，CMake 与其他来源按可得数据降级 |
| 在 C++ 生态中的角色 | 相当于 rust-analyzer 的工程模型、SourceKit-LSP 的 BSP 客户端；C++ 此前没有这一层 |

### 4.2 做与不做

| 做 | 不做 |
|---|---|
| 把任意构建来源归一化为 S1 模型，再生成 clangd 能执行的数据库 | 自研 C++ 语义前端 |
| 零配置交付：内置 clangd 与语义工具包，不写工程目录，降级原因写在语言状态项 | 定义可移植 BMI，或读取构建产出的 BMI |
| 模块层功能：模块名导航、`import` 补全、未解析与歧义诊断、模块图、上下文切换 | 构建、链接、代码生成 |
| 规范 S1–S4 与一致性测试 | 第一版不含 header unit 与 Apple clang |

### 4.3 复杂度：实现薄，覆盖宽

| 部分 | 非空行 |
|---|---|
| 协议与平台（`base`、`platform`、`lsp`） | 2,517 |
| 规范库（`spec`） | 1,277 |
| 工程模型与工具链探测（`project`、`toolchain`） | 2,370 |
| 归一化（`normalize`） | 897 |
| 引擎适配与模块索引（`engine`、`index`） | 546 |
| 会话与路由（`server`） | 3,085 |
| 工具：一致性运行器、协议生成器、模拟 mcpp（`tools`） | 1,352 |
| **服务端 `src/` 合计** | **12,050（97 个文件）** |
| 单元测试 / VS Code 扩展 / 规范正文与 Schema | 3,048 / 1,152 / 1,433 |

- 代码量约为 clangd 核心的六分之一、Clang 的 0.7%。
- 复杂度在覆盖面：7 种构建组合（P1–P7）、3 种无编译器组合（K1–K3）、5 类数据来源、3 个主机。控制手段是规范与测试：36 个一致性夹具、每次提交 18 个 CI 任务、144 条规范规则逐条有证据。
- 结果：两类工程形态零配置全部通过；小工程首次跳转，开发机冷启动 2.1–2.5 秒、温启动 0.8 秒，CI 冷启动中位数 Linux 6.29 秒、macOS 4.31 秒、Windows 8.53 秒。零配置建模的代价约 1 秒：同一工程上 lsp-mcpp 3.03 秒，数据预先备好的原生 clangd 2.05 秒。

---

## 5. 与 mcpp 生态的联动

### 5.1 参照：其他语言的纵向组合

| 语言 | 构建与包管理 | 工具链管理 | 语言服务 | 引擎拿工程模型的方式 |
|---|---|---|---|---|
| Rust | cargo | rustup | rust-analyzer | `cargo metadata`；非 Cargo 工程用 `rust-project.json` 或发现命令 |
| Go | go | `go.mod` 的 `toolchain` 行 | gopls | `go list`，或外部 `GOPACKAGESDRIVER` |
| Swift | SwiftPM | swiftly | SourceKit-LSP | SwiftPM 或 BSP；`buildTarget/prepare` 先构建被导入模块 |
| C++，常见组合 | CMake 等 + vcpkg、Conan | 发行版或手工安装 | clangd、C/C++ 扩展 | `compile_commands.json`，没有模块图 |
| **C++，mcpp 生态** | **mcpp** | **mcpp，经 xlings 安装并按工程固定** | **lsp-mcpp + clangd** | **S1 构建数据库 + S2 发现协议** |

体验好的语言，构建工具与语言服务之间都有明确的工程模型接口。C++ 长期只有扁平的编译数据库；mcpp 生态在 C++ 里补齐了这条接口。

### 5.2 生态组件在五层中的位置

| 层 | mcpp 生态 | 与 lsp-mcpp 的连接 | 状态 |
|---|---|---|---|
| ① 编辑器 | lsp-mcpp 的 VS Code 扩展；mcpp-vscode（构建、运行、测试任务） | mcpp-vscode 改为依赖 lsp-mcpp 扩展，去掉 clangd 配置与兼容数据库改写 | 已定，等扩展发布 |
| ② 引擎 | lsp-mcpp + 内置 clangd | — | 第一版完成（PR #1） |
| ③ 数据 | S1/S2；mcpp SPEC-005 | `mcpp emit build-database --format json`：S1 文档、`watch` 列表、输入指纹 | mcpp 2026.9.15.1 已发布，三主机闭环验证 |
| ④ 构建 | mcpp；mcpp-index（231 个包描述文件） | `mcpp --protocol-version` 声明每个命令的副作用；`emit build-database` 从不声明 `write-project` | 已接入 |
| ⑤ 编译器 | mcpp、xlings 安装的 GCC 16 与 LLVM 22；Windows 上的 MSVC | 工具链探测查找 `~/.mcpp/registry/data/xpkgs` 与 `~/.xlings/data/xpkgs`；工程 `.xlings.json` 固定的 mcpp 版本在工程内生效 | 已接入 |
| 平台与分发 | openkal（musl + libc++ 运行时与三平台实现）；xlings、xim-pkgindex、xlings-res | 服务端基于 openkal 交叉构建；`xim:lsp-mcpp`、`xim:lsp-mcpp-kit` 描述模板 | 构建已用；描述文件未提交 |

### 5.3 联动优势

1. **数据层闭环：目前唯一生产方与消费方都已实现并互测的模块构建数据。**
   - mcpp 输出 S1 等级 2：模块图、单元角色、按包划分的集合（另有 `<包>:test` 与 `mcpp:std`）、`std` 翻译单元、需要监视的输入；lsp-mcpp 的 S1 库补全到等级 3。
   - 不写工程目录；示例工程 0.55–0.60 秒，lsp-mcpp 仓库（11 个集合、1,747 个单元）1.6 秒。
   - CI 在三个主机上校验 mcpp 的真实输出并由夹具消费。对照：CMake 的构建数据库仍为实验且不含 `std`，其他构建系统没有模块图输出。
2. **契约由双方协商，能持续演进。** 需求 mcpp#636 → 维护者三点答复（只输出等级 2、`visible-sets` 列出其余全部集合、按包划分集合）→ 规范与模拟数据修订 → mcpp#639 实现 → 闭环验证中修正 S1 库两处问题。`--protocol-version` 让消费方在执行前就知道命令是否写工程、联网、运行构建脚本。
3. **GCC 与 MSVC 工程也能用模块导航。** mcpp-vscode 过去在 GCC 工程里只能提示切换到 LLVM；现在 mcpp 的 GCC 16、`msvc@system`、面向 MSVC 目标的 LLVM 工程都由 lsp-mcpp 翻译后交给 clangd，`mcpp-gcc`、`mcpp-split-gcc`、`mcpp-msvc`、`mcpp-llvm-msvc`、`mcpp-split-msvc` 夹具在对应主机上逐次通过。
4. **包模型与 IDE 路线一致。** mcpp 依赖包的模块接口一律以源码交付、不分发 BMI，正是引擎从源码重建 BMI 所需的输入；按包划分的集合让跳转能进入依赖包的模块，`std` 由依赖包提供时同样可解析（lsp-mcpp 仓库的 `std` 来自 openkal-llvm-runtime 包）。
5. **一次构建，两个渠道分发。** openkal 让一台 Linux 主机交叉构建 Linux、macOS、Windows 的可搬移二进制；同一套产物放进按平台的 VSIX（31.3 / 28.1 / 36.4 MB），也按 xlings-res 约定拆包，供 Neovim、Emacs 等编辑器经 `xlings install` 使用（描述文件待提交，见 5.4），GitHub 与 GitCode 双镜像。
6. **生态互为测试场，问题就地修到上游。**
   - lsp-mcpp 自身是纯 C++23 模块工程，由 mcpp 构建，使用 mcpp-index 的模块化库 `nlohmann.json`、`mcpplibs.cmdline`。
   - 第一版向 openkal 链路提交并合入 14 个 PR，修复 K1–K14 中的 9 项缺陷；向 mcpp-index 提交 #414–#425 收录新版本。
   - nightly 用 mcpp 仓库（约 170 个模块，全 `.cppm`）与 lsp-mcpp 仓库（75 个 `.cppm` 接口、78 个 `.cpp`，接口/实现分离）做自举。xlings 同样是 mcpp 构建的模块工程（`src/` 194 个文件），生态里的大型模块工程本身就是 lsp-mcpp 的目标用户。
7. **规范走向标准的筹码。** S1/S2 已有两个独立实现与 144 条规则的一致性证据，这是提交 EcoStd 的前提；P2977 自 2024-10 停滞，“IDE 可用的模块构建数据”目前只有 mcpp 生态有可用实现。
8. **优势外溢，不锁定。** mcpp 工程拿到最完整的数据（等级 3、监视、不写工程）；CMake、任意 `compile_commands.json`、仅源码与无编译器的机器按可得数据降级，同样零配置可用。

### 5.4 联动尚未完成的部分

| 事项 | 现状 |
|---|---|
| mcpp-vscode 依赖 lsp-mcpp 扩展 | 等扩展发布；D28 决定暂不发布预览版 |
| xlings 渠道 | 描述模板与拆包脚本就绪，未提交 xim-pkgindex（issue #2） |
| 旧版 mcpp | 2026.9.15.1 之前的版本（包括工程 `.xlings.json` 固定的旧版本，例如 mcpp 仓库自身）走 `--configure-only` 回退，会写入 `compile_commands.json` 与 `target/`，状态中给出提示 |
| Windows 上的 openkal 工程 | lsp-mcpp 仓库在 Windows 上需要 `--target x86_64-windows-gnu`，编辑器还不能把目标传给 mcpp，Windows 自举未覆盖 |

---

## 6. 风险与建议

摘自深度对比第 8 节。

| 风险 | 建议 |
|---|---|
| 引擎上限就是 clangd：模块支持仍为实验；负载固定在 23.1.0，并绕过 MSVC STL 上的 `align_val_t` 回归 | 引擎接口保持可替换，评估 clice（A1）；向 clangd 上游提交实测问题（A2）；自建可移植的 clangd 23.1.1+（A7） |
| GCC、MSVC 工程得到的是 Clang 语义 | 诊断标注语义来源，评估合并构建编译器自身的诊断（A9） |
| S1/S2 只有 mcpp 一个生产方 | 提交 EcoStd，邀请 CMake、xmake、build2 实现生产方（A4） |
| 没有真实用户与发布历史；负载只有三个平台，只打包了 VS Code | 重新评估 D28，发 0.1 预览版（A6）；补 arm64 负载与其他编辑器（A5） |

---

## 附录：规模统计口径

- 统计非空行（`grep -cv '^[[:space:]]*$'`），不含测试目录；闭源工具没有数据。lsp-mcpp 含空行统计约 1.4 万行（`src/`、`os/`、`testing/`），即深度对比中的数字。
- 版本与范围：
  - clangd 与 Clang：llvmorg-23.1.0（ea7d852a）。clangd 为 `clang-tools-extra/clangd` 的 `.cpp`、`.h`，不含 `test`、`unittests`、`benchmarks`、`fuzzer`、`quality`；Clang 为 `clang/lib` 与 `clang/include` 的 `.cpp`、`.h`、`.def`、`.td`，含代码生成、静态分析器与内置头文件。
  - clice：cd64f06（2026-09-15），`src/`。
  - GCC：gcc-mirror/gcc master 的 `gcc/cp/module.cc`，2026-09-15 取得。
  - CMake：d099b15（2026-09-15），`Source/` 的 `.cxx`、`.h`、`.c`、`.hxx`；模块相关顶层文件为 `cmCxxModule*`、`cmDyndepCollation`、`cmScanDepFormat`、`cmBuildDatabase`、`cmImportedCxxModuleInfo`、`cmInstallCxxModuleBmiGenerator`。
  - 扩展：vscode-clangd e026b35、mcpp-vscode 56594db、lsp-mcpp 扩展，均为 `src/*.ts`。
  - mcpp：69fae268，`src/` 与 `modules/`；xlings：59068d6，`src/`；lsp-mcpp：8119917。
- 通过项、耗时与上游 PR 的原始记录：深度对比第 2、5、6 节，可用方案第 10 节，设计方案第 12.9 节。
