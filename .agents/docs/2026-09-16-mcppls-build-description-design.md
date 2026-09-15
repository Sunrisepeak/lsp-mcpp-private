# mcppls 构建描述与外部工具运行设计

| | |
|---|---|
| 起因 | 2026-09-16 在 VS Code 中打开 openxlings/xlings，mcppls 连跳转都失效。mcpp 在 `emit build-database` 的规划阶段同步调用 `xlings update`，后者挂在一条不经代理的 GitHub 连接上；mcppls 被拖住约 12 分钟，期间没有引擎数据库，最后退回推断模型并覆盖了缓存 |
| 目标 | 获取构建描述永远不影响可用性；运行用户的构建工具时尽量与用户终端一致；默认不联网；三平台行为一致；用户无感；启动快 |
| 状态 | 方案。第 9 节决策 1–5 已确认（2026-09-16）；决策 6：向 mcpp 提适配 issue（M1–M6），xlings 的 X1、X2 待定；决策 7 待定 |
| 日期 | 2026-09-16 |
| 核对对象 | mcppls feat/mcppls @ 3ebe9a5；mcpp 2026.9.15.1（源码 mcpp-community/mcpp @ 2fc7b5b0）；VS Code 1.125.1 |

## 1. 现场证据

### 1.1 时间线（UTC，服务端日志与进程表）

| 时间 | 事件 |
|---|---|
| 19:55:31 | VS Code 启动 mcppls；恢复上一次模型的模块索引（763 个文件）。只恢复索引，不生成引擎数据库 |
| 19:55:32 | 运行 `~/.xlings/subos/current/bin/mcpp emit build-database --format json`。VS Code 的 PATH 里没有 xlings 目录，这个路径是 mcppls 的后备路径；它是 xvm 的 shim，按工程的 `.xlings.json` 选到 `xim-x-mcpp/2026.9.15.1` |
| 19:55:33 | mcpp 在规划阶段用 `popen` 同步执行 `sh -c "cd ~/.mcpp/registry && env -u XLINGS_PROJECT_DIR -u XLINGS_ACTIVE_SUBOS PATH=… XLINGS_HOME=~/.mcpp/registry xlings update 2>&1 </dev/null"`，自己阻塞在读取上。`xlings update` 连 `[2606:50c0:8003::154]:443`（GitHub），发送队列 85 字节始终没有确认，没有超时 |
| 19:57:31 | mcppls 等首个计划满 120 s，“serving without it”：clangd 没有数据库，报 `Failed to get Project Modules information`，跳转全部失效 |
| 20:00:32 | 5 分钟期限到，mcppls 结束 mcpp，只结束了这个直接子进程；`sh` 与 `xlings update` 被 systemd 收养 |
| 20:00:32–20:07:00 | mcpp 已退出，但 `sh` 与 `xlings update` 的 fd 3 与 mcppls 的 fd 11 是同一个 `pipe:[428462498]`，即 mcppls 给 mcpp 的标准输出管道。mcppls 的读取线程停在 `pipe_read`，模型加载线程无法结束 |
| 20:07:00 | `xlings update` 结束，管道关闭 |
| 20:07:01 | 记下 `discovery-timeout`，退回推断模型（1 个集合、level 2、10 个占位模块、85 个问题），并用它覆盖了缓存里原来的 mcpp 模型（24 个集合、level 3） |
| 20:15:23 | 用户重启 VS Code。索引已刷新，mcpp 3.3 s 给出描述，恢复正常；实现单元的定义查找也在其后正常工作 |

fd 3 的来历：`emit build-database` 在规划期间把标准输出转到标准错误，原标准输出用 `dup(1)` 保存（`modules/platform/src/terminal.cppm:82`，没有 close-on-exec），最低空闲描述符正是 3。规划中经 `popen` 启动的每个程序都继承了它（mcpp 注释原话：“Planning narrates on stdout and may start programs that inherit it”，`src/cli/cmd_build.cppm:342`）。

### 1.2 为什么和用户终端的环境不一样

