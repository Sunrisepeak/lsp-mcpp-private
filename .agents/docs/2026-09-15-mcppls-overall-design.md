# mcpp-language-server（mcppls）总体设计：AI 时代的 C++ 语言服务引擎

日期：2026-09-15
状态：**草案，待 review**
基线：lsp-mcpp v1（Sunrisepeak/lsp-mcpp-private#1，提交 8119917）
关联文档：
- v1 设计方案：[2026-09-13-cxx-modules-unified-lsp-design.md](2026-09-13-cxx-modules-unified-lsp-design.md)
- v1 可用方案与执行记录：[2026-09-14-lsp-mcpp-v1-usable-plan.md](2026-09-14-lsp-mcpp-v1-usable-plan.md)
- 语言服务方案深度对比：[2026-09-15-cxx-modules-language-servers-comparison.md](2026-09-15-cxx-modules-language-servers-comparison.md)
- 代码提示架构简报：[2026-09-15-cxx-code-intelligence-layers-brief.md](2026-09-15-cxx-code-intelligence-layers-brief.md)

> 与 v1 设计的关系：本文取代 v1 中与之冲突的决策，即 D2（锁定 clangd 为唯一语义引擎）、D19（扩展 ID）、D22（工具包命名）。其余 v1 决策继续有效：零配置、不写工程目录、S1–S4 规范体系、openkal 平台层、VS Code 与 xlings 两个分发渠道、暂不发布预发布版本（D28）。
>
> 标注：**已定**是 2026-09-15 讨论中确认的决策；**建议**是本文提出、待 review 的方案；**实验**是需要先验证的技术前提（第 13 节）。外部事实的来源列在附录 B。

## 目录

- **第一部分　背景与定位**：0 结论先行 · 1 背景 · 2 定位、目标与度量 · 3 决策记录
- **第二部分　架构**：4 总体架构 · 5 引擎抽象层 · 6 编排层 · 7 AI 能力层 · 8 入口
- **第三部分　规范、评估与安全**：9 规范 · 10 评估体系 · 11 安全与隐私
- **第四部分　实施**：12 改名实施 · 13 需要先做的实验 · 14 路线图与工作项 · 15 风险 · 16 需要 review 的要点
- **附录**：A v1 模块到新模块 · B 来源

---

# 第一部分　背景与定位

## 0. 结论先行

1. **定位。** mcpp-language-server（mcppls）是 AI 时代的 C++ 语言服务引擎：代码的语义事实层、校验层与审查引擎，同时服务编辑器里的人、编码代理与 CI。**不做代理本身**：没有代理循环、聊天界面和自主修改。
2. **引擎抽象层。** 所有语义能力都来自引擎。mcppls 引擎（自研，进程内）从第一天起是一等引擎，起步只实现模块能力；C++ 核心语义由 clangd 引擎提供；clice 以后作为实验引擎评估。引擎按能力声明参与，由编排器路由与合并。
3. **AI 能力独立成 `src/ai/`，作为核心功能建设。** 六组模块：查询（query）、上下文（context）、校验（verify）、审查（review）、模型接入（model）、MCP 入口（mcp）。
4. **一个内核，三个入口。** LSP 服务编辑器，MCP 服务代理，命令行服务 CI 与脚本；三者共用同一个编排器与同一组引擎。
5. **审查是核心 AI 功能，且证据驱动。**
   - 确定性发现在前：语义 diff、影响分析、模块规则、诊断、跨编译器结果。
   - 模型判断在后，每条必须引用证据；修复先验证、再呈现。
   - 输出为 LSP 诊断与代码操作、SARIF、MCP 结果。
6. **模型接入可替换、默认关闭。** 服务端进程不直接联网（`openkal.net` 不含域名解析与 TLS）。可选来源：交给代理自己判断（只返回证据）、MCP sampling、编辑器客户端的模型、独立的模型网关进程。
7. **基础侧同步补齐。**
   - clangd 专有逻辑收进 clangd 引擎，能力表真正驱动补偿策略。
   - kit 按引擎版本精确选择。
   - 同一工作区多实例先做协调，再引入工作区守护进程。
   - S3 状态改为引擎列表。
8. **三套评估。** 一致性夹具证明事实正确，代理任务基准证明对代理有价值，审查夹具证明审查质量。规则达到精确率门槛才默认开启，模型层始终需要显式开启。
9. **改名（已定）。**
   - 项目名 `mcpp-language-server`。
   - 可执行文件、C++ 模块与命名空间 `mcppls`。
   - 扩展 ID `mcpp-community.mcpp-language-server`，设置前缀 `mcppls.*`。
   - 工具包 `mcppls-kit`，独立成包。
10. **路线（已定）。** 改名 → 引擎抽象层 → 代理语义底座 → 审查规则层 → 模型层。每个阶段都以"两类模块工程形态、三主机夹具结果不变"为底线。

## 1. 背景

### 1.1 v1 基线

| 方面 | 现状（PR #1，提交 8119917） |
|---|---|
| 服务端 | C++23 全模块，`src/` 97 个文件、12,050 非空行；基于 openkal、由 mcpp 构建，一台 Linux 主机交叉构建三个平台 |
| 规范 | S1 构建数据库、S2 发现协议、S3 LSP 模块扩展、S4 语义工具包，144 条规则有一致性证据 |
| 验证 | 36 个一致性夹具、每次提交 18 个 CI 任务、nightly 自举与计时 |
| 核心标准 | 全 `.cppm` 工程与接口/实现分离工程零配置通过（15/15、20/20），覆盖 LLVM、GCC、MSVC 与三个主机 |
| 生态闭环 | mcpp 2026.9.15.1 的 `emit build-database` 输出 S1，CI 在三主机校验真实输出 |
| 性能 | 小工程首次跳转：开发机冷启动 2.1–2.5 秒、温启动 0.8 秒；171 个模块的仓库冷启动 25 秒 |
| 分发 | 按平台的 VSIX（31.3 / 28.1 / 36.4 MB）；xlings 描述模板就绪 |

### 1.2 v1 架构的限制（已按代码核对）

| 限制 | 位置 | 影响 |
|---|---|---|
| 引擎接口只抽象到"进程"：启动、推送数据库、转发 LSP、停止 | `src/engine/engine.cppm` | 自研能力无法作为引擎接入；路由写死在 router |
| clangd 专有逻辑散在接口之外：默认构造 clangd、解析 clangd 日志、按 `.cache/clangd/modules` 布局判断模块是否已建、状态写死 `engine.name = "clangd"`（S3 也把类型写死） | `src/server/workspace.cpp` | 换引擎要改会话代码 |
| 针对 clangd 23.1 的补偿无条件执行：模块提示、准备单元、排除导入无法解析的单元 | `src/normalize/plan.cpp` | 对其他引擎多余，甚至有害 |
| 能力表只在单元测试中被读取；`align_val_t` 绕过对所有 MSVC STL 上下文无条件添加 | `src/engine/clangd.cpp`、`src/normalize/gnu.cpp` | 与可用方案"按 clangd 版本决定"的写法不一致 |
| kit 取 xlings 中版本最新的，不看引擎版本；`payload.json` 缺少 clangd 部件时不回退 PATH | `src/server/payload.cpp` | 引入第二个引擎后会选错 kit |
| 同一工作区的多个实例共用缓存目录，彼此没有协调 | `<缓存>/workspaces/<工作区键>/` | 编辑器与代理各起一个实例时，会互相重写引擎数据库与准备单元 |
| 协议以光标为中心，所有能力都要"文件 + 位置"；命令行 `check`、`model` 每次冷启动 | `src/server/cli.cpp` | 不适合代理批量、按符号查询 |

### 1.3 AI 时代已经发生的变化

| 变化 | 事实 |
|---|---|
| 代理直接使用语言服务器 | **Claude Code 的代码智能插件：** 每次编辑后自动拿到诊断；能跳转、查引用、悬停、列符号、找实现、查调用层级。C/C++ 官方插件是 `clangd-lsp`，要求 PATH 上有 `clangd`；云端会话不启动插件的语言服务器 |
| | **GitHub Copilot CLI：** 目录受信任后在后台启动相关 LSP 服务器，使用定义、引用、悬停、重命名、文档符号、工作区符号、实现、调用层级 8 种操作；文档强调结果来自语言自己的编译器或分析器 |
| AI 功能走 LSP 通道 | GitHub Copilot Language Server 用 `textDocument/inlineCompletion` 给各编辑器提供行内补全 |
| 代理与编辑器之间有了协议 | ACP（Agent Client Protocol）：Zed 发起，JetBrains 参与 |
| 语言服务器被包装成代理工具 | Serena 以 MCP 提供基于语言服务器的符号级检索与编辑；C++ 已有基于 clangd 的 MCP 服务器，要求先有 `compile_commands.json` |
| AI 审查卡在误报上 | Sonar（2026-07）：AI 代码审查推广停滞，最常见的原因是误报太多、磨掉了信任；给出的解法是确定性分析与模型判断结合 |
| 本机观察一 | 本机 Claude Code 的 LSP 工具有 9 个操作，全部要求"文件 + 行 + 列"，连工作区符号搜索也不例外 |
| 本机观察二 | Claude Code 文档写明：工作区配置不对时会出现"无法解析导入"的误报诊断，而代理会依据诊断在同一轮修改代码。C++ 模块工程正是这种情况：原生 clangd 在 mcpp 工程上 0/10 |

