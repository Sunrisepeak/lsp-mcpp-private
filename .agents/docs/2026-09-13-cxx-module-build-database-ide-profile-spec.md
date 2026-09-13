# C++ Build Database IDE Profile 规范草案 v0.2

日期：2026-09-13（v0.2 于 2026-09-14 按 review 调整定位）
状态：**草案，供 review**。名称为工作名。
关联文档：
- 设计方案：[2026-09-13-cxx-modules-unified-lsp-design.md](2026-09-13-cxx-modules-unified-lsp-design.md)
- 调研综述：[2026-09-13-cxx-modules-landscape-research.md](2026-09-13-cxx-modules-landscape-research.md)
- 本地实测：[2026-09-13-cxx-modules-lsp-experiments.md](2026-09-13-cxx-modules-lsp-experiments.md)

> 本文是规范体系中的 S1（统一模块描述）与 S2（发现协议，第 12 节）。
> S3（LSP 模块扩展）与 S4（语义工具包约定）暂放在设计方案第 8、9 节，实现仓库初始化后统一迁入仓库根目录的 `specs/`。

---

## 0. 摘要

本规范**自包含地**定义一种描述 C++ 工程模块信息的 JSON 文档，使语言服务器、索引器、重构工具可以**不依赖构建所用编译器**地提供 C++ named modules 语义功能。

它参考并兼容现有行业做法：

- 文档结构与字段和 WG21 P2977R2“构建数据库文件”兼容，一份合规文档同时是合法的 P2977R2 文档，CMake 输出的构建数据库可以直接作为低等级输入。
- 依赖边沿用 P1689R5 的概念，外部与标准库模块沿用 P3286 / EcoStd RFC #3 的元数据格式，包描述交给 CPS。
- 可以无损降级导出为 `compile_commands.json`，供尚不支持本规范的工具使用。

P2977 目前只是 SG15 提案，没有进入任何标准，也没有正在推进的标准化载体（见调研文档第 4 节）。
因此本规范写全所有字段与语义，**不依赖 P2977 的标准化进度**；两者将来出现冲突时，由本规范的 MAJOR 版本处理。

设计原则：

1. **只描述源码、模块图与语义选项，不描述 BMI 的内容。** BMI 路径只作为构建事实记录，消费方不得依赖它做语义分析。
2. **复用而不是替代。** 能沿用的行业格式与字段名一律沿用，只补 IDE 必需而现有格式缺失的部分。
3. **隐式输入显式化。** 工具链身份、target、sysroot、标准库及其模块清单必须显式给出。
4. **可渐进采纳。** 通过一致性等级，允许构建工具从“只有模块图”逐步升级到“可实时更新”。

---

## 1. 范围

### 1.1 覆盖

- 模块单元：主接口单元、分区接口单元、分区实现单元、模块实现单元。
- 导入模块的非模块翻译单元。
- 标准库模块（`std`、`std.compat`）与预构建库模块，通过模块元数据文件描述。
- 工具链身份与标准库选择。
- 结构化语义选项。
- set 之间的可见性。
- 发现与变更通知。

### 1.2 不覆盖

- Header units：保留 `role` 取值，语义待后续版本定义。
- Clang header modules（module map）。
- BMI 二进制格式。
- 链接信息与打包描述（交给 CPS）。

---

## 2. 约定

- 关键字 **MUST / MUST NOT / SHOULD / SHOULD NOT / MAY** 按 RFC 2119 与 RFC 8174 解释。
- JSON 按 RFC 8259。
- 路径解析：
  - 翻译单元对象内的相对路径，相对该单元的 `work-directory` 解析（与 P2977 一致）。
  - 文档级与 set 级 `ide` 对象内的相对路径，相对数据库文件所在目录解析。
- 本文所有新增字段都位于名为 `ide` 的对象中，不会与 P2977 未来新增的顶层字段冲突。

---

## 3. 与现有规范的关系