| | 用户的 GNOME Terminal 里的 fish | VS Code 集成终端里的 fish | VS Code 扩展宿主，即 mcppls 与它启动的程序 |
|---|---|---|---|
| PATH 中的 `~/.xlings/subos/current/bin`、`~/.xlings/bin` | 有 | 有 | 没有 |
| `XLINGS_HOME`、`XLINGS_BIN` | 有 | 有 | 没有 |
| `http_proxy`、`https_proxy`、`ALL_PROXY` | 有 | 没有 | 没有 |

两个原因：

1. **VS Code 没有解析登录 shell 的环境。** 它只在不是从命令行启动时解析（1.125.1 `out/main.js`：`resolveShellEnv(): skipped (Windows)`、`skipped (VSCODE_CLI is set)`，除非 `--force-user-env`）。xvm 生成的桌面项 `Exec=…/xim-x-code/1.125.1/bin/code` 运行的是命令行脚本，它设置 `VSCODE_CLI=1`，扩展宿主于是只有桌面会话的环境。fish 配置里经 `xlings-profile.fish` 加入的 PATH 与 `XLINGS_*` 都不在其中。
2. **代理变量不在任何 shell 配置里。** fish 配置、`~/.profile`、`~/.bashrc`、`environment.d` 都没有，GNOME Terminal 的服务进程也没有；它们只在用户那个终端窗口里 shell 的初始环境中，VS Code 的集成终端里没有。这类随会话而来的变量，编辑器无法复现。

这不是 xlings 独有的：macOS 从 Dock 启动的程序只有 launchd 的环境；Linux 上经命令行脚本启动的桌面项、其他从图形界面启动的编辑器都有同样的问题。Windows 的图形程序从注册表取环境，通常与终端一致，但 shell 配置文件中设置的变量同样不在其中。

S2-6-3 要求发现命令“以编辑器会话的环境启动”。本意是不添加额外凭据，但字面上把偶然的启动环境当成了用户环境。

### 1.3 mcpp 的相关行为（源码 @ 2fc7b5b0）

| 方面 | 事实 | 位置 |
|---|---|---|
| `emit build-database` 做什么 | 与 `mcpp build` 走同一个 `prepare_build`（`plan_only`，私有工作目录，不写工程），依赖与工具链解析是真实执行的 | `src/cli/cmd_build.cppm:94-96, 342-374` |
| 何时联网 | ① 注册表依赖：`decide_for_dependency` 判定需要时刷新一次索引，受 `[index] auto_refresh` 与 120 s 去抖约束；② 工具链或 xim 包缺失、自动安装前：`ensure_official_package_index_fresh`，**不受** `auto_refresh` 约束；③ 工程自定义 `[indices]` 首次同步：直接 `update_index`，也不受 `auto_refresh` 约束 | `src/build/prepare.cppm:4449-4476`、`src/pm/index_refresh.cppm:284`、`src/pm/package_fetcher.cppm:1120-1129`、`src/xlings/xlings.cppm:1988`、`src/build/prepare.cppm:4644-4662` |
| 刷新方式 | 同步：`popen` 加阻塞读取加 `pclose`，在规划线程上执行；刷新失败只警告，之后仍用本地数据解析 | `modules/platform/src/process.cppm:550-579`、`src/build/prepare.cppm:4465-4472` |
| 超时 | 对 xlings 的任何调用都没有期限；只在非零退出时重试 3 次 | `src/xlings/xlings.cppm:1952-1967` |
| 离线 | `--offline` 等同 `MCPP_OFFLINE=1`（非空且不为 `0`）：不刷新索引、不下载、不自动安装工具链、不访问 git；`emit build-database` 同样遵守。不阻止 `build.mcpp` 执行与写全局缓存 | `modules/platform/src/env.cppm:103-108`、`src/cli.cppm:96, 172`、`docs/05-dependencies.md:290` |
| 离线时缺东西 | 信封失败：`MCPP_BUILD_DATABASE_PLAN_FAILED`，消息如 “offline mode: `pkg@ver` is not installed and cannot be downloaded / run without --offline (or unset MCPP_OFFLINE) to fetch it”，退出 1；没有专门的错误码，只能匹配消息文本 | `src/cli/cmd_build.cppm:302-318, 375`、`src/pm/package_fetcher.cppm:1114-1119` |
| 声明的效应 | `--protocol-version` 的静态表中 `emit build-database` 含 `network`；每次运行的信封 `effects` 只有 `read-project`、`write-global-cache`（和 `exec-build-script`），即使这次刷新了索引也不含 `network` | `src/cli.cppm:1075-1077`、`src/cli/cmd_build.cppm:427-428` |
| 子进程与描述符 | 调用 xlings 用 `popen`/`std::system`，没有独立进程组，也不关闭无关描述符；全仓库只有索引锁的描述符带 `O_CLOEXEC` | `modules/platform/src/process.cppm:492-592`、`modules/platform/src/fs.cppm:314` |
| 镜像 | `mcpp self config --mirror CN|GLOBAL` 实际执行 `xlings config --mirror`，只影响 mcpp 私有仓库；它选择包描述中 GLOBAL/CN 两套下载地址。mcpp-index 的仓库与制品地址是写死的 GitHub 常量，没有 CN 一套（CHANGELOG 记为“待部署后再扩”） | `src/cli/cmd_self.cppm:127-129`、`src/config.cppm:55-60` |
| 代理 | 不读、不设、不清除代理变量，原样传给 xlings | `src/xlings/xlings.cppm:1237-1255` |

