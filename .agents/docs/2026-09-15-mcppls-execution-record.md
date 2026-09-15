# mcppls 总体设计执行记录

| | |
|---|---|
| 依据 | [总体设计](2026-09-15-mcppls-overall-design.md)（D29–D40，工作项 R/E/A/N/RV/M） |
| 分支 | `feat/mcppls`（PR #3） |
| 日期 | 2026-09-15 |

本文记录总体设计的实施结果：每个工作项做到了什么、怎样验证、与设计的偏差及原因、尚未完成的部分。

## 1. 工作项对照

### 阶段 0：改名（D29）

| 编号 | 结果 | 验证 |
|---|---|---|
| R1 | 模块、命名空间、可执行文件、路径包、环境变量改为 `mcppls` / `MCPPLS_*` | 三主机构建与单元测试 |
| R2 | VS Code 扩展 `mcpp-community.mcpp-language-server`、设置与命令 `mcppls.*`；打包脚本、xlings 模板、`mcppls-kit` | payload 与 VSIX 端到端 |
| R3 | 规范、README、一致性说明、CI 名称；历史文档保留旧名 | `specs/tools/validate.py` |

未做：GitHub 仓库改名（`Sunrisepeak/lsp-mcpp-private` → `mcpp-community/mcpp-language-server`）是对外操作，需要仓库所有者执行。

### 阶段 1：引擎抽象层（D30、D31）

| 编号 | 结果 |
|---|---|
| E1 | `mcppls.engine`：`Engine`、`Host`、能力声明（answer / fallback / merge 与优先级）、`EngineTraits` |
| E2 | `mcppls.orchestrator.*`：工作区、文档、路由与合并、客户端汇 |
| E3 | clangd 引擎收进模块准备、模块提示、缓存、日志解析；补偿由特征行驱动 |
| E4 | mcppls 引擎 N1：模块索引作为引擎，模块名相关请求优先回答 |
| E5 | payload 清单版本 3（`engines.clangd`），kit 按 libc++ 版本精确匹配，不匹配时给出 `kit-version-mismatch` |
| E6 | S3 `engines` 字段与 VS Code 状态提示 |
| E7 | 实例协调：`owner.lease` 租约（10 s 续租、30 s 过期），后来的实例使用私有目录并提示 `shared-workspace` |
| E8 | `--engine none` / `mcppls.engine`，夹具 `engine-none` |

### 阶段 2：代理语义底座

| 编号 | 结果 |
|---|---|
| A1 | Claude Code 插件（LSP + MCP，`claude plugin validate --strict` 通过）、Copilot CLI 的 `lsp.json` 与 `mcp-config.json`、接入说明 |
| A2 | 代理任务基准 v0：10 个任务（两类工程形态、GCC 与 LLVM），`bench/run.py validate` 进 CI；`run` 的四个对照组（含 MCP 组）已实现但未运行 |
| A3 | 工作区守护进程：环回地址 + 令牌，多条 MCP 连接共享一个热会话；`mcppls mcp --daemon`、`mcppls daemon run/start/status/stop`；空闲 30 分钟退出 |
| A4 | `ai/query`：符号（名字、USR、位置）、引用、调用者与被调用者、文件大纲、模块描述与模块图、新鲜诊断；S5 查询部分 |
| A5 | `ai/context`：模块接口摘要（N2 导出面提取，按预算裁剪）、构建上下文 |
| A6 | `ai/mcp`：9 个只读工具（符号、引用与调用、大纲、模块、构建上下文、校验、影响、审查、诊断）；命令行 `query`、`diagnostics`；MCP 2024-11-05 / 2025-03-26 / 2025-06-18 |
| A7 | `ai/verify`：编辑后校验（改动文件及其搜索范围内的导入方）、片段原位校验 |
| N2 | mcppls 引擎导出面提取（词法级，26 个用例） |

### 阶段 3：审查规则层