| 规范 | 关系 |
|---|---|
| P2977R2 构建数据库 | **兼容目标。** 合规文档同时是合法的 P2977R2 文档；P2977 字段在本规范中重新完整定义，不要求读者查阅 P2977。 |
| CMake 构建数据库输出（3.31+ 实验特性） | 可直接作为等级 1 输入；消费方按第 11 节补全缺失信息。 |
| JSON Compilation Database（`compile_commands.json`） | 下游兼容格式；工具 SHOULD 能把合规文档导出为它，见 12.3。 |
| P1689R5 依赖格式 | 生产方可以由 P1689 扫描结果填充 `provides`/`requires`；消费方可以在一致性等级不足时自行扫描补全。 |
| P3286R0 / EcoStd RFC #3 模块元数据 | `module-metadata` 字段引用该格式的文件；标准库模块必须通过它被发现。 |
| CPS 0.15 | CPS 包中 `cpp_module_metadata` 指向的文件，可直接作为 `module-metadata` 条目。 |
| P2717 / EcoStd RFC #2 工具自描述 | 工具链描述应当优先由工具自描述能力填充。 |
| P3335 结构化核心选项 | `options` 词汇表以向其收敛为目标。 |
| LSP 3.18 | 不修改 LSP；IDE 侧扩展见设计文档第 8 节。 |

---

## 4. 数据模型总览

```
Database                                     （P2977 顶层）
 ├─ version, revision                        （P2977）
 ├─ ide                                      （本 Profile）
 │   ├─ profile-version
 │   ├─ generator
 │   ├─ toolchains { <id>: Toolchain }
 │   └─ extensions
 └─ sets[]                                   （P2977）
     ├─ name, family-name, visible-sets[], baseline-arguments[]   （P2977）
     ├─ ide
     │   ├─ toolchain        -> Toolchain id
     │   ├─ configuration, kind
     │   ├─ options          -> SemanticOptions
     │   ├─ module-metadata[]
     │   └─ extensions
     └─ translation-units[]                  （P2977）
         ├─ source, work-directory, arguments[], local-arguments[],
         │  object, private, provides{}, requires[]               （P2977）
         └─ ide
             ├─ role
             ├─ options      -> SemanticOptions（相对 set 的增量）
             └─ extensions
```

---

## 5. 文档级 `ide` 对象

| 字段 | 类型 | 要求 | 说明 |
|---|---|---|---|
| `profile-version` | string | MUST | 本 Profile 的语义化版本，例如 `"0.1.0"` |
| `generator` | object | SHOULD | 生产方信息：`name`、`version` |
| `toolchains` | object | MUST | 工具链 id 到 Toolchain 对象的映射 |
| `extensions` | object | MAY | 厂商扩展，见第 13 节 |

---

## 6. Toolchain 对象

Toolchain 描述**构建**所用的编译器。它决定 `arguments` 的方言，也决定 IDE 应当模拟的标准库与目标平台。

| 字段 | 类型 | 要求 | 说明 |
|---|---|---|---|
| `family` | enum | MUST | `gcc`、`clang`、`msvc`、`clang-cl`、`other`；决定 `arguments` 的解析方言 |
| `version` | string | MUST | 编译器报告的版本 |
| `build-id` | string | MAY | 编译器构建修订，例如 clang 的 commit 哈希；BMI 兼容性以此为准，版本号相同不代表 BMI 兼容 |
| `driver` | string | MUST | 构建使用的驱动程序绝对路径 |
| `target` | string | MUST | 目标三元组，取自 `-dumpmachine`、`-print-target-triple` 或等价查询 |
| `sysroot` | string | MAY | 构建使用的 sysroot |
| `stdlib` | object | 条件 MUST | 任一单元导入 `std` 或 `std.compat` 时必须提供，见 6.1 |
| `config-files` | string[] | SHOULD | 生产方已知的隐式配置输入，例如 clang 驱动旁的 `.cfg`、GCC specs |
| `introspection` | object[] | MAY | 获取上述字段时执行的查询命令与输出摘要，便于复现 |

### 6.1 `stdlib` 对象

| 字段 | 类型 | 要求 | 说明 |
|---|---|---|---|
| `name` | enum | MUST | `libstdc++`、`libc++`、`msvc-stl`、`other` |
| `version` | string | SHOULD | 标准库版本 |
| `module-metadata` | string | MUST | P3286 形状的标准库模块清单路径，例如 `libstdc++.modules.json` |

> 设计理由：本地实测发现，某些 clang 发行包在驱动旁放置默认配置文件，会在用户不知情时把标准库从 libstdc++ 切换为 libc++。
> 若 IDE 不知道构建实际使用的标准库，就会在“没有任何报错”的情况下给出错误语义。因此标准库必须显式出现。

---

## 7. Set 级 `ide` 对象

P2977 中的 set 大致对应“一个 target 在一个配置下的全部翻译单元”。