### 1.4 实验

| 实验 | 结果 |
|---|---|
| 用户终端环境中 `MCPP_OFFLINE=1 mcpp emit build-database --format json`（xlings） | 2.6 s，退出 0，输出 2,608,930 字节 |
| 模拟 VS Code 环境（`env -i`，只有 `HOME` 与扩展宿主的 PATH）中同样离线运行 | 2.6 s，退出 0，输出与上面逐字节相同 |
| mcppls `platform::run` 启动的子进程拿到的描述符 | 0、1、2 为管道；另有 3、4、12 三个目录描述符（openkal-linux 为 `execveat` 保留的目录不带 `O_CLOEXEC`）；没有多余的管道端 |
| 子进程启动一个继承其标准错误的孙进程后立即退出，`platform::run` 期限 5 s | 30.0 s 后才返回（等孙进程结束），`timedOut=false` |
| 从扩展宿主的环境解析登录 shell：`env -i HOME PATH=扩展宿主的 PATH fish -l -i -c …`，标准输入为空 | 14 ms；得到 `XLINGS_HOME=~/.xlings`，PATH 以 `~/.xlings/subos/current/bin` 开头，`mcpp` 可在 PATH 中找到；没有代理变量 |

结论：

- 离线运行得到完全相同的构建描述，不需要网络。
- 这次挂起由三件事叠加：mcpp 同步等一个没有期限的网络操作；mcpp 泄漏了调用方的管道；mcppls 期限只结束直接子进程，读取又等管道结束。三件事任何一件不成立，都不会挂 12 分钟。
- 真正让语言服务失效的是 mcppls 自己：缓存不参与规划、超时后不带数据库服务、失败时降级并覆盖缓存。

## 2. 问题

### 2.1 mcppls

| 编号 | 问题 | 后果 |
|---|---|---|
| D1 | 构建工具在编辑器进程的环境中运行，与用户终端不同（PATH、`XLINGS_*`、代理、工具链变量） | 找到的 mcpp 可能不同；同一工程在终端与编辑器里得到不同的描述 |
| D2 | 隐式运行（打开工程、保存构建文件）可能触发构建工具联网，没有上限 | 与总体设计 G7“默认配置下的网络请求为 0”冲突；网络异常时挂起 |
| D3 | `platform::run` 期限只结束直接子进程；读取等到管道结束 | 任何持有管道的后代都能无限期拖住调用方（1.4 第 4 行） |
| D4 | 期限过长：生产方 5 分钟，首个计划 120 s | 等待不可接受 |
| D5 | 缓存只保存数据库与来源，启动时只恢复索引，不参与规划 | 构建工具慢或挂起时没有可用的数据库 |
| D6 | 首个计划超时后不带数据库启动 clangd | clangd 对不存在的数据库 30 s 才复查，也不为已打开的文档重读；数据库之后到了，已打开的文件仍然没有命令 |
| D7 | 生产方失败时退回推断模型，并覆盖缓存中更好的模型 | 结果降级，下次启动只能恢复降级后的索引 |
| D8 | 外部程序的运行没有记录 | 卡住时日志里没有线索，只能靠进程表排查 |