| 编号 | 结果 |
|---|---|
| RV1 | 变更收集（git，只在受信任工作区，`--no-optional-locks`）、Myers 行 diff、语义 diff（模块声明、导入、导出面） |
| RV2 | 影响分析：搜索范围、改动导出名的使用（引擎引用 + 词法使用）、涉及的集合与测试集合 |
| RV3 | 7 条规则：`module/export-removed-in-use`、`module/export-signature-changed`、`module/partition-misuse`、`module/import-unresolved`、`build/diagnostic-introduced`、`build/toolchain-divergence`、`test/exported-change-untested` |
| RV4 | 发现结构（S5 5.1）、SARIF 2.1.0、LSP 诊断、Markdown；`cxx_impact`、`cxx_review`、`mcppls impact`、`mcppls review` |
| RV5 | 审查夹具 `bench/review/`（13 个，含两个干净补丁、一个模型夹具、一个跨工具链夹具），CI 三主机运行并输出每条规则的精确率与召回率 |
| RV6 | 跨工具链校验：在用户缓存中的工程副本里用各工具链增量构建，比较错误 |

### 阶段 4：模型层

| 编号 | 结果 |
|---|---|
| M1 | 模型来源抽象：none、agent、gateway、mcp-sampling、client |
| M2 | 参考网关 `mcppls-model`（独立 mcpp 包，常规工具链构建，mock OpenAI 端点 25 项检查） |
| M3 | 证据驱动的模型审查：版本化模板、数据块转义、输出 Schema 校验、证据编号校验、按内容哈希缓存、token 预算 |
| M4 | 模型给出的修复在 kernel 中以覆盖层原位校验，只保留不引入错误的修复 |
| M5 | `mcp-sampling` 来源（`sampling/createMessage`）；`client` 来源未绑定编辑器 |
| M6 | 显式开启（只能由启动服务器或运行命令的人开启）、`--explain-context`、排除路径 |

### 编辑器侧 AI 功能（7.7）

`workspace/executeCommand` 的 `mcppls.review.run` / `mcppls.review.clear`；VS Code 在 `mcppls.ai.enabled` 下显示“Review Changes”“Clear Review”。

## 2. 与设计的偏差

| 编号 | 设计 | 实际 | 原因 |
|---|---|---|---|
| X-1 | 引用、调用者由 clangd 索引回答 | 先打开符号所在模块的单元与其导入方（含经重导出者），等 AST 建好后再问；调用者由引用加外围函数推导 | clangd 23.1 后台索引不构建模块导入（"Failed to compile …, index may be incomplete"），导入方中的引用缺失；两个程序各自的 `main` USR 相同，调用层级会丢调用者 |
| X-2 | 符号定义来自 `symbolInfo` | 缺定义位置时打开模块自身单元后再请求 `textDocument/definition` | 从导入方位置查询时 `symbolInfo` 不带另一单元里的定义 |
| X-3 | 片段校验在独立的校验实例中进行（实验 X1） | 在 kernel 自己的 clangd 中以覆盖层进行，结束后恢复 | kernel 属于 MCP / 命令行会话，不与编辑器共享文档；X1 未做 |
| X-4 | 审查引擎作为引擎接入编排器，拉取式诊断 `identifier: mcppls-review` | LSP 会话以子进程运行 `mcppls review --format lsp`，发现合并进推送诊断（source `mcppls review`） | 审查流水线运行在 headless kernel 上；在 LSP 事件循环里同步等待会阻塞编辑器。推送诊断在所有客户端都可用 |
| X-5 | 守护进程承载 LSP、MCP、内部查询三类会话，`mcppls serve` 默认经守护进程 | 守护进程只承载 MCP 与控制连接；`mcppls serve` 仍在自身进程 | 编辑器的未保存内容与代理的磁盘读取共享一个工作区，需要多客户端的工作区（覆盖层归属、文档版本、诊断分发），风险大；实例协调已保证两者可同时工作 |
| X-6 | 跨工具链校验的构建目录（实验 X9） | 同步副本到 `<缓存>/workspaces/<键>/toolchains/<工具链>/`，增量构建 | mcpp 不支持把构建产物放到工程外 |
| X-7 | 参考网关基于 llmapi | 基于 tinyhttps，http 端点自带 HTTP/1.1 实现 | tinyhttps 的 HttpClient 拒绝非 https 地址（X3 结论记在 `model-gateway/README.md`） |
| X-8 | MCP 取消 | 请求依次处理，`notifications/cancelled` 不中断进行中的工具调用 | 单线程事件循环；工具调用都有期限 |
| X-9 | clangd 进程停止 | 先请求退出，再发 SIGTERM，2 秒后结束其进程组 | 模块图非法（导出实现分区）时 clangd 卡住不响应 SIGTERM，v1 的重启路径同样会挂起 |
| X-10 | `client` 模型来源 | 未绑定 | 需要 VS Code 语言模型 API 与自定义请求，留待编辑器侧 AI 功能继续 |