| 字段 | 类型 | 要求 | 说明 |
|---|---|---|---|
| `toolchain` | string | MUST | 指向文档级 `toolchains` 中的 id |
| `configuration` | string | SHOULD | 配置名，例如 `debug`、`release`、`dev` |
| `kind` | enum | SHOULD | `library`、`executable`、`test`、`other`，用于 IDE 选择默认上下文 |
| `options` | SemanticOptions | 等级 3 MUST | `baseline-arguments` 的结构化形式 |
| `module-metadata` | string[] | MAY | 该 set 可见的外部模块元数据文件（不含工具链标准库） |
| `extensions` | object | MAY | 厂商扩展 |

---

## 8. 翻译单元级 `ide` 对象

| 字段 | 类型 | 要求 | 说明 |
|---|---|---|---|
| `role` | enum | 等级 2 MUST | 见 8.1 |
| `options` | SemanticOptions | MAY | 相对 set `options` 的增量，对应 `local-arguments` |
| `extensions` | object | MAY | 厂商扩展 |

### 8.1 `role` 取值

| 取值 | 对应源码形态 | 是否产生可导入接口 |
|---|---|---|
| `module-interface` | `export module M;` | 是 |
| `module-partition-interface` | `export module M:P;` | 是 |
| `module-partition-implementation` | `module M:P;` | 是（仅限模块 M 内部导入） |
| `module-implementation` | `module M;` | 否 |
| `non-module` | 无模块声明，可以包含 `import` | 否 |
| `unknown` | 生产方无法确定，例如模块声明位于条件编译块中 | 由消费方判定 |
| `header-unit` | 保留 | 保留 |

生产方 **MUST** 依据源码内容而不是文件扩展名确定 `role`。
若生产方无法确定，**MUST** 使用 `unknown` 而不是猜测；消费方遇到 `unknown` 时 **SHOULD** 自行扫描源码判定，并按第 11 节标明降级。

---

## 9. SemanticOptions 对象

SemanticOptions 以与编译器方言无关的方式，描述影响解析、语义与 BMI 兼容性的选项。
不影响语义的选项（优化级别、调试信息、输出路径、依赖文件生成、多数警告）**SHOULD NOT** 出现在这里。

| 字段 | 类型 | 说明 | GCC / Clang 来源 | MSVC 来源 |
|---|---|---|---|---|
| `language-standard` | string | `c++20`、`c++23`、`c++26` | `-std=` | `/std:` |
| `language-extensions` | enum | `none`、`gnu`、`ms` | `-std=gnu++XX` | `/permissive` 等 |
| `macros` | object[] | 有序序列，元素为 `{"define": "N", "value": "V"}` 或 `{"undefine": "N"}`；`value` 为 `null` 表示无值定义 | `-D`、`-U` | `/D`、`/U` |
| `include-directories` | object | `user`、`quote`、`system`、`after` 四个有序数组 | `-I`、`-iquote`、`-isystem`、`-idirafter` | `/I`、`/external:I` |
| `forced-includes` | string[] | 强制包含的头文件 | `-include` | `/FI` |
| `exceptions` | bool | 是否启用异常 | `-fno-exceptions` | `/EH` |
| `rtti` | bool | 是否启用 RTTI | `-fno-rtti` | `/GR-` |
| `raw-semantic-arguments` | object | 以 `family` 为键的原始参数数组，承载无法结构化但影响语义的参数，例如 `-fchar8_t`、`/Zc:` 系列 | — | — |

规则：

1. 消费方在 `options` 存在时 **MUST** 以其为准；不存在时 **MUST** 按 `toolchain.family` 解析 `arguments`。
2. 翻译单元的有效选项为 set `options` 与单元 `options` 的合并：对象按键递归合并；数组按“set 在前、单元在后”拼接；标量以单元为准。
3. 生产方 **MUST NOT** 在 `options` 中放入 BMI 定位参数（mapper、`-fmodule-file=`、`/reference` 等）。

---

## 10. 模块名解析语义

对 set `S` 中的翻译单元 `T`，其 `requires` 中的模块名 `N` 按以下顺序解析：

1. `S` 内 `provides` 含 `N` 的翻译单元。
2. `S.visible-sets` 列出的 set 中，`private` 为 `false` 且 `provides` 含 `N` 的翻译单元。
3. `S.ide.module-metadata` 列出的元数据文件中 `logical-name` 为 `N` 的模块。
4. `S.ide.toolchain` 对应工具链 `stdlib.module-metadata` 中 `logical-name` 为 `N` 的模块。