工具链探测（运行编译器取事实）、审查中的 git 与 `mcpp build` 共用 `platform::run`，有 D3 同样的风险；例如 `sccache` 首次运行会启动常驻服务并继承描述符。

### 2.2 mcpp 与 xlings

见第 7 节。

## 3. 原则

| 编号 | 原则 |
|---|---|
| P1 | 可用性不依赖任何外部程序：外部程序失败、挂起、联网、不存在，语言服务都照常可用，只是精度不同 |
| P2 | 运行用户的构建工具，用可以复现的用户环境：登录 shell 的环境。会话里临时设置的变量不属于此列 |
| P3 | 隐式运行不联网；联网只由用户发起，或由用户显式允许 |
| P4 | 外部程序有期限；结束时连同它启动的一切一起结束；读取有界，不能拖住服务端 |
| P5 | 模型只升级不降级：更差的来源不替换更好的结果，不覆盖更好的缓存 |
| P6 | 三平台行为一致；做不到的地方显式写明 |
| P7 | 每次外部运行都可观测：命令、环境来源、是否离线、耗时、结局、标准错误末尾 |

## 4. 方案

### 4.1 模型来源与状态

来源按精度从高到低：本次运行的生产方结果 > 输入指纹一致的同类缓存 > 指纹不一致的同类缓存 > 推断模型。

启动：

| 条件 | 立即使用 | 后台 | 升级 |
|---|---|---|---|
| 受信任，工程有生产方（mcpp、CMake、发现命令），有同类缓存且输入指纹一致 | 缓存模型：立即生成引擎数据库并启动 clangd | 离线运行生产方 | 结果相同：只标记为最新；不同：重新规划，需要时按闸门重启 clangd |
| 同上，缓存指纹不一致 | 先等生产方最多 3 s；没有结果就用缓存模型，状态注明“可能过期” | 同上 | 同上 |
| 同上，没有缓存 | 先等生产方最多 10 s；没有结果就用推断模型 | 同上 | 同上 |
| 不受信任，或工程没有生产方 | 推断模型（现有行为） | 无 | — |

生产方的结局：

| 结局 | 处理 |
|---|---|
| 成功 | 如上升级；保存为该来源的缓存 |
| 离线运行说明需要下载（缺包、缺工具链、索引未初始化） | 保留当前模型；状态给出原因与动作（4.4）；不保存缓存 |
| 其他失败 | 保留当前模型；当前模型与失败的来源同类时标记“可能过期”（S2 5 现有行为）；当前是推断模型时附上原因 |
| 硬期限到 | 结束整个单元；保留当前模型；状态注明；构建文件再次变化，或 5 分钟后，再试 |

clangd 启动时总有数据库（缓存或推断），不再有“没有数据库也开始服务”。来源从推断切换到生产方时，已有单元的参数变化按现有规则（robustness design C4）重启 clangd。

缓存：

- 按来源分文件：`model.mcpp.json`、`model.cmake.json`、`model.inferred.json`……推断模型不覆盖生产方的缓存。
- 内容为完整模型：数据库、工具链事实、语义配置、是否用工具包、level、来源、watch、问题与提示、生产方名称与版本。
- 输入指纹：构建描述文件的大小与修改时间（mcpp：`mcpp.toml`、`mcpp.lock`、`.xlings.json`；CMake：watch 列表），生产方可执行文件的路径与版本，工具环境（4.3）中影响生产方的变量的摘要。
- 写入用临时文件加改名；读取失败按没有缓存处理。

### 4.2 外部程序运行器

所有为了了解工程而运行的外部程序（生产方、`--protocol-version`、工具链探测、git、校验与审查中的构建）都经过同一个运行器：