### 1.4 判断

**传统功能的权重变化：**

| 功能 | 对人 | 对代理 | 趋势 |
|---|---|---|---|
| 诊断 | 实时提示 | 编辑后自检的主要信号 | 更重要，对准确度要求更高 |
| 跳转、引用、调用层级 | 导航 | 代替 grep；做影响分析 | 更重要 |
| 符号搜索、大纲 | 浏览 | 省 token 的阅读方式 | 更重要 |
| 重命名等重构 | 手动触发 | 可靠的机械修改 | 更重要 |
| 悬停中的类型与签名 | 查看 | 核实 API 存在、签名匹配 | 更重要 |
| 补全、签名帮助 | 输入辅助 | 代理不用 | 被 AI 行内补全分走 |
| 语义高亮、折叠、inlay hints | 显示 | 不用 | 不变 |

**由此得出的判断：**
1. **传统能力没有过时，反而被代理大量调用。**
2. **对代理而言，准确比覆盖更重要。** 误报诊断会把代理引向修改正确的代码。
3. **LSP 需要在上面补一层。** 代理要的是按符号查询、面向快照、带证据、可裁剪的结构化结果，而 LSP 以光标为中心。
4. **审查是语言服务器的长处。** 写代码的赛道已被专门产品占据；审查依赖工程模型、语义查询与确定性检查。
5. **C++ 放大了这一切。** 宏、模板、重载、模块、构建配置让基于文本的方法不可靠，工程模型的价值更高。

## 2. 定位、目标与度量

### 2.1 定位

| 使用者 | mcppls 提供的 |
|---|---|
| 人（编辑器） | v1 的全部语言功能；审查发现以诊断与代码操作呈现 |
| 代理（Claude Code、Copilot CLI、任意 MCP 客户端） | 零配置的正确语义；符号级查询与上下文；编辑后校验；审查证据与发现 |
| 流程（CI、提交前检查） | 命令行校验与审查、SARIF 输出、跨编译器检查 |

**职责分四层：**

| 职责 | 人 | 代理 | 流程 |
|---|---|---|---|
| 事实：代码是什么 | 跳转、悬停、引用 | 符号查询、模块接口摘要、构建上下文 | 依赖与影响 |
| 校验：改得对不对 | 实时诊断 | 编辑后校验、片段校验 | 跨编译器、跨配置校验 |
| 变更：怎么安全地改 | 重命名、快速修复 | 可验证的机械修改（由代理发起） | 迁移（远期） |
| 审查：该不该这样改 | 诊断、代码操作 | 发现与证据作为代理的反馈 | 语义 diff、审查报告、质量关口 |

### 2.2 目标

| 编号 | 目标 |
|---|---|
| G1 | 保持 v1 核心标准：两类模块工程形态、三主机、零配置、不写工程目录 |
| G2 | 引擎可替换、可组合：mcppls 引擎与 clangd 引擎并存，按能力路由 |
| G3 | 代理零配置接入，拿到正确的 C++ 模块语义 |
| G4 | 面向机器的查询：按符号、有快照语义、可裁剪、带证据 |
| G5 | 校验：编辑后校验、片段校验、跨编译器校验 |
| G6 | 审查：语义 diff、影响分析、证据驱动的发现、已验证的修复 |
| G7 | 本地优先：确定性能力全部在本地运行；模型能力需要显式开启 |

### 2.3 非目标

- **代理本身：** 代理循环、任务规划、聊天界面、自主的多文件修改。
- **模型：** 托管、训练或绑定某一家模型。
- **行内补全：** 不与 AI 补全产品竞争；只保留 `textDocument/inlineCompletion` 通道，不实现。
- **完整的 C++ 语义前端：** mcppls 引擎按能力逐步扩展，核心语义继续由 clangd 提供。
- **header unit：** 沿用 v1，不在范围内。
- **写用户文件：** 所有修改都以代码操作或补丁的形式交给使用者决定。

### 2.4 度量（建议值，基准建成后校准）

| 目标 | 指标 | 建议值 |
|---|---|---|
| G1 | 两类形态与现有 36 个夹具 | 每个阶段三主机结果与改动前一致 |
| G3 | 代理任务基准：mcppls 对比 clangd-lsp 的成功率、轮数、token | 模块工程上成功率更高、轮数与 token 更少；门槛在基准 v0 跑出基线后确定 |
| G4 | 温状态查询延迟（小工程，开发机） | p50 ≤ 200 毫秒，p95 ≤ 1 秒 |
| G4 | 代理会话连上已预热的工作区后，首个结果的耗时 | ≤ 1 秒（冷启动为 2–25 秒） |
| G5 | 修改一个实现单元后的校验（温状态） | ≤ 3 秒 |
| G6 | 审查规则在审查夹具上的精确率 | ≥ 90% 才默认开启该规则 |
| G6 | 模型发现的精确率 | ≥ 70% 才展示，且始终需要显式开启 |
| G7 | 默认配置下的网络请求 | 0 |

## 3. 决策记录

| 编号 | 决策 | 状态 |
|---|---|---|
| D29 | **改名。** 项目与包 `mcpp-language-server`；可执行文件 `mcppls`；C++ 模块与命名空间 `mcppls`；扩展 ID `mcpp-community.mcpp-language-server`；设置与命令前缀 `mcppls.*`；工具包 `mcppls-kit`。S3 方法保持 `cxxModules/*`，`.agents/docs` 历史文档保留旧名。取代 v1 D19、D22 | 已定 |
| D30 | **引擎抽象层。** 所有语义能力来自引擎；mcppls 引擎从第一天起是一等引擎，起步只实现模块能力；C++ 核心语义由 clangd 引擎提供。取代 v1 D2 | 已定 |
| D31 | **kit 独立。** 工具包保持独立包，版本等于所服务引擎的 libc++ 版本，并按引擎版本精确选择 | 已定 |
| D32 | **实施顺序。** 改名 → 引擎抽象层 → 代理语义底座 → 审查规则层 → 模型层 | 已定 |
| D33 | **AI 目录。** AI 相关模块独立为 `src/ai/`，作为核心功能建设；不实现代理本身 | 已定 |
| D34 | **AI 方向。** 以代码审查为核心，而非代码生成 | 已定 |
| D35 | 一个内核、三个入口：LSP、MCP、命令行共用编排器与引擎 | 建议 |
| D36 | 证据驱动的审查：模型发现必须引用确定性证据；修复先验证、再呈现 | 建议 |
| D37 | 模型接入抽象为多种来源（不使用、代理自身、MCP sampling、编辑器客户端、模型网关），默认不使用；服务端进程不直接联网 | 建议 |
| D38 | 多使用者分两步：先做同一工作区的实例协调，代理语义底座阶段再引入工作区守护进程 | 建议 |
| D39 | 代理查询与审查结果规范化为 S5（项目接口规范，先实现、后标准化） | 建议 |
| D40 | 代理任务基准与审查夹具进入 nightly；审查规则按精确率门槛决定是否默认开启 | 建议 |

---

# 第二部分　架构

## 4. 总体架构

### 4.1 分层

```text
使用者    编辑器（VS Code、Neovim……）   编码代理（Claude Code、Copilot CLI……）   CI 与脚本
            │ LSP + S3                     │ MCP，或 LSP 插件                        │ 命令行
入口层    server                          ai/mcp                                   cli
AI 能力层 ai/query · ai/context · ai/verify · ai/review · ai/model
编排层    orchestrator：工作区、文档与快照、能力路由、结果合并、实例协调、守护进程
引擎层    engine：接口、能力、特征、注册表、安装定位
            ├─ engine/native：mcppls 引擎（进程内；起步：模块能力）
            ├─ engine/clangd：clangd 引擎（外部进程；C++ 核心语义）
            └─ engine/clice：clice 引擎（以后，实验）
工程层    project · toolchain · normalize · spec（S1、S2、S4、S5）
基础层    base · platform（openkal） · lsp（JSON-RPC、协议类型、连接）
```