## 3. 验证

- 单元测试 25 个可执行文件（新增 test_query、test_review、test_model、test_exports、test_net）。
- 一致性夹具：原有夹具全部保持通过；新增 MCP、命令行、守护进程、编辑器审查命令检查，覆盖 `mcpp-split`、`mcpp-all-cppm`、`engine-none`、`verify-changes`，三主机运行。
- 审查夹具：13 个，三主机通过（跨工具链夹具需要 GCC 与 LLVM 两套工具链，只在 Linux 运行，另两台主机跳过）。7 条确定性规则的精确率、召回率均为 100%；模型夹具中带证据的模型发现、校验通过的修复、校验不通过的修复各一个都被识别，编造证据的发现被丢弃（夹具规模小，数字只说明规则按预期工作）。
- 规范：S5 72 条规则全部有证据（`validate.py` 219 条规则，0 失败）。
- CI：run 34977817986（提交 `2f399e4`）22 个作业全部通过。一致性检查 Linux 339 项、Windows 316 项、macOS 183 项通过，0 失败；守护进程检查在三主机通过。

CI 发现并修复的问题：

| 问题 | 原因 | 修复 |
|---|---|---|
| Windows 上守护进程检查挂起 30 分钟（run 34966008745，取消） | 守护进程在装好日志文件之前把启动日志写到启动者给的错误管道，无人读取，写满后阻塞在写发现文件之前；Windows 上放置标准流的启动会把启动者的全部可继承句柄交给子进程，守护进程因此持有代理的输出管道，代理要等守护进程空闲退出才读到输出结束 | 日志从第一行起写文件；Windows 上启动守护进程不放置任何标准流（不继承句柄）；连接后不应答的守护进程 30 秒超时（`20f4410`） |
| 修复上一项后 Windows 上仍然挂起（run 34972803140，取消） | openkal-windows 的套接字不带重叠属性，系统把同一句柄上的操作串行化：读线程等待数据时，另一线程的写被挡住。中继转发请求、守护进程回复都是“一个线程读、另一个线程写”，双方互相等待 | Windows 上读以 10 ms 为一片等待就绪、只读已到达的字节，片与片之间让给等待中的写、半关闭、关闭；写等有空间后分块写。新增单元测试：一个线程读的同时另一个线程写与关闭，旧实现在 Wine 下死锁，新实现在 Wine 与 Windows CI 通过（`2f399e4`） |
| 审查代理发现 | 审查分词器不认带编码前缀的原始字符串（`LR"(…)"`、`u8R"(…)"`）；kernel 请求超时后迟到的响应留在响应表里，守护进程长期运行会累积 | 分词器识别 R/LR/uR/UR/u8R 前缀；超时请求的迟到响应被丢弃（`2a6bd28`） |

## 4. 未验证与后续

- 真实代理（Claude Code、Copilot CLI）与真实模型服务未运行：需要密钥与预算；基准 `run` 与网关的真实端点只经 mock 验证。
- `mcp-sampling` 没有支持 sampling 的客户端验证（实验 X6）。
- VS Code 扩展的审查命令只经 LSP 一致性检查验证，未进 VS Code 端到端测试。
- 性能指标（10.4）未进 nightly。
- GitHub 仓库改名需要仓库所有者操作。
- openkal-windows 的套接字默认可继承（未设 `WSA_FLAG_NO_HANDLE_INHERIT`），守护进程启动的 clangd、mcpp 会继承监听与连接套接字；非重叠套接字上的操作被串行化，mcppls 目前在自己的连接层分片绕开。两点都宜在 openkal 中修正。