| 方面 | 做法 |
|---|---|
| 进程归属 | 在自己的单元中启动：POSIX 为独立进程组，Windows 为 Job 对象（openkal `kal_spawn.job`，即现有的 `ownUnit`） |
| 标准流 | 标准输入立即关闭；标准输出、标准错误为管道 |
| 读取 | 分片限时读取（`kal_timeout_read`）。子进程退出或被结束后，最多再读 1 s 或读到管道结束；仍未结束说明有后代持有管道：结束整个单元，关闭读端，结果标记 `outputHeldOpen` |
| 期限 | 软期限（生产方 5 s）只用于状态提示；硬期限（生产方 60 s，工具链探测 20 s）先请求结束单元，2 s 后强制结束 |
| 输出上限 | 标准输出 256 MB、标准错误 1 MB，超出截断并标记 |
| 并发 | 同一工作区同一时间只运行一个生产方；运行期间的新触发合并为下一次 |
| 记录 | 事件 `tool-run`：程序、参数、工作目录、环境来源、是否离线、耗时、退出码、是否超时、是否结束了单元、`outputHeldOpen`、标准错误末尾 20 行；报告列出最近 20 次 |

结束单元对 POSIX 上用 `setsid` 脱离进程组的后代无效（mcpp 目前不这样启动 xlings），有界读取对它们仍然有效。Windows 上 openkal-windows 放入标准流时会把进程中所有可继承的句柄交给子进程（守护进程为此改为不放标准流启动，见 20f4410），后代同样可能持有管道，有界读取与结束 Job 对象同样适用；`kal_timeout_read` 对 Windows 管道的行为要先在 Wine 与 windows-2022 上验证（第 8 节 T3）。

### 4.3 工具环境

服务端为外部程序准备“工具环境”。编辑器进程自己的环境不变；clangd 仍用编辑器进程的环境，因为它只读引擎数据库中已经归一化的命令。

POSIX（Linux、macOS）：

1. 服务端启动后在后台解析一次登录 shell 的环境：`$SHELL -l -i -c '<mcppls> print-environment <标记>'`。标准输入为空，期限 10 s，环境中加 `VSCODE_RESOLVING_ENVIRONMENT=1` 与 `MCPPLS_RESOLVING_ENVIRONMENT=1`（与 VS Code 相同，shell 配置可据此跳过重型初始化）。`print-environment` 在两个标记之间输出 JSON，shell 配置打印的其他内容被忽略。`$SHELL` 为空时依次用 `/etc/passwd` 中的登录 shell、`/bin/sh`。
2. 合并：以编辑器进程环境为底，登录 shell 的值覆盖同名变量；不取 `SHLVL`、`PWD`、`OLDPWD`、`_`、`TERM*`、`VSCODE_*`、`ELECTRON_*`、`MCPPLS_*`。
3. 解析失败或超时：用编辑器进程环境，报告中注明原因（例如 “the login shell did not finish within 10 s”）。
4. 结果在服务端生命周期内缓存；“重启语言服务”时重新解析。
5. 解析在后台进行，不阻塞 4.1 的缓存模型路径；没有缓存时，生产方等解析完成（有 10 s 期限）后再启动。

Windows：图形程序的环境来自注册表，用编辑器进程环境，不运行 shell。

能做到的与做不到的：登录 shell 解析能拿到 shell 配置里的 PATH 与 `XLINGS_*`，即 1.2 表中的前两行；拿不到会话里临时设置的变量，即第三行的代理。所以联网行为不能靠环境“与终端一致”来保证，见 4.4。

设置 `mcppls.toolEnvironment`：`auto`（POSIX 解析登录 shell，Windows 用编辑器环境）| `editor`（始终用编辑器进程环境，供 shell 配置有副作用的用户使用）。

隐私：报告只列出工具环境与编辑器环境不同的变量名，不列值；代理变量的值可能含凭据。

规范：S2-6-3 改为“**SHOULD** 以用户会话的环境启动发现命令——编辑器进程的环境缺少用户 shell 配置时，以登录 shell 的环境为准——不添加额外凭据”，同步 traceability 与 CHANGELOG。