约束：

- 生产方 **MUST** 在 `visible-sets` 中列出完整的可见闭包；消费方 **MUST NOT** 自行做传递推导。
- 同一优先级出现多个提供者时，消费方 **MUST** 报告歧义诊断，**MAY** 按文档顺序选取第一个继续工作。
- 所有步骤均未命中时，消费方 **MUST** 在对应 `import` 声明处报告“模块无法解析”诊断。
- 分区名 `M:P` 只能被模块 `M` 的单元导入；消费方 **SHOULD** 诊断跨模块导入分区的情况。
- 元数据文件中的模块单元，其编译选项为：发起导入的 set 的有效选项，加上元数据条目的 `local-arguments`。

---

## 11. 一致性等级

### 11.1 生产方

| 等级 | 名称 | 要求 |
|---|---|---|
| 1 | Graph | 合法的 P2977R2 文档；所有提供或依赖 named module 的单元都带 `provides`/`requires` |
| 2 | IDE | 等级 1；并提供 `ide.profile-version`、`ide.toolchains`、每个 set 的 `ide.toolchain`、每个单元的 `ide.role`（允许 `unknown`）；标准库模块可按第 10 节解析；**工程内全部翻译单元**都在文档中，使其可替代 `compile_commands.json` |
| 3 | Structured | 等级 2；每个 set 提供 `ide.options`，局部参数不同的单元提供 `ide.options` 增量 |
| 4 | Live | 等级 3；提供第 12 节的发现命令，并在构建描述变化后原子地重写数据库 |

### 11.2 消费方

- **MUST** 忽略未知字段与未知扩展。
- **MUST NOT** 要求 `provides` 中的 BMI 路径存在。
- **MUST NOT** 使用构建 BMI 做语义分析，除非同时满足：消费方语义引擎与 `toolchain` 的 `family`、`version`、`build-id` 完全一致；用户显式开启“权威模式”；消费方能检测 BMI 相对源码是否陈旧。
- **MUST** 按 `toolchain.family` 的方言解释 `arguments`。
- **MUST** 按第 10 节解析模块并报告无法解析与歧义。
- **SHOULD** 接受等级 1 文档，通过词法扫描推断 `role`、通过驱动查询补全工具链，并向用户标明处于降级模式。

---

## 12. 发现与变更

### 12.1 静态发现

消费方按以下顺序寻找数据库：

1. 用户配置中显式指定的路径。
2. 发现命令（12.2）返回的路径。
3. 工作区内已知构建目录下的 `build_database.json`（例如 CMake 输出）。

生产方 **SHOULD** 以“写临时文件再重命名”的方式原子更新数据库。

### 12.2 发现命令协议

借鉴 rust-analyzer 的 discover 命令与 Go 的 `GOPACKAGESDRIVER`，采用“子进程 + stdio JSON”形式：

- 消费方启动用户配置的命令，并向 stdin 写入一个 JSON 请求：

```json
{ "workspace": "/abs/path/to/workspace",
  "files": ["/abs/path/to/opened/file.cppm"],
  "configuration": "debug",
  "profile-version": "0.1.0" }
```

- 生产方向 stdout 逐行输出 JSON（JSONL），最后一行必须是 `finished` 或 `error`：

```text
{"kind": "progress", "message": "scanning module sources"}
{"kind": "finished", "database": "/abs/path/target/build_database.json", "watch": ["mcpp.toml", "mcpp.lock", "src/**/*.cppm"]}
```

- 消费方 **MUST** 监视 `database` 文件与 `watch` 列出的路径，任一变化时重新执行发现命令或重新加载数据库。
- 发现命令 **MUST NOT** 要求完整构建；它只需完成配置与模块扫描。

### 12.3 向 `compile_commands.json` 的兼容导出

为了让尚不支持本规范的工具继续工作，生产方或转换工具 **SHOULD** 提供导出能力：

- 每个翻译单元导出为一个条目：`directory` 取 `work-directory`，`file` 取 `source`，`arguments` 取 `arguments`，`output` 取 `object`。
- 同一源文件出现在多个 set 中时，导出方 **MUST** 让调用者选择 set，或只导出默认上下文中的条目。
- 导出结果会丢失模块图、工具链与角色信息，导出方 **SHOULD** 在文档或日志中提示这一点。

---

## 13. 版本与扩展