依赖规则：
- 依赖只能自上而下；`ai` 之下的层不依赖 `ai`；入口层之间互不依赖。
- 具体引擎由组合根（`cli`）注册，编排器只认识引擎接口。

### 4.2 目录与模块

**命名对应：** `src/<目录>/<名字>.cppm` 对应模块 `mcppls.<目录>.<名字>`，子目录继续展开。例如 `src/ai/review/findings.cppm` 对应 `mcppls.ai.review.findings`。

**同名文件：** mcpp 2026.9.15.1 会为根包内同名的源文件区分目标文件路径（本机实测：`src/a/util.cppm` 与 `src/b/util.cppm` 分别编译为 `obj/<包>/src/a/util.m.o` 和 `obj/<包>/src/b/util.m.o`），所以不同目录可以有同名文件。

```text
src/
  main.cpp                      进入 cli
  base/          mcppls.base.*          错误、日志、路径、文本、URI、sha256、glob（v1 base）
  platform/      mcppls.platform.*      进程、文件、任务、环境、目录、标准流（v1）；新增 net：本机 TCP
  lsp/           mcppls.lsp.*           JSON-RPC、LSP 3.18 类型、LSP 分帧连接（v1 lsp）
  spec/          mcppls.spec.*          S1、S2、S4（v1）；新增 query：S5 的数据类型
  project/       mcppls.project.*       工程识别、数据来源、扫描（v1 project）
  toolchain/     mcppls.toolchain.*     编译器发现与探测（v1 toolchain）
  normalize/     mcppls.normalize.*     方言翻译；plan 只产出与引擎无关的计划
  engine/        mcppls.engine          引擎接口、能力声明、特征、注册表、内部查询接口
    payload.cppm mcppls.engine.payload  引擎二进制与 kit 的定位、完整性校验（v1 server/payload）
    native/      mcppls.engine.native.* mcppls 引擎：模块索引（v1 index/modules）、模块能力、导出面提取
    clangd/      mcppls.engine.clangd.* clangd 引擎：进程、能力行、数据库与模块提示渲染、
                                        模块准备（v1 server/primer）、缓存探测、日志解析
  orchestrator/  mcppls.orchestrator.*  工作区（v1 server/workspace 的非协议部分）、文档与快照（v1 server/documents）、
                                        路由与合并（v1 server/router）、实例协调、守护进程
  ai/            mcppls.ai.*            AI 能力（第 7 节）
    query/                              符号级、快照语义的查询
    context/                            上下文包：模块接口摘要、构建上下文、预算裁剪
    verify/                             编辑后校验、片段校验、跨编译器校验
    review/                             变更收集、语义 diff、影响分析、规则、发现、证据、SARIF；审查引擎
    model/                              模型接入：来源抽象、网关协议、提示模板、结构化输出校验
    mcp/                                MCP 入口：分帧、会话、工具
  server/        mcppls.server.*        LSP 入口：会话、S3 状态、编辑器侧 AI 功能（v1 server/session）
  cli/           mcppls.cli.*           命令行入口与组合根：serve、mcp、daemon、query、verify、review……（v1 server/cli）
  tools/                                mcppls-conformance、mcppls-lspgen、mcppls-mock-mcpp、mcppls-bench（新增）
bench/                                  代理任务与审查夹具（第 10 节）
model-gateway/                          模型网关参考实现 mcppls-model，独立的 mcpp 包（7.5 节，待实验 X3）
```

### 4.3 依赖规则