### 4.4 网络

默认 `offline`：服务端隐式运行的外部程序不联网。

| 程序 | 离线方式 |
|---|---|
| mcpp | 环境加 `MCPP_OFFLINE=1`，覆盖 1.3 中三处联网。`build.mcpp` 仍会执行，这是受信任工作区才运行生产方的原因之一 |
| CMake 配置（私有构建目录） | CMake 没有通用离线模式。私有构建目录第一次配置时 `FetchContent`、`ExternalProject`、`file(DOWNLOAD)` 可能下载；之后的配置加 `-DFETCHCONTENT_UPDATES_DISCONNECTED=ON`，不再更新已取得的依赖。第一次配置是 P3 的例外，由 4.2 的期限与结束单元兜底，状态中写明（第 9 节决策 7） |
| 发现命令（S2） | 请求中加可选字段 `"network": false`（S2 新增，生产方可以忽略）；期限兜底 |
| 工具链探测、git | 本身不联网 |

识别“需要下载”：mcpp 目前只能匹配消息文本（`offline mode:`、`run without --offline`）；mcpp 提供专门的错误码后改用错误码（第 7 节 M1）。

需要下载时：

- 状态显示“构建描述需要下载依赖（mcpp：<缺少的包>）”，给出两个动作：“复制命令”（`mcpp build`，或 mcpp 消息所指的命令），在用户自己的终端中运行；“在集成终端中运行”。前者保证代理与凭据和用户手动运行时完全相同（1.2 的第三行只有用户自己的终端才有）。
- 窗口重新获得焦点时离线重试一次（两次重试至少间隔 30 s）；集成终端中运行的命令结束时也重试。

设置 `mcppls.buildTool`：

| 值 | 行为 |
|---|---|
| `offline`（默认） | 隐式运行离线；需要下载时给出动作 |
| `online` | 隐式运行允许联网：用工具环境，硬期限放宽到 10 分钟，状态显示进度，仍按 4.2 隔离 |
| `off` | 从不运行构建工具：只用缓存与推断模型 |

镜像：mcppls 不管理镜像。镜像是 mcpp 自己的配置，在任何环境里运行 mcpp 都生效；它能让下载更快，但不改变 P3 与 P4：镜像同样可能慢或不可达，而且 mcpp-index 本身目前没有镜像地址（1.3）。

与总体设计 G7（默认配置下网络请求为 0）的关系：G7 原来只约束服务端进程本身，本设计把它扩展到服务端隐式启动的所有程序。

### 4.5 用户能看到什么

| 情形 | 状态栏 | 功能 |
|---|---|---|
| 缓存命中，后台确认中 | 与正常相同 | 全部可用 |
| 构建工具超过软期限仍在运行 | “正在读取构建描述（mcpp，12 s）” | 全部可用（缓存或推断模型） |
| 需要下载 | “构建描述需要下载依赖”，带动作 | 可用，精度取决于当前模型 |
| 构建工具失败或超时 | “构建描述可能过期：<原因>”，带查看日志 | 可用 |
| 登录 shell 解析失败 | 状态不打扰，报告与日志中说明 | 可用 |

### 4.6 可观测性

- 事件：`tool-run`（4.2）；`tool-environment`（来源、耗时、与编辑器环境不同的变量名）；`model-source`（使用的来源与原因：cache-fresh、cache-stale、producer、inferred、needs-download、producer-timeout）。
- 报告的 `roots[].project` 增加：当前模型来源与年龄、缓存指纹是否一致、最近 20 次外部运行、工具环境摘要、`buildTool` 设置。
- 日志：外部程序超过软期限时记一行，结束时记一行（含 `outputHeldOpen`）。

## 5. 跨平台