- P2977 的 `version` 与 `revision` 保持 P2977 定义。
- `ide.profile-version` 采用语义化版本：MAJOR 表示不兼容变更，MINOR 表示新增字段，PATCH 表示澄清。
- 厂商扩展放在各层 `ide.extensions` 中，键使用反向域名或注册短名，例如 `"io.github.mcpp-community"`。
- 进入标准流程后，经采纳的 `ide` 字段应推动并入 P2977 本体（或其 EcoStd 移植版），届时本 Profile 发布 MAJOR 版本对齐。

---

## 14. 安全考虑

- 工具链查询与发现命令都会执行外部程序。消费方 **MUST** 只在工作区受信任时执行，并 **SHOULD** 提供类似 clangd `--query-driver` 的驱动白名单。
- 标准库模块源码通常位于工作区之外，消费方 **MUST** 以只读方式访问。
- 数据库中的路径可能指向任意位置，消费方 **MUST NOT** 据此写入文件。

---

## 15. 完整示例

以下是 mcpp 示例工程（GCC 16，模块 `hello.greet` 带分区 `:detail`，入口 `main.cpp`）在等级 3 下的输出。路径已缩写。

```json
{
  "version": 1,
  "revision": 0,
  "ide": {
    "profile-version": "0.1.0",
    "generator": { "name": "mcpp", "version": "2026.9.13.1" },
    "toolchains": {
      "gcc-16.1.0-x86_64-linux-gnu": {
        "family": "gcc",
        "version": "16.1.0",
        "driver": "/opt/xpkgs/gcc/16.1.0/bin/g++",
        "target": "x86_64-linux-gnu",
        "sysroot": "/opt/subos/default",
        "stdlib": {
          "name": "libstdc++",
          "version": "16.1.0",
          "module-metadata": "/opt/xpkgs/gcc/16.1.0/lib64/libstdc++.modules.json"
        },
        "config-files": []
      }
    }
  },
  "sets": [
    {
      "name": "hello@dev",
      "family-name": "hello",
      "visible-sets": [],
      "baseline-arguments": ["-std=c++23", "-fmodules", "-O0", "-g"],
      "ide": {
        "toolchain": "gcc-16.1.0-x86_64-linux-gnu",
        "configuration": "dev",
        "kind": "executable",
        "options": {
          "language-standard": "c++23",
          "language-extensions": "none",
          "macros": [],
          "include-directories": { "user": [], "quote": [], "system": [], "after": [] },
          "forced-includes": [],
          "exceptions": true,
          "rtti": true
        }
      },
      "translation-units": [
        {
          "source": "src/greet/detail.cppm",
          "work-directory": "/home/u/hello",
          "arguments": ["/opt/xpkgs/gcc/16.1.0/bin/g++", "-std=c++23", "-fmodules", "-O0", "-g",
                        "-c", "src/greet/detail.cppm", "-o", "target/obj/detail.m.o"],
          "local-arguments": [],
          "object": "target/obj/detail.m.o",
          "private": false,
          "provides": { "hello.greet:detail": "target/gcm.cache/hello.greet-detail.gcm" },
          "requires": ["std"],
          "ide": { "role": "module-partition-interface" }
        },
        {
          "source": "src/greet/greet.cppm",
          "work-directory": "/home/u/hello",
          "arguments": ["/opt/xpkgs/gcc/16.1.0/bin/g++", "-std=c++23", "-fmodules", "-O0", "-g",
                        "-c", "src/greet/greet.cppm", "-o", "target/obj/greet.m.o"],
          "local-arguments": [],
          "object": "target/obj/greet.m.o",
          "private": false,
          "provides": { "hello.greet": "target/gcm.cache/hello.greet.gcm" },
          "requires": ["hello.greet:detail", "std"],
          "ide": { "role": "module-interface" }
        },
        {
          "source": "src/main.cpp",
          "work-directory": "/home/u/hello",
          "arguments": ["/opt/xpkgs/gcc/16.1.0/bin/g++", "-std=c++23", "-fmodules", "-O0", "-g",
                        "-c", "src/main.cpp", "-o", "target/obj/main.o"],
          "local-arguments": [],
          "object": "target/obj/main.o",
          "private": true,
          "provides": {},
          "requires": ["hello.greet", "std"],
          "ide": { "role": "non-module" }
        }
      ]
    }
  ]
}
```

按第 10 节，`main.cpp` 对 `std` 的依赖在 set 内与可见 set 中都没有提供者，
于是落到工具链 `stdlib.module-metadata`，解析为 `libstdc++.modules.json` 中的 `bits/std.cc`。