| 层 | 可以依赖 |
|---|---|
| base | std、nlohmann.json |
| platform | base、openkal |
| lsp、spec | base、platform |
| project、toolchain、normalize | base、platform、spec，以及 v1 已有的相互依赖 |
| engine（接口） | base、spec、normalize 的计划类型 |
| engine/native、engine/clangd、engine/payload | engine 接口、lsp、project、toolchain、normalize、platform |
| orchestrator | engine 接口、project、normalize、lsp、platform |
| ai/* | orchestrator、engine 接口、project、spec、lsp、platform；内部依赖 query → context → verify → review，model 独立，mcp 在最上 |
| server、cli | 以上全部 |

**引擎之间、引擎与 AI 能力之间不直接调用。**
- `engine` 层定义内部查询接口 `engine::Query`，由编排器实现，并通过引擎上下文交给每个引擎。
- 例如审查引擎要查引用时，经这个接口由 clangd 引擎回答。

### 4.4 进程形态

| 进程 | 启动者 | 说明 |
|---|---|---|
| `mcppls serve` | 编辑器、代理的 LSP 插件 | LSP over stdio。阶段 2 起默认经工作区守护进程；`--embedded` 保留 v1 形态作为回退 |
| `mcppls mcp` | 代理 | MCP over stdio；连接工作区守护进程，没有时拉起 |
| `mcppls daemon` | 以上入口按需拉起 | 每个工作区一个，持有工程模型、引擎、索引、缓存；空闲超时后退出 |
| `mcppls query`、`verify`、`impact`、`review`、`check`、`model` | CI、脚本、代理的 shell 调用 | 连接守护进程或冷启动；输出 JSON、文本或 SARIF |
| clangd | 编排器 | 每个工作区上下文一个；片段校验另有按需启动的校验实例（实验 X1） |
| `mcppls-model` | ai/model | 只在选择网关来源时启动，默认不启动 |
| mcpp、cmake、编译器、git | 工程层、ai/verify、ai/review | 短时子进程，只在受信任的工作区执行 |

## 5. 引擎抽象层

### 5.1 接口

```cpp
export module mcppls.engine;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.normalize.plan;

export namespace mcppls::engine {

// 同一方法上多个引擎的参与方式
enum class Role {
    answer,     // 按优先级，第一个认领的引擎回答
    fallback,   // 前面的引擎回答为空时回答
    merge,      // 所有认领的引擎都回答，由编排器合并
};

struct MethodCapability {
    std::string_view method;   // 例如 "textDocument/definition"，或内部方法 "mcppls/moduleInterface"
    Role role { Role::answer };
    int priority { 0 };        // 数值大的先问
};

// 引擎的行为特点，决定服务端替它做哪些补偿；按（引擎、版本）查表
struct EngineTraits {
    bool importNavigation { false };            // 能否在 import 语句上跳转
    bool pushesDiagnostics { true };
    bool hangsOnUnresolvedImports { false };    // 是：计划排除这些单元
    bool needsModulePreparation { false };      // 是：按模块图并行准备
    bool needsModuleHints { false };            // 是：数据库写模块提示
    bool msvcStlNeedsNoAlignedAllocation { false };
    std::string kitStdlibVersion;               // 无编译器时需要的 libc++ 版本；空：不使用 kit
};

class Engine {
public:
    virtual ~Engine() = default;
    virtual std::string_view id() const = 0;                         // "mcppls" | "clangd" | "clice" | "review"
    virtual std::span<const MethodCapability> methods() const = 0;
    virtual EngineTraits traits() const = 0;

    virtual base::Result<void> start(const EngineContext& context) = 0;
    virtual void stop(std::chrono::milliseconds grace) = 0;
    virtual EngineState state() const = 0;

    // 工程模型或计划变化：clangd 引擎渲染数据库与模块提示，mcppls 引擎重建模块索引
    virtual void apply(const ModelSnapshot& snapshot) = 0;
    // 文档变化，由编排器按快照规则分发
    virtual void document(const DocumentEvent& event) = 0;

    // 这个请求是否由本引擎负责，例如 mcppls 引擎判断光标是否在模块名上
    virtual bool claims(std::string_view method, const RequestView& request) const = 0;
    // 异步回答：reply 恰好调用一次；被取消时也要调用（可带空结果）
    virtual void request(std::string_view method, const nlohmann::json& params, Reply reply, CancelToken cancel) = 0;
};

}
```

`EngineContext` 提供五样东西：
- 工作区根；
- 该引擎的缓存目录；
- 事件出口：消息、诊断、模块构建事件、崩溃；
- 安装信息：二进制、版本、kit；
- 内部查询接口 `engine::Query`。

### 5.2 编排器的路由与合并

1. 按方法取出所有声明了该方法的引擎，按优先级排序。
2. `answer`：第一个认领的引擎回答；结果为空、且有 `fallback` 引擎认领时，再问它。
3. `merge`：所有认领的引擎并发回答，由按方法区分的合并器合并（文档符号、工作区符号、诊断、代码操作、code lens）。
4. 每个引擎有自己的超时与看门狗；合并类请求允许部分结果，并在状态中标注。
5. 取消向所有被询问的引擎传播。

**起步时的能力分工。** 与 v1 的路由行为一致，只是改用声明来表达：

| 请求 | mcppls 引擎 | clangd 引擎 |
|---|---|---|
| 光标在模块名上的跳转、声明、悬停 | answer，优先 | fallback |
| `import` 之后的补全 | answer，优先 | fallback |
| 其他位置的跳转、悬停、补全；引用、重命名、签名、语义高亮、inlay hints、调用层级 | — | answer |
| 文档符号、工作区符号 | merge | merge |
| 诊断 | merge：模块诊断 | merge：编译诊断 |
| S3：模块图、模块信息、上下文 | answer | — |

### 5.3 mcppls 引擎

| 阶段 | 能力 | 数据来源 | 接管条件 |
|---|---|---|---|
| N1（引擎抽象层） | v1 的模块能力：模块名跳转与悬停、`import` 补全、模块大纲与符号；`unresolved-module`、`ambiguous-module`、`partition-outside-module` 诊断；模块图、上下文 | S1 模型、词法扫描 | 现有夹具结果不变 |
| N2（代理语义底座） | 导出面提取：每个模块导出了哪些声明（名字、种类、声明文本、文档注释），经内部方法 `mcppls/moduleInterface` 提供给模块接口摘要与语义 diff | 声明级扫描；签名细节经内部查询由 clangd 补全（实验 X7） | 新增夹具，覆盖两类形态 |
| N3（审查规则层） | 模块规则：实现分区被导出、分区在模块外被导入、`export import` 成环、导出名被删除但仍有导入方使用 | N2 + 模块图 + 内部查询引用 | 审查夹具精确率达标 |
| N4（远期） | 补全跨模块引用与重命名（clangd#2569 的缺口）；分析头文件到模块的迁移 | N2 + clangd 索引 | 夹具证明结果不差于 clangd |

规则：mcppls 引擎每接管一项能力，都要先有夹具证明它的结果不差于原来回答的引擎，再调整优先级。

### 5.4 clangd 引擎

**收进 clangd 引擎的 v1 逻辑：**
- 进程管理与启动参数（v1 `engine/clangd`）；
- 数据库渲染与模块提示（v1 `normalize/plan` 的 `to_compile_commands` 与 `moduleHints`）；
- 模块准备与准备单元（v1 `server/primer`，以及 `workspace.cpp` 中的调度）；
- 持久化模块缓存探测（v1 `workspace.cpp` 读取 `.cache/clangd/modules`）；
- 日志解析（模块构建失败）。

**能力行：**
- 23.1.0：
  - `importNavigation=false`、`hangsOnUnresolvedImports=true`；
  - `needsModulePreparation=true`、`needsModuleHints=true`；
  - `msvcStlNeedsNoAlignedAllocation=true`、`kitStdlibVersion="23.1.0"`。
- 23.1.1 起：关闭对齐分配绕过（llvm-project#218152 已在 23.1.1 修复）。
- payload 继续固定在 23.1.0（2026-09-14 的"路线 1"决定）。

**normalize 与引擎的边界：**
- normalize 只保留与引擎无关的计划：单元、参数、`std` 注入、问题、可解析性。
- 是否排除单元、是否写模块提示、是否加 `-fno-aligned-allocation`，由编排器按引擎特征决定。
- 这些开关以计划输入中的普通字段传入，normalize 不依赖 engine。

### 5.5 clice 引擎（实验）

**评估时机与前提：** 在代理语义底座阶段之后评估。前提来自深度对比 A1、A3：推送诊断、实现单元中的实现分区、缓存不写工作区、Windows 稳定性。

**适配要点：**
- 渲染 `compile_commands.json`，不写模块提示；
- 能力行：`importNavigation=true`、`pushesDiagnostics=false`、`hangsOnUnresolvedImports=false`、`needsModulePreparation=false`；
- kit 为 libc++ 22.1.8，或只在有编译器时可选。

### 5.6 引擎选择与安装

| 事项 | 设计 |
|---|---|
| 选择 | 设置 `mcppls.engine`、命令行 `--engine`：`clangd`（默认）、`clice`、`none`；mcppls 引擎始终启用 |
| payload 清单 | 版本 3：`engines` 按引擎列出二进制、版本与 kit，例如 `{"clangd": {"version": "23.1.0", "path": "clangd/bin/clangd", "kit": "kit"}}` |
| VSIX | 服务端 + 默认引擎（clangd 23.1.0）+ 对应 kit |
| xlings | `mcpp-language-server` 依赖 `llvm-tools@23.1.0` 与 `mcppls-kit@23.1.0`；`mcppls-model` 可选 |
| kit 选择 | 按引擎特征中的 `kitStdlibVersion` 精确匹配：先 payload，再 xlings 仓库 `xim-x-mcppls-kit/<版本>`，都没有则不用 kit |
| 回退 | payload 没有提供某个引擎时回退 PATH（修正 v1 中 `payload.json` 缺部件时不回退的问题），并检查版本是否在能力表内；不在表内时按最保守的特征运行，并在状态中提示 |

## 6. 编排层

### 6.1 工作区

每个工作区根一个 `orchestrator::Workspace`，持有工程模型、计划、引擎集合、文档与快照、诊断存储和状态。

v1 `WorkspaceRoot` 中与 LSP 协议相关的部分留在 `server`：初始化握手、S3 状态通知的节流、LSP 请求 ID 的映射。

### 6.2 快照

| 规则 | 内容 |
|---|---|
| 视图 | 引擎只有一份视图：磁盘内容 + 编辑器未保存的内容（覆盖层），与 v1 一致 |
| 代数 | 文件内容、工程模型或计划每变一次，快照代数加一；查询结果都带 `snapshot`：代数、哪些文件带覆盖层、是否仍在准备 |
| 代理的修改 | 代理写盘；编排器通过文件监视，或代理的显式通知（MCP `cxx_verify`、命令行 `--changed`）刷新 |
| 冲突 | 同一文件既有覆盖层、又被代理改了磁盘内容：以覆盖层为准，结果中标注 `dirty-in-editor` |
| 片段校验 | 不改动共享引擎里的文档，改在校验实例中进行（实验 X1） |
| 新鲜度 | 查询可要求 `fresh`：等相关文件的诊断与模块准备完成；超时则返回当前结果并标注 |

### 6.3 多使用者

**第一步：实例协调（引擎抽象层阶段）。**
- 第一个实例在 `workspaces/<键>/owner.lock` 写入进程号、启动时间和版本，独占该目录。
- 后来的实例使用私有目录 `workspaces/<键>/instances/<进程号>/`：冷启动，不共享模块缓存；状态中提示 `shared-workspace`。
- 锁文件对应的进程已经退出时，视为失效，由新实例接管目录。

**第二步：工作区守护进程（代理语义底座阶段）。**

| 事项 | 设计 |
|---|---|
| 发现 | `workspaces/<键>/daemon.json` 记录端口、令牌、进程号、版本；只有当前用户可读 |
| 连接 | 用 `openkal.net` 在 127.0.0.1 的随机端口监听（实验 X2）；首条消息携带令牌 |
| 会话 | 每个连接一个读线程，事件进入同一个消息队列，由主线程独占状态（v1 并发模型的延伸） |
| 协议 | 每个连接承载 LSP、MCP、内部查询三种会话之一；`mcppls serve` 与 `mcppls mcp` 只在 stdio 与连接之间转接 |
| 生命周期 | 由第一个入口按需拉起；所有连接断开后空闲 30 分钟退出；版本不同的入口启动自己的守护进程 |
| 覆盖层 | 覆盖层属于打开它的 LSP 会话，会话断开时撤销 |

## 7. AI 能力层（`src/ai/`）

### 7.0 边界

| 做 | 不做 |
|---|---|
| 回答关于代码的事实（查询、上下文） | 规划任务、决定下一步做什么 |
| 检查修改是否正确（校验） | 自主修改文件；所有修改都交给使用者 |
| 判断变更质量并给出证据（审查） | 聊天、多轮对话 |
| 受约束的模型调用：固定模板、结构化输出、单次或少量调用、没有工具循环 | 托管或训练模型 |

### 7.1 `ai/query`：查询

面向代理的符号级、快照语义的查询，编辑器与命令行同样可用。

| 查询 | 输入 | 输出 | 来源 |
|---|---|---|---|
| `symbol.find` | 名字（可带作用域与模块）、种类 | 候选符号：稳定标识、种类、限定名、定义与声明位置、所属模块 | clangd 工作区符号 + `textDocument/symbolInfo`（USR）；模块名由 mcppls 引擎回答 |
| `symbol.describe` | 符号标识或位置 | 签名、类型、文档注释、定义与声明、所属模块与集合 | clangd 悬停与定义；mcppls 引擎补模块信息 |
| `symbol.references` | 符号标识或位置 | 按模块与文件分组的引用、数量、截断标记 | clangd 引用（后台索引） |
| `symbol.calls` | 符号标识、方向 | 调用者或被调用者 | clangd 调用层级 |
| `file.outline` | 文件 | 紧凑大纲，含模块声明 | 两个引擎合并 |
| `module.describe` | 模块名 | 提供者、分区、导入与被导入、所在集合、角色 | mcppls 引擎 |
| `module.graph` | 可选过滤条件 | 模块依赖图 | mcppls 引擎 |
| `diagnostics.get` | 文件或改动集合、`fresh` | 诊断（带语义来源标签） | 两个引擎合并 |

约定（写入 S5）：
- **符号标识：** 用 clangd `textDocument/symbolInfo` 返回的 USR 作为稳定标识（clangd 23.1.0 源码中有该扩展，返回名字、容器名、USR、声明与定义位置；实验 X10）；另附限定名与模块名，便于人读。
- **位置：** 行号、列号都从 1 开始，列按 Unicode 字符计数；每个位置附带该行文本，代理不必自己数列。
- **快照：** 每个结果带 `snapshot`（见 6.2 节）。
- **截断：** 支持 `maxResults` 与 `maxTokens`；超出时给出 `truncated`、`total` 与续查令牌。排序确定：模块 → 文件 → 位置。

### 7.2 `ai/context`：上下文包

| 上下文 | 内容 | 用途 |
|---|---|---|
| 模块接口摘要 | 模块及其重导出分区的导出声明：名字、种类、声明文本、文档注释；不含实现 | 代理了解 `import M;` 带来了什么，不必读整个接口文件 |
| 构建上下文 | 文件所在的集合与角色、工具链家族与版本、目标、标准库、语言标准、宏、语义来源（构建工具链或 kit）、问题 | 代理不再猜"用的哪个编译器、`std` 从哪来" |
| 符号上下文 | 定义与声明、签名、N 个调用者、覆盖它的测试、所属模块的接口摘要 | 修改某个符号前一次拿全 |
| 变更上下文 | 语义 diff、影响范围、相关测试、确定性发现（来自 ai/review） | 代理自检、模型审查的输入 |

**预算与缓存：**
- 按字符数估算 token；
- 按优先级裁剪：定义 > 签名 > 调用者 > 测试 > 其余；裁剪处留标记；
- 结果按快照代数缓存。

### 7.3 `ai/verify`：校验

| 校验 | 做法 | 输出 |
|---|---|---|
| 编辑后校验 `verify.changed` | ① 取变更文件（显式列表，或 git 工作区的变化）；② 构建描述变了，先刷新模型；③ 按单元角色分类：只改了实现单元，只查该单元；改了接口单元，查直接导入方，传递导入方按预算继续查；④ 等待诊断 | 结论（通过、有错误、未完成）、按文件分组的诊断、已查与未查的文件、语义来源 |
| 片段校验 `verify.snippet` | 把候选代码叠加到目标位置所在的文件（在校验实例中进行），取落在片段范围内的诊断（实验 X1） | 是否通过、诊断 |
| 跨编译器校验 `verify.toolchains` | 仅限 mcpp 工程：用工程声明或指定的其他工具链（GCC、Clang、MSVC）编译受影响的单元，比较各工具链的诊断 | 只在部分工具链上出现的错误。只在受信任工作区执行；构建目录放在哪里见实验 X9 |

### 7.4 `ai/review`：审查引擎

审查引擎实现 `engine` 接口，认领三类方法：
- `textDocument/diagnostic`（标识 `mcppls-review`）；
- `textDocument/codeAction`（合并）；
- `workspace/executeCommand`（`mcppls.review.*`）。

**审查范围：** 相对某个修订的差异（`--base`）、已暂存的修改、工作区中的修改、单个文件或模块。

**流水线：**

| 步骤 | 内容 | 确定性 |
|---|---|---|
| 1 变更收集 | 调用 `git`（仅受信任工作区）取差异；把改动映射到文件、单元角色、集合 | 是 |
| 2 语义 diff | 用 mcppls 引擎分别扫描基线版本（`git show <基线>:<文件>`，实验 X8）与当前版本，比较模块声明、导入、导出面、分区与角色；当前版本经 clangd 补签名与类型 | 是 |
| 3 影响分析 | 从改动的导出名出发：找导入方与引用 → 受影响的集合与包 → 覆盖它们的测试（S1 `<包>:test` 集合中引用这些名字的单元） | 是 |
| 4 规则 | 见下表 | 是 |
| 5 模型判断（可选） | 以变更上下文为输入，固定模板、结构化输出；每条发现必须引用证据编号 | 否 |
| 6 修复（可选） | 模型给出补丁 → 用 `verify.snippet` 或 `verify.changed` 校验 → 只保留校验通过的补丁 | 生成部分否，校验部分是 |
| 7 输出 | LSP 诊断与代码操作、SARIF、MCP 结果、Markdown 报告 | — |

**首批规则**（建议；按审查夹具的精确率决定是否默认开启）：

| 规则 | 检查 | 证据 |
|---|---|---|
| `module/export-removed-in-use` | 导出声明被删除或改名，但仍有导入方在使用 | 导入方中的引用位置 |
| `module/export-signature-changed` | 导出函数的签名变化，列出受影响的调用点 | 调用点 |
| `module/partition-misuse` | 实现分区被导出，或分区在模块外被导入 | 模块图 |
| `module/import-unresolved` | 改动引入了无法解析或有歧义的导入 | 模块索引 |
| `build/diagnostic-introduced` | 改动范围内出现错误或警告 | 引擎诊断 |
| `build/toolchain-divergence` | 只在部分工具链上失败（开启跨编译器校验时） | 各工具链的诊断 |
| `test/exported-change-untested` | 导出接口变化，覆盖它的测试既没有改动也没有运行（提示级） | 影响分析 |

**发现的结构**（写入 S5）：

```json
{
  "id": "F3",
  "rule": "module/export-removed-in-use",
  "severity": "error",
  "message": "hello.greet no longer exports format_name, which 2 importers still call",
  "location": { "file": "src/greet/greet.cppm", "line": 12, "column": 1, "text": "export std::string format_name(std::string_view name);" },
  "evidence": [
    { "id": "E1", "kind": "reference", "location": { "file": "src/main.cpp", "line": 8, "column": 17, "text": "std::println(\"{}\", format_name(user));" } },
    { "id": "E2", "kind": "diff", "location": { "file": "src/greet/greet.cppm", "line": 12, "column": 1, "text": "-export std::string format_name(std::string_view name);" } }
  ],
  "origin": "rule",
  "fix": null,
  "fingerprint": "sha256:…"
}
```

模型发现在此基础上多带三项：`origin: "model"`、`model`（来源、模型名、模板版本）、`confidence`。`evidence` 为空，或引用了不存在的证据编号时，这条发现直接丢弃。

**映射：**

| 发现字段 | LSP 诊断 | SARIF 2.1.0 |
|---|---|---|
| `rule` | `code` | `ruleId` 与 `reportingDescriptor` |
| `severity` | `severity` | `level` |
| `location` | `range` | `locations` |
| `evidence` | `relatedInformation` | `relatedLocations` |
| `fix`（已验证） | 代码操作，在 `codeAction/resolve` 时给出修改 | `fixes` |
| `fingerprint` | `data` | `partialFingerprints` |
| `origin`、`model` | `source`：`mcppls review` 或 `mcppls review · <模型>` | `properties` |

**忽略与配置：**
- 用户忽略的发现按指纹记录在用户缓存目录中。
- 团队共享的规则配置 `.mcppls/review.toml` 由用户编写，mcppls 只读不写。

### 7.5 `ai/model`：模型接入

**来源：**

| 来源 | 适用入口 | 说明 |
|---|---|---|
| `none`（默认） | 全部 | 只返回确定性发现与变更上下文 |
| `agent` | MCP、命令行 | 代理自己就是模型：只返回证据与上下文，由代理判断，不产生额外调用与费用 |
| `mcp-sampling` | MCP | 经 MCP sampling 请代理客户端的模型完成判断，由客户端征得用户同意（各代理的支持情况见实验 X6） |
| `client` | LSP | 通过自定义请求，由编辑器扩展调用编辑器自带的模型接口（例如 VS Code 的语言模型 API） |
| `gateway` | 全部 | 独立的模型网关进程 `mcppls-model`，连接本地 OpenAI 兼容端点或云端服务 |

**网关协议**（stdio，每行一个 JSON）：
- `initialize`：可用模型、上下文长度、是否支持结构化输出；
- `complete`：消息、输出 JSON Schema、最大 token、温度 0；
- `cancel`；
- `usage`：token 用量。

服务端进程本身不联网：`openkal.net` 只提供 TCP 连接，规范明确不含域名解析，也没有 TLS。

**参考网关：**
- 放在 `model-gateway/` 下，作为独立的 mcpp 包，基于 mcpplibs 的 `llmapi`（C++23 模块，OpenAI 兼容，支持 Anthropic）。
- 能否基于 openkal 构建待实验 X3；不能的话，用常规工具链单独构建，以 xlings 包 `mcppls-model` 发布。
- 协议公开，用户可以换成自己的实现。

**约束：**
- 模板带版本号，发现中记录模板版本与模型名；
- 输出按 JSON Schema 校验，不合格即丢弃；
- 按提示内容的哈希缓存结果；
- 每次审查有 token 预算，超出即停止模型阶段并标注；
- 代码只作为数据：模板把代码放在明确分隔的区块中，模型输出永不执行（见第 11 节）。

### 7.6 `ai/mcp`：MCP 入口

传输：stdio（`mcppls mcp`），每行一条 JSON-RPC 消息；消息层复用 `lsp/jsonrpc`，只是分帧不同。

**工具**（建议；粒度待 review）：

| 工具 | 作用 | 对应 |
|---|---|---|
| `cxx_symbol` | 按名字或位置找符号：定义、声明、签名、文档、所属模块、稳定标识 | `symbol.find`、`symbol.describe` |
| `cxx_references` | 引用、调用者、被调用者；分组、计数、截断 | `symbol.references`、`symbol.calls` |
| `cxx_outline` | 文件或模块的紧凑大纲 | `file.outline` |
| `cxx_module` | 模块信息、接口摘要、模块图 | `module.*`、模块接口摘要 |
| `cxx_build_context` | 文件的构建上下文 | 构建上下文 |
| `cxx_diagnostics` | 文件或改动集合的诊断，可等待新鲜结果 | `diagnostics.get` |
| `cxx_verify` | 校验改动或候选代码 | `verify.*` |
| `cxx_impact` | 语义 diff、影响范围、相关测试 | ai/review 第 1–3 步 |
| `cxx_review` | 审查：确定性发现，并按模型来源的设置附加模型发现与已验证修复 | ai/review |

- **只读：** 所有工具都不写用户文件，全部标注 `readOnlyHint`。
- **信任：** `cxx_verify`、`cxx_impact`、`cxx_review` 会运行编译器或 git；在不受信任的工作区中拒绝执行，并说明原因。
- **描述：** 保持简短，因为代理的上下文开销随工具数量和描述长度增长。

### 7.7 编辑器侧的 AI 功能

| 功能 | LSP 载体 |
|---|---|
| 审查发现 | 动态注册拉取式诊断，`identifier: "mcppls-review"`，与编译诊断分开管理 |
| 修复 | `textDocument/codeAction`（`quickfix`）；在 `codeAction/resolve` 时才生成并校验修改 |
| 触发 | 命令 `mcppls.review.run`（范围：工作区修改、相对基线、当前文件或模块）、`mcppls.review.clear` |
| 进度 | `$/progress`；完成后发 `workspace/diagnostic/refresh` |
| 报告 | 在用户缓存目录生成 Markdown，用 `window/showDocument` 打开 |
| 行内补全 | 不实现，保留 `textDocument/inlineCompletion` 通道 |

**VS Code 扩展：**
- AI 命令与设置只在 `mcppls.ai.enabled` 开启后才显示；开启后，语言状态项显示审查与模型来源的状态；不新增任何弹窗。
- v1 的"命令 ≤ 4、可选设置 ≤ 4"改为：基础命令 ≤ 4、基础设置 ≤ 5（新增 `mcppls.engine`），AI 分组单独计数。

## 8. 入口

### 8.1 命令行

| 命令 | 作用 | 输出 |
|---|---|---|
| `mcppls serve` | LSP over stdio | — |
| `mcppls mcp` | MCP over stdio | — |
| `mcppls daemon start`、`stop`、`status` | 工作区守护进程 | 文本、JSON |
| `mcppls query symbol`、`refs`、`calls`、`outline`、`module`、`context` | 7.1 与 7.2 节的查询 | JSON（默认）、文本 |
| `mcppls diagnostics <文件…>` | 诊断 | JSON、文本 |
| `mcppls verify [--changed] [--files …] [--snippet …] [--toolchains]` | 7.3 节的校验 | JSON、文本 |
| `mcppls impact --base <修订>` | 语义 diff 与影响 | JSON、文本 |
| `mcppls review --base <修订> [--format sarif] [--model <来源>]` | 7.4 节的审查 | JSON、文本、SARIF |
| `mcppls check`、`mcppls model`、`mcppls version` | v1 命令 | 沿用 |

退出码：0 表示通过；1 表示存在错误级发现或诊断；2 表示命令本身失败。

### 8.2 代理接入

| 代理 | 方式 |
|---|---|
| Claude Code | LSP 插件：把 `mcppls` 作为 C/C++ 语言服务器，替代 `clangd-lsp`；另提供 MCP 配置 `mcppls mcp`（实验 X4、X6） |
| GitHub Copilot CLI | `.github/lsp.json` 配置示例（实验 X5）；MCP 配置 |
| 其他 MCP 客户端 | `mcppls mcp` |
| 提交前检查或代理钩子 | `mcppls verify --changed`、`mcppls review --base HEAD` |

---

# 第三部分　规范、评估与安全

## 9. 规范

| 规范 | 变化 |
|---|---|
| S1、S2、S4 | 内容不变，只更新文中名称（S4 的分发包名改为 `mcppls-kit`） |
| S3 | 小版本修订：状态增加 `engines` 字段（每个引擎的名字、版本、角色、状态、问题）；保留 `engine` 字段，填写核心语义引擎；诊断来源示例改为 `mcppls · gcc 16` |
| S5（新增） | 语义查询与审查接口：查询操作、结果约定（位置、快照、截断、证据）、发现的结构、LSP 与 SARIF 映射、MCP 工具与命令行绑定。作为项目接口规范，与实现同步演进，稳定后再评估是否提交标准组织 |

一致性要求沿用 v1：每条规则至少对应一个用例；规范、Schema 与用例在同一次提交中修改。

## 10. 评估体系

### 10.1 一致性夹具：事实是否正确

- 现有 36 个夹具在每个阶段结果不变。
- 新增检查种类：
  - S5 查询：`query-symbol`、`query-references`、`module-interface`、`build-context`；
  - 校验：`verify-changed`；
  - 审查：`review-findings`，精确匹配规则与位置。
- 两类工程形态分别覆盖新增的查询与审查检查。

### 10.2 代理任务基准：对代理是否有价值

| 事项 | 设计 |
|---|---|
| 任务 | `bench/tasks/<编号>/task.json`：工程（夹具或固定提交的仓库）、任务描述、成功判定（例如 `mcpp build && mcpp test` 加语义检查）、时限 |
| 首批 | 10–20 个任务，覆盖两类形态、GCC 与 LLVM、一个真实仓库。例如：跨模块改名并修好导入方；给导出函数加参数并更新调用点；把实现移入分区；修复模块构建错误；回答"某个分区在哪里被使用" |
| 对照组 | 只有 grep；clangd-lsp 插件；mcppls LSP 插件；mcppls MCP 工具 |
| 代理 | Claude Code 无界面模式、Copilot CLI |
| 指标 | 成功率、轮数、输入与输出 token、耗时、中途构建失败次数、改到任务范围外的文件 |
| 统计 | 每个任务、每个组重复 5 次，报告成功比例与中位数 |
| 运行 | nightly 手动或定时触发，需要模型服务的密钥与预算 |
| 运行器 | `mcppls-bench`，形式与一致性运行器一致 |

### 10.3 审查夹具：审查质量

| 事项 | 设计 |
|---|---|
| 夹具 | `bench/review/<编号>/`：基线工程、补丁、`expected.json`（必须出现的发现：规则、文件、范围；以及不得出现的发现） |
| 类别 | 模块接口破坏、分区误用、漏改导入方、跨编译器可移植性（例如只有 GCC 严格执行的 TU-local 规则）、构建描述错误；模型层另加生命周期与 UB 类 |
| 干净补丁 | 不含缺陷的补丁，专门用来度量误报 |
| 指标 | 按类别与来源（规则、模型）统计精确率与召回率；模型层多次重复运行，度量稳定性 |
| 门槛 | 规则精确率 ≥ 90% 才默认开启；模型发现精确率 ≥ 70% 才展示 |

### 10.4 性能

以下指标进入 nightly 计时，与 v1 的计时夹具并列：
- 温状态查询的 p50 与 p95；
- 连接守护进程后首个结果的耗时；
- 编辑后校验的耗时；
- 守护进程的内存占用；
- 审查耗时。

## 11. 安全与隐私

| 方面 | 设计 |
|---|---|
| 工作区信任 | 不受信任时：不执行构建工具、编译器与 git，不使用模型；需要执行进程的查询、校验、审查返回原因 |
| 模型数据流 | 默认不使用模型；开启需显式设置；`--explain-context` 列出将要发送的文件与片段；支持排除路径；日志默认不含代码 |
| 遥测 | 不收集（沿用 v1） |
| 守护进程 | 只监听 127.0.0.1；令牌文件仅当前用户可读；版本不一致时不复用 |
| 写入范围 | 只写用户缓存目录；所有修改都以代码操作或补丁的形式交给使用者 |
| 提示注入 | 代码与注释只作为数据，放在分隔区块中；输出必须符合 Schema 并引用证据；模型输出不执行、不自动应用；补丁先校验 |
| MCP 工具 | 全部标注为只读；需要执行进程的工具遵循工作区信任 |

---

# 第四部分　实施

## 12. 改名实施（D29）

| 对象 | 现在 | 改为 |
|---|---|---|
| 仓库、包、发布产物 | lsp-mcpp | mcpp-language-server |
| 可执行文件 | lsp-mcpp、lsp-mcpp-conformance、lsp-mcpp-lspgen、lsp-mcpp-mock-mcpp | mcppls、mcppls-conformance、mcppls-lspgen、mcppls-mock-mcpp |
| 模块、命名空间 | `lspmcpp.*`、`lspmcpp::` | `mcppls.*`、`mcppls::` |
| 路径包 | lsp-mcpp-testing、lsp-mcpp-os-* | mcppls-testing、mcppls-os-* |
| 环境变量 | `LSP_MCPP_*`：缓存目录、payload、日志级别、引擎参数、测试用等 8 类 | `MCPPLS_*` |
| 用户缓存目录 | `<缓存>/lsp-mcpp/` | `<缓存>/mcppls/`（尚未发布，不做迁移） |
| VS Code 扩展 | `mcpp-community.lsp-mcpp`，设置与命令 `lspMcpp.*` | `mcpp-community.mcpp-language-server`，`mcppls.*`；显示名暂时保持"C++ Modules"，AI 功能发布时再定 |
| 工具包 | lsp-mcpp-kit，xlings 仓库目录 `xim-x-lsp-mcpp-kit` | mcppls-kit，`xim-x-mcppls-kit` |
| xlings 描述模板 | `lsp-mcpp.lua.in`、`lsp-mcpp-kit.lua.in` | `mcpp-language-server.lua.in`、`mcppls-kit.lua.in` |
| 诊断来源标签 | `lsp-mcpp · gcc 16` | `mcppls · gcc 16` |
| 规范、README、CI、打包脚本 | 旧名 | 新名 |
| 保持不变 | `.agents/docs` 历史文档、S3 方法 `cxxModules/*`、上游 issue 与 PR 中的文字 | — |

**做法：**
- 改名单独做成一个机械提交，涉及约 175 个文件：只改名，不移动目录、不改行为。目录重组放到引擎抽象层阶段。
- GitHub 仓库改名，旧地址会自动重定向；公开仓库名为 `mcpp-community/mcpp-language-server`。

**验收：**
- 除历史文档外，仓库中不再出现 `lsp-mcpp`、`lspmcpp`、`lspMcpp`、`LSP_MCPP`。
- CI 18 个任务与 nightly 全部通过，夹具结果与改名前一致。

## 13. 需要先做的实验

| 编号 | 问题 | 决定什么 |
|---|---|---|
| X1 | 两个 clangd 实例共用同一个数据库目录和持久化模块缓存时：锁是否可靠；用未保存内容构建的 BMI 会不会写进共享缓存；后台索引是否冲突 | 片段校验用的校验实例方案；实例协调时能否共享缓存 |
| X2 | `openkal.net` 在三个主机上：监听 127.0.0.1 随机端口、半关闭、多连接各一个读线程；Windows 上令牌文件的权限 | 守护进程的连接方式 |
| X3 | `llmapi` 及其依赖 `tinyhttps` 能否基于 openkal 构建 | 模型网关的构建与发布形态 |
| X4 | Claude Code 的 LSP 插件接入 mcppls：启动参数、初始化耗时、编辑后的诊断如何送达、未保存内容如何同步 | 插件形态；编辑后校验的实现 |
| X5 | Copilot CLI 通过 `.github/lsp.json` 接入 mcppls | 配置示例 |
| X6 | 主流代理对 MCP 的支持：stdio 服务端的生命周期、工具结果的大小上限与超时、是否支持 sampling | MCP 工具粒度；`mcp-sampling` 来源是否可用 |
| X7 | 导出面提取：声明级扫描的准确度；用 clangd 的 `documentSymbol`、悬停或 `textDocument/ast` 补全签名的成本 | 模块接口摘要与语义 diff 的实现 |
| X8 | 基线版本的语义 diff：用 `git show` 取出内容交给扫描器，覆盖面与耗时如何 | 审查第 2 步的实现 |
| X9 | 跨编译器校验的构建目录：mcpp 能否把构建产物放到工程之外（文档中未找到对应选项） | 跨编译器校验能在编辑器与代理中运行，还是只在 CI 中运行 |
| X10 | `textDocument/symbolInfo` 返回的 USR 在模块工程中跨翻译单元是否一致 | 符号标识方案 |

## 14. 路线图与工作项

### 阶段 0：改名（D29）

| 编号 | 工作项 | 验收 |
|---|---|---|
| R1 | 模块、命名空间、可执行文件、路径包、环境变量改名 | 构建与单元测试（dev、release）三主机通过 |
| R2 | VS Code 扩展 ID、设置、命令；打包脚本、xlings 模板、kit 名称 | payload 与 VSIX 端到端测试通过 |
| R3 | 规范、README、一致性 README、CI 名称 | `specs/tools/validate.py` 通过；检索旧名为空 |

### 阶段 1：引擎抽象层（D30、D31）

| 编号 | 工作项 | 验收 |
|---|---|---|
| E1 | `engine` 接口、能力声明、特征、注册表、内部查询接口 | 单元测试用假引擎覆盖路由与合并 |
| E2 | 编排器：从 v1 的 `workspace`、`documents`、`router` 中抽出与协议无关的部分 | 36 个夹具结果不变 |
| E3 | clangd 引擎：收进模块准备、模块提示渲染、缓存探测、日志解析；由能力行驱动补偿（含对齐分配绕过） | 同上；MSVC 夹具通过 |
| E4 | mcppls 引擎 N1：模块索引与模块能力作为引擎 | 同上 |
| E5 | payload 清单版本 3；kit 按引擎版本精确选择；修正 PATH 回退 | payload 夹具、`inferred`、`untrusted` 通过 |
| E6 | S3 `engines` 字段；VS Code 扩展读取它 | S3 用例；扩展端到端测试 |
| E7 | 实例协调（6.3 节第一步） | 新夹具：同一工作区两个实例 |
| E8 | `mcppls.engine` 选择（`clangd`、`none`） | 新夹具：`none` 下模块功能可用 |

### 阶段 2：代理语义底座

| 编号 | 工作项 | 验收 |
|---|---|---|
| A1 | Claude Code LSP 插件、Copilot CLI 配置、接入文档（依赖 X4、X5） | 两个代理在两类形态上完成导航与编辑后诊断 |
| A2 | 代理任务基准 v0：10–20 个任务，clangd-lsp 与 mcppls LSP 两组 | 基线数据入库；门槛确定 |
| A3 | 工作区守护进程与连接（依赖 X2） | 多使用者夹具：编辑器与代理同时查询 |
| A4 | `ai/query` 与 S5 的查询部分（依赖 X10） | S5 用例覆盖两类形态 |
| A5 | mcppls 引擎 N2 与 `ai/context`（依赖 X7） | 模块接口摘要、构建上下文用例 |
| A6 | `ai/mcp` 工具（依赖 X6）；命令行 `query`、`diagnostics` | MCP 与命令行用例；基准增加 MCP 组 |
| A7 | `ai/verify` 的编辑后校验与片段校验（依赖 X1） | 校验用例；性能达到 2.4 节的建议值 |

### 阶段 3：审查规则层

| 编号 | 工作项 | 验收 |
|---|---|---|
| RV1 | 变更收集与语义 diff（依赖 X8） | 语义 diff 用例 |
| RV2 | 影响分析与测试选择 | 影响分析用例 |
| RV3 | mcppls 引擎 N3 与首批规则 | 在审查夹具上精确率达标的规则默认开启 |
| RV4 | 发现的结构、LSP 诊断与代码操作、SARIF、S5 的审查部分 | 编辑器端到端测试；SARIF 经 GitHub 代码扫描上传验证 |
| RV5 | 审查夹具与 nightly 统计 | 精确率、召回率进入 nightly |
| RV6 | 跨编译器校验（依赖 X9；先在 CI 中提供） | mcpp 夹具在 GCC 与 LLVM 上能发现只在一侧失败的改动 |

### 阶段 4：模型层

| 编号 | 工作项 | 验收 |
|---|---|---|
| M1 | 模型接入抽象，以及 `none`、`agent` 两种来源 | MCP 与命令行返回变更上下文 |
| M2 | 网关协议与参考网关 `mcppls-model`（依赖 X3） | 本地 OpenAI 兼容端点与一个云端服务可用 |
| M3 | 证据驱动的模型审查：模板、结构化输出、证据校验、缓存、预算 | 模型层在审查夹具上精确率 ≥ 70% |
| M4 | 修复的生成与校验 | 只展示通过校验的修复 |
| M5 | `mcp-sampling` 与 `client` 来源（依赖 X6） | 至少一个代理与 VS Code 可用 |
| M6 | 隐私控制：显式开启、`--explain-context`、排除路径 | 端到端测试：未开启时零网络请求 |

### 阶段 5：远期

- 头文件到模块的迁移助手：mcppls 引擎 N4 做分析，模型负责命名，编译器负责验证。
- 构建与模块错误的解释。
- 用模块图与导出面生成架构概览。
- clice 引擎评估（也可以提前到阶段 2 之后）。

## 15. 风险

| 风险 | 影响 | 应对 |
|---|---|---|
| 引擎重构引入回归 | 核心标准退化 | 阶段 1 不改行为，以 36 个夹具结果一致作为合并门槛；用假引擎做单元测试 |
| 守护进程与多使用者带来状态复杂度 | 难以复现的并发缺陷 | 先做实例协调；守护进程沿用"主线程独占状态"；多使用者夹具与压力测试 |
| 代理生态变化快 | 插件与接入方式失效 | 只依赖 LSP、MCP 等标准协议；插件与配置保持很薄；基准持续运行，尽早发现问题 |
| 审查误报 | 用户与代理失去信任 | 证据驱动；规则按精确率门槛默认开启；模型层需要显式开启，且有门槛 |
| 模型成本与延迟 | 体验与费用不可控 | 只在显式触发时调用；缓存；预算；`agent` 来源不产生额外调用 |
| 提示注入 | 模型输出被代码中的文字操纵 | 第 11 节的约束；输出不执行、不自动应用 |
| openkal 缺少域名解析与 TLS | 服务端无法直接调用云端模型 | 模型调用放到网关进程，或交给代理与编辑器客户端 |
| clangd 模块支持仍是实验性的 | 能力上限受限 | 引擎抽象层；评估 clice；mcppls 引擎按能力逐步接管 |
| 范围扩大拖慢 v1 发布 | 核心标准迟迟不能交付 | 每个阶段单独验收；AI 阶段不阻塞阶段 0、1 的发布决定 |
| 基准需要模型服务费用 | 评估难以常态化 | 基准按周或按版本运行；任务规模可调 |

## 16. 需要 review 的要点

1. **建议决策：** D35–D40 六项是否采纳，即一个内核三个入口、证据驱动审查、模型来源抽象且默认关闭、多使用者分两步、S5、基准与门槛。
2. **AI 目录划分：** `src/ai/` 六组模块的划分与边界（7.0 节）。尤其是两点：审查引擎作为引擎接入编排器；`ai/query` 放在 `ai` 下而不是核心层。
3. **mcppls 引擎：** 成长阶段 N1–N4，以及"接管某项能力须有夹具证明不差于原引擎"的规则。
4. **MCP 工具：** 7.6 节 9 个工具的粒度与命名。
5. **审查规则：** 首批规则（7.4 节）与默认开启的门槛（规则 ≥ 90%、模型 ≥ 70%）。
6. **度量：** 2.4 节的建议值。
7. **基准预算：** 代理任务基准的规模与模型服务预算（10.2 节）。
8. **改名时机：** 改名提交放在 PR #1 的分支上，还是等 PR #1 合入之后。
9. **VS Code 扩展：**
   - 显示名何时调整；
   - 是否接受把 v1 的"命令 ≤ 4、设置 ≤ 4"改为"基础 + AI 分组"；
   - 阶段 2 起 `mcppls serve` 是否默认经守护进程。
10. **实验优先级：** X1–X10 中，建议先做 X4、X6、X10，它们决定代理接入方式与查询形态。

---

## 附录 A：v1 模块到新模块

| v1 模块 | 新模块 |
|---|---|
| `lspmcpp.base.*` | `mcppls.base.*` |
| `lspmcpp.platform.*` | `mcppls.platform.*`（新增 `mcppls.platform.net`） |
| `lspmcpp.lsp.*` | `mcppls.lsp.*` |
| `lspmcpp.spec.*` | `mcppls.spec.*`（新增 `mcppls.spec.query`） |
| `lspmcpp.project.*`、`lspmcpp.toolchain.*` | `mcppls.project.*`、`mcppls.toolchain.*` |
| `lspmcpp.normalize.gnu`、`.msvc`、`.semantic` | `mcppls.normalize.*` |
| `lspmcpp.normalize.plan` | `mcppls.normalize.plan`（与引擎无关的部分）+ `mcppls.engine.clangd.database`（数据库与模块提示渲染） |
| `lspmcpp.engine` | `mcppls.engine`（接口重新设计） |
| `lspmcpp.engine.clangd` | `mcppls.engine.clangd.*` |
| `lspmcpp.index.modules` | `mcppls.engine.native.index` |
| `lspmcpp.server.primer` | `mcppls.engine.clangd.primer` |
| `lspmcpp.server.payload` | `mcppls.engine.payload` |
| `lspmcpp.server.documents` | `mcppls.orchestrator.documents` |
| `lspmcpp.server.router` | `mcppls.orchestrator.routing` |
| `lspmcpp.server.workspace` | `mcppls.orchestrator.workspace` + `mcppls.server.status` |
| `lspmcpp.server.session` | `mcppls.server.session` |
| `lspmcpp.server.cli` | `mcppls.cli.*` |
| `lspmcpp.os`、`lspmcpp.testing` | `mcppls.os`、`mcppls.testing` |

## 附录 B：来源

**代理与语言服务器**
- [Claude Code Docs：Discover and install prebuilt plugins（代码智能插件）](https://code.claude.com/docs/en/discover-plugins)
- [GitHub Docs：Using LSP servers with GitHub Copilot CLI](https://docs.github.com/en/copilot/concepts/agents/copilot-cli/lsp-servers)
- [@github/copilot-language-server（npm）](https://www.npmjs.com/package/@github/copilot-language-server)
- [github/copilot-language-server-release](https://github.com/github/copilot-language-server-release)

**代理协议与工具**
- [Zed：Agent Client Protocol](https://zed.dev/acp)
- [JetBrains × Zed：Open Interoperability for AI Coding Agents](https://blog.jetbrains.com/ai/2025/10/jetbrains-zed-open-interoperability-for-ai-coding-agents-in-your-ide/)
- [oraios/serena](https://github.com/oraios/serena)
- [mpsm/mcp-cpp](https://github.com/mpsm/mcp-cpp)

**AI 审查**
- [Sonar：Best AI Code Review Tools（2026-07-09）](https://www.sonarsource.com/resources/library/best-ai-code-review-tools/)

**协议**
- [LSP 3.18 规范](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.18/specification/)（仓库内 `tools/lspgen/metaModel-3.18.json` 已核对 `textDocument/inlineCompletion`、`textDocument/diagnostic`、`workspace/diagnostic`、`codeAction/resolve`、`window/showDocument` 与 `DiagnosticOptions.identifier`）

**本地核对**
- lsp-mcpp 提交 8119917 的源码（第 1.2 节）
- clangd 23.1.0 源码（llvmorg-23.1.0，`ClangdLSPServer.cpp` 中的 `textDocument/symbolInfo`、`textDocument/ast`）
- mcpp 2026.9.15.1 的同名源文件构建实测
- openkal `src/net.cppm`（不含域名解析）
- mcpplibs `llmapi` 0.2.8 的 `mcpp.toml`（依赖 `tinyhttps`）