| 方面 | Linux | macOS | Windows |
|---|---|---|---|
| 工具环境 | 登录 shell 解析 | 登录 shell 解析 | 编辑器进程环境（注册表） |
| 结束后代 | 进程组，先 `SIGTERM` 后 `SIGKILL`；`setsid` 脱离进程组的只能靠有界读取摆脱 | 同 Linux | Job 对象，后代默认在 Job 内 |
| 后代持有管道 | 有界读取 | 有界读取 | 有界读取（先验证 `kal_timeout_read` 对管道的支持） |
| 描述符继承 | openkal-linux 管道带 `O_CLOEXEC`；目录描述符被继承（向 openkal 记录） | 同 Linux（待核对 openkal-macos） | 放入标准流时继承全部可继承句柄（20f4410 记录的行为） |
| mcpp 离线 | `MCPP_OFFLINE=1` | 同 | 同 |

## 6. 性能

| 场景 | 当前 | 目标 |
|---|---|---|
| 有缓存时，从启动到 clangd 拿到数据库 | 等 mcpp 与探测：xlings 3.3–8 s | 0.3 s 内（读缓存、规划、写数据库；xlings 规划实测 0.15 s） |
| 没有缓存 | 等生产方，最长 120 s，之后不带数据库 | 最长 10 s，之后用推断模型 |
| 构建工具挂起 | 语言服务失效 12 分钟 | 功能不受影响；60 s 后结束整个单元 |
| 登录 shell 解析 | — | 后台一次；本机 fish 实测 14 ms，期限 10 s |
| 保存 `mcpp.toml` 后重新读取 | 在线运行 mcpp | 离线运行（xlings 2.6 s），期间功能不变 |

## 7. 需要 mcpp 与 xlings 适配的点（issue 草稿，review 后再提）

mcppls 按第 4 节实施后，不依赖下列任何一项也能正常工作；它们让 mcpp 自身、以及其他使用 `emit build-database` 的工具更可靠。

**mcpp**

| 编号 | 问题 | 证据 | 建议 |
|---|---|---|---|
| M1 | 离线缺少依赖时只有通用错误码 `MCPP_BUILD_DATABASE_PLAN_FAILED`，调用方只能匹配消息文本 | `src/cli/cmd_build.cppm:375`、`src/pm/package_fetcher.cppm:1114-1119` | 专门的错误码（如 `MCPP_OFFLINE_DOWNLOAD_REQUIRED`），诊断中结构化列出缺少的包、版本、索引，以及用户应运行的命令 |
| M2 | 规划期间保存的原标准输出 `dup(1)` 不带 close-on-exec，经 `popen` 启动的程序（xlings）继承调用方的管道；mcpp 被结束后，这些程序仍持有管道 | `modules/platform/src/terminal.cppm:82`；现场 fd 3 | 用 `fcntl(1, F_DUPFD_CLOEXEC, 3)` 保存（Windows 用不可继承的句柄）；启动 xlings 时关闭无关描述符 |
| M3 | 调用 xlings 的网络操作（`update`、`install`）没有期限；挂起的连接让 `emit build-database` 与 `mcpp build` 一起无限期挂起 | `modules/platform/src/process.cppm:550-579`、`src/xlings/xlings.cppm:1952-1967`；现场 11.5 分钟 | 整体期限（如索引刷新 60 s），超时即按“刷新失败只警告”的既有语义继续用本地数据；xlings 放进独立进程组，mcpp 结束时一并结束 |
| M4 | 每次运行信封的 `effects` 从不含 `network`，即使这次刷新了索引 | `src/cli/cmd_build.cppm:427-428` | 实际发生联网时加入 `network` |
| M5 | `auto_refresh = false` 不能完全禁止隐式刷新：工具链自动安装前的刷新与自定义索引首次同步不受它约束 | `src/xlings/xlings.cppm:1988-2018`、`src/build/prepare.cppm:4644-4662` | 统一由 `auto_refresh` 约束，或在文档中写明只有 `--offline` 能完全禁止 |
| M6 | mcpp-index 的仓库与制品地址只有 GitHub 一套，`mirror = CN` 时索引刷新仍访问 GitHub | `src/config.cppm:55-60`；CHANGELOG 中 CN 索引地址记为待部署 | 为 mcpp-index 提供 CN 地址，刷新时按镜像选择 |

**xlings**