---

## 16. JSON Schema（节选）

完整 Schema 将放在仓库根目录的 `specs/` 中。以下为 `ide` 相关定义的节选。

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://example.invalid/cxx-build-database-ide-profile/0.1.0",
  "$defs": {
    "toolchain": {
      "type": "object",
      "required": ["family", "version", "driver", "target"],
      "properties": {
        "family": { "enum": ["gcc", "clang", "msvc", "clang-cl", "other"] },
        "version": { "type": "string" },
        "build-id": { "type": "string" },
        "driver": { "type": "string" },
        "target": { "type": "string" },
        "sysroot": { "type": "string" },
        "stdlib": {
          "type": "object",
          "required": ["name", "module-metadata"],
          "properties": {
            "name": { "enum": ["libstdc++", "libc++", "msvc-stl", "other"] },
            "version": { "type": "string" },
            "module-metadata": { "type": "string" }
          }
        },
        "config-files": { "type": "array", "items": { "type": "string" } }
      }
    },
    "role": {
      "enum": ["module-interface", "module-partition-interface", "module-partition-implementation",
               "module-implementation", "non-module", "unknown", "header-unit"]
    },
    "macro": {
      "oneOf": [
        { "type": "object", "required": ["define"],
          "properties": { "define": { "type": "string" }, "value": { "type": ["string", "null"] } } },
        { "type": "object", "required": ["undefine"],
          "properties": { "undefine": { "type": "string" } } }
      ]
    },
    "semanticOptions": {
      "type": "object",
      "properties": {
        "language-standard": { "type": "string" },
        "language-extensions": { "enum": ["none", "gnu", "ms"] },
        "macros": { "type": "array", "items": { "$ref": "#/$defs/macro" } },
        "include-directories": {
          "type": "object",
          "properties": {
            "user": { "type": "array", "items": { "type": "string" } },
            "quote": { "type": "array", "items": { "type": "string" } },
            "system": { "type": "array", "items": { "type": "string" } },
            "after": { "type": "array", "items": { "type": "string" } }
          }
        },
        "forced-includes": { "type": "array", "items": { "type": "string" } },
        "exceptions": { "type": "boolean" },
        "rtti": { "type": "boolean" },
        "raw-semantic-arguments": { "type": "object", "additionalProperties": { "type": "array", "items": { "type": "string" } } }
      }
    }
  }
}
```

---

## 17. 一致性测试套件（大纲）

规范与参考实现一起交付一个公开测试套件，结构参考 test262 与 LSP 的 conformance 实践。

| 用例 | 覆盖点 |
|---|---|
| F1 | 单模块 + `import std` |
| F2 | 分区接口 + 分区实现 + `export import :part` |
| F3 | 模块实现单元 `module M;` |
| F4 | 两个 set，通过 `visible-sets` 导入 |
| F5 | 两个私有 set 各自提供同名模块（多变体） |
| F6 | 外部预构建库，通过 P3286 元数据导入 |
| F7 | 条件编译包裹的 `import`，检验扫描精度与降级 |
| F8 | 仅 GCC 接受的写法，检验诊断来源标注 |
| F9 | 工具链隐式配置（驱动旁 `.cfg`），检验标准库显式化 |

每个用例包含：

1. 源码工程。
2. 按工具链（GCC 16、Clang 22/23、MSVC）生成的期望数据库。
3. 期望的模块解析结果（提供者、歧义、无法解析）。
4. 期望的 LSP 结果：诊断、跨模块跳转、悬停、补全、引用、模块名跳转、未保存修改传播。

首批用例可直接由实验文档中的探针脚本演化而来。

---

## 18. 待决问题

1. 提交 EcoStd 时的组织方式：与 P2977 作者合作，把 P2977 的移植与 `ide` 字段合并为一份 RFC，还是分成“构建数据库”和“IDE 字段”两份。
2. `options` 是否直接采用 P3335 的字段命名，还是先保持本草案词汇再对齐。
3. Header units 的建模方式：独立 `role`，还是按 P1689 的 `lookup-method` 扩展 `requires`。
4. 多配置生成器（例如 Ninja Multi-Config）下，一个数据库包含多个配置时的默认上下文选择规则。
5. 工程内自带的库是否也应通过 P3286 元数据暴露，还是只用 `visible-sets`。
6. 规范名称与提交渠道：EcoStd RFC、SG15 论文，或两者同时。