| 编号 | 问题 | 证据 | 建议 |
|---|---|---|---|
| X1 | `xlings update` 的网络连接没有超时：一条 IPv6 连接发送队列停在 85 字节，挂了约 11.5 分钟 | 现场 `ss -tnp` | 连接与读取超时；IPv6 不通时回退 IPv4 |
| X2 | 索引仓库 `xim-pkgindex-awesome`、`d2x`、`scode` 登记为 GitHub 地址；`mirror = CN` 时是否改写需要 xlings 确认 | `~/.mcpp/registry/data/xim-index-repos/xim-indexrepos.json` | 镜像覆盖索引仓库 |

**openkal**（记录）：openkal-linux 为 `execveat` 保留的目录描述符被子进程继承（1.4 第 3 行）。

## 8. 实施与验证

| 编号 | 工作 | 验证 |
|---|---|---|
| T1 | 运行器（4.2）：独立单元、有界读取、软硬期限、`outputHeldOpen`、输出上限、`tool-run` 事件；生产方、探测、git、校验改用它 | 单元测试：孙进程持有标准错误时 1 s 左右返回并结束单元（现为 30 s）；期限结束整个进程组；输出上限 |
| T2 | 完整模型缓存与输入指纹；按来源分文件；启动路径（4.1）；不再不带数据库启动；不降级 | 单元测试：指纹一致立即规划；不一致等 3 s；推断结果不覆盖 mcpp 缓存。一致性夹具 `mcpp-emit-hang`（模拟 mcpp 挂起，并启动持有管道的后代）：悬停 5 s 内由 clangd 应答，无重启，报告显示 `producer-timeout` 与 `outputHeldOpen` |
| T3 | Windows：`kal_timeout_read` 对管道的支持；结束 Job 时结束后代 | Wine 下单元测试；windows-2022 一致性夹具 |
| T4 | 工具环境（4.3）：`print-environment`、登录 shell 解析、合并规则、失败退路、`mcppls.toolEnvironment` | 单元测试：合并规则。集成测试：以 `env -i` 启动服务端，`SHELL` 指向会设置 PATH 与变量的测试 shell，生产方收到这些值；测试 shell 挂起时 10 s 后退回编辑器环境 |
| T5 | 网络（4.4）：`MCPP_OFFLINE=1`、CMake `FETCHCONTENT_UPDATES_DISCONNECTED`、S2 请求 `network` 字段、“需要下载”状态与动作、焦点重试、`mcppls.buildTool` | 模拟 mcpp 记录收到的环境，未离线时挂起：默认设置下夹具通过；`online` 下收到工具环境；模拟离线缺包时状态带动作 |
| T6 | 状态与报告（4.5、4.6） | 报告检查：模型来源、最近外部运行、环境差异只有变量名 |
| T7 | S2-6-3 措辞、S2 请求 `network` 字段、traceability、CHANGELOG | `validate.py` |
| T8 | 在 xlings 上复现并验证：VS Code 环境（无代理）与有代理两种情形，冷缓存与温缓存 | 第 6 节表格实测 |

## 9. 决策

| 编号 | 决策 | 状态 |
|---|---|---|
| 1 | 默认 `mcppls.buildTool = offline`：隐式运行不联网，需要下载时给动作 | 已确认 |
| 2 | POSIX 默认解析登录 shell 环境（与 VS Code 自己的做法一致） | 已确认 |
| 3 | clangd 继续用编辑器进程环境（它只读归一化后的命令） | 已确认 |
| 4 | 有缓存时立即使用、后台确认。代价：编辑器关闭期间工程改动过时，启动后最初几秒用旧描述，确认后可能重启一次 clangd | 已确认 |
| 5 | 期限：生产方软 5 s、硬 60 s，工具链探测 20 s，登录 shell 10 s，无缓存时等生产方 10 s | 已确认 |
| 6 | 第 7 节的外部 issue：mcpp 的 M1–M6 合为一个适配 issue；xlings 的 X1、X2 | mcpp：提交中；xlings：待定 |
| 7 | CMake 私有构建目录的第一次配置允许下载（否则使用 FetchContent 的工程在离线默认下没有构建描述）；或同样离线、失败时给动作 | 待定 |
