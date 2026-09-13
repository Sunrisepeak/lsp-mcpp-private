# C++ Modules LSP 本地实测记录

日期：2026-09-13（2026-09-14 补充 E14–E16）
关联文档：
- 设计方案：[2026-09-13-cxx-modules-unified-lsp-design.md](2026-09-13-cxx-modules-unified-lsp-design.md)
- 调研综述：[2026-09-13-cxx-modules-landscape-research.md](2026-09-13-cxx-modules-landscape-research.md)
- 规范草案：[2026-09-13-cxx-module-build-database-ide-profile-spec.md](2026-09-13-cxx-module-build-database-ide-profile-spec.md)
- 原型脚本（一次性 spike，非产品代码）：[assets/2026-09-13-modules-lsp-spike/](assets/2026-09-13-modules-lsp-spike/)

---

## 0. 结论先行

1. **直接使用构建产出的 `compile_commands.json`，clangd 在 GCC 工程下完全不可用。** 9 项检查全部失败，clangd 22 与 23 结果相同。
2. **经过“归一化”后，同一个 GCC 16 工程在 clangd 中 9 项通过 8 项。** 跳转、悬停、补全、引用都正常，`std::println` 能跳进 GCC 16 自己的 libstdc++ 头文件。
3. **Clang 工程同样需要归一化。** 直接吃构建 PCM 时，接口修改不会生效（clangd 22），或者 PCM 因版本不同被整体拒绝（clangd 23）。
4. **唯一始终失败的是“在 `import hello.greet;` 的模块名上跳转”。** clangd 22 返回空，clangd 23 返回错误位置。这需要 lsp-mcpp 自己提供。
5. **clangd 的隐藏参数 `--use-dirty-headers` 让模块构建使用编辑器中未保存的内容。** 未保存的接口修改可以传播到导入方。
6. **clangd 23.1 的持久化模块缓存可用。** 同一工程冷启动约 3.1 秒，二次启动约 0.7 秒，日志显示复用 std。
7. **工具链隐式配置是隐藏输入。** clang 驱动旁的默认配置文件会悄悄把标准库切换为 libc++，归一化必须显式指定标准库、target 与配置文件策略。
8. **clangd 23.1 在模块无法解析时会挂起该文件的请求。** 22.1.8 没有这个问题。lsp-mcpp 需要在转发前保证可解析，并给引擎请求加超时。
9. **MinGW-w64 目标（Windows 上的 GCC）同样可行，但归一化规则不同。** clang 面向 MinGW 目标时忽略 GCC 安装目录参数，需要改用 sysroot 指向 MinGW 根目录；修正后 9 项通过 8 项（Linux 主机交叉目标）。
10. **没有编译器、没有构建系统也能获得完整模块语义。** clangd 23 加一个只含标准库头文件、std 模块源码与 C 库头文件的“语义工具包”，9 项通过 8 项；Linux 工具包压缩后约 3.7 MB。

---

## 1. 环境

| 组件 | 版本 | 来源 |
|---|---|---|
| OS | Linux 6.8，x86_64 | — |
| mcpp | 2026.9.13.1 | xlings |
| GCC | 16.1.0 | mcpp 工具链 `gcc@16.1.0` |
| Clang / clang-scan-deps | 22.1.8 | mcpp 工具链 `llvm@22.1.8` |
| MinGW-w64 GCC（交叉） | 16.1.0，`x86_64-w64-mingw32` | mcpp 目标 `x86_64-windows-gnu` |
| clangd | 22.1.8（commit ca7933e47d3a） | xlings `llvm-tools` |
| clangd | 23.1.0（commit ea7d852a70e8，2026-09-02 发布） | clangd GitHub release |
| CMake | 4.4.2 | xlings |
| Ninja | 1.12.1 | mcpp payload |

测试工程（`assets/.../fixture`）：

```
src/main.cpp            import std; import hello.greet;  调用 hello::greet 与 std::println
src/greet/greet.cppm    export module hello.greet; export import :detail; import std;
src/greet/detail.cppm   export module hello.greet:detail; import std;
tests/test_smoke.cpp    import std;
```

---

## 2. 复现方式

```bash
cd .agents/docs/assets/2026-09-13-modules-lsp-spike
CLANGD=/path/to/clangd-23 CLANG=/path/to/llvm-22/bin/clang++ PROBE_TIMEOUT=20 ./run.sh /tmp/lsp-mcpp-run
```

脚本对三种构建各执行一次：`gcc@16.1.0`、`llvm@22.1.8`、`x86_64-windows-gnu`（MinGW-w64 GCC 16）。
`PROBE_TIMEOUT` 限制每个 LSP 请求的等待时间；使用 clangd 23.1 时建议设小，否则 E13 的挂起会让失败配置等满超时。
2026-09-14 用 clangd 23.1.0、超时 20 秒完整运行一次耗时 251 秒。

每种构建的步骤：

1. 复制测试工程并用 mcpp 构建。
2. 用 `normalize_cdb.py` 生成归一化数据库。
3. 分别对“构建原始数据库”和“归一化数据库”运行 `lsp_probe.py`，参数为 `--experimental-modules-support --use-dirty-headers`。

探针检查项：

| 编号 | 检查内容 |
|---|---|
| C1 | `main.cpp` 首次诊断为空 |
| C2 | `hello::greet` 跳转到 `greet.cppm` |
| C3 | 悬停 `hello::greet` 显示提供它的模块单元 |
| C4 | `std::println` 跳转到工具链自己的标准库 |
| C5 | 在 `import hello.greet;` 的模块名上跳转到模块声明 |
| C6 | `hello::` 后补全出 `greet` |
| C7 | `greet.cppm` 中未保存的新增函数 `greet2` 对 `main.cpp` 可见 |
| C8 | 关闭缓冲区后，磁盘上保存的 `greet2` 对 `main.cpp` 可见 |
| C9 | `hello::greet` 的引用同时覆盖 `main.cpp` 与 `greet.cppm` |

---

## 3. 实验记录

### E1 编译数据库条目能否独立重放

做法：取 mcpp（GCC 16）生成的 `compile_commands.json` 中 `main.cpp` 的条目，在其 `directory` 下原样执行。

结果：

```text
std: error: failed to read compiled module: No such file or directory
std: note: compiled module file is 'gcm.cache/std.gcm'
```

结论：GCC 依赖 cwd 下的 `gcm.cache/`，而 mcpp 实际在 `target/<triple>/<fingerprint>/` 下执行构建。
**模块场景下，编译数据库条目不再是自包含的。** 这是 `compile_commands.json` 不足以描述模块工程的直接证据。

### E2 P1689 输出的实现差异

同一工程，mcpp 的 ninja dyndep 路径分别由 GCC 与 clang-scan-deps 产出 `.ddi`：

```jsonc
// GCC 16: -fdeps-format=p1689r5
{ "rules": [ { "primary-output": "obj/greet.m.o",
    "provides": [ { "logical-name": "hello.greet", "is-interface": true } ],
    "requires": [ { "logical-name": "hello.greet:detail" }, { "logical-name": "std" } ] } ],
  "version": 0, "revision": 0 }

// clang-scan-deps 22: -format=p1689
{ "revision": 0, "rules": [ { "primary-output": "obj/greet.m.o",
    "provides": [ { "is-interface": true, "logical-name": "hello.greet",
                    "source-path": "/abs/src/greet/greet.cppm" } ],
    "requires": [ { "logical-name": "hello.greet:detail" }, { "logical-name": "std" } ] } ],
  "version": 1 }
```

结论：同为 P1689R5，GCC 输出 `version: 0` 且缺少 `source-path`。消费方必须容忍字段差异。

### E3 标准库模块清单

```jsonc
// libc++ 22: clang++ -print-library-module-manifest-path
{ "version": 1, "revision": 1, "modules": [
  { "logical-name": "std", "source-path": "../../share/libc++/v1/std.cppm", "is-std-library": true,
    "local-arguments": { "system-include-directories": ["../../share/libc++/v1"] } },
  { "logical-name": "std.compat", "source-path": "../../share/libc++/v1/std.compat.cppm", "is-std-library": true,
    "local-arguments": { "system-include-directories": ["../../share/libc++/v1"] } } ] }

// libstdc++ 16: g++ -print-file-name=libstdc++.modules.json
{ "version": 1, "revision": 1, "modules": [
  { "logical-name": "std", "source-path": "../include/c++/16.1.0/bits/std.cc", "is-std-library": true },
  { "logical-name": "std.compat", "source-path": "../include/c++/16.1.0/bits/std.compat.cc", "is-std-library": true } ] }
```

结论：两家标准库已经按同一形状（P3286 / EcoStd RFC #3）发布清单，规范可以直接复用。

### E4 clangd 基线（clangd 22.1.8，`--check` 模式）

| 输入 | 模块参数 | 结果 |
|---|---|---|
| mcpp GCC 数据库 | 无 | `module 'std' not found` |
| mcpp GCC 数据库 | `--experimental-modules-support` | `Failed to build module std; due to Don't get the module unit for module std` |
| mcpp LLVM 数据库（含 `-fmodule-file=std=` 与 `-fprebuilt-module-path=`，构建已完成） | 无 | 0 错误 |
| CMake 4.4.2 + GCC 16 数据库 | 有或无 | `unknown argument: '-fmodules-ts'`、`'-fmodule-mapper=...modmap'`、`'-fdeps-format=p1689r5'`，随后 `module 'hello.greet' not found` |
| CMake 4.4.2 + Clang 22 数据库（`@...modmap` 响应文件，构建已完成） | 有或无 | 0 错误 |

结论：只有“构建编译器与 clangd 同为 Clang 且构建已完成”时，原始数据库才可用。

### E5 Clang 22 能否编译 libstdc++ 16 的 std 模块

```bash
clang++ -std=c++23 --no-default-config --gcc-install-dir=$GCC16/lib/gcc/x86_64-linux-gnu/16.1.0 \
  --sysroot=$SYSROOT -x c++-module $GCC16/include/c++/16.1.0/bits/std.cc --precompile -o std.pcm
```

结果：1.5 秒完成，产出 35 MB 的 `std.pcm`，仅有一条 `'std' is a reserved name for a module` 警告。

结论：对本测试工程，“Clang 前端 + GCC 标准库”的组合可行。调研中另有 Clang 与 GCC 15.2 libstdc++ 的互操作问题报告（clangd#2610），需要纳入一致性测试持续跟踪。

### E6 LSP 探针矩阵

clangd 22.1.8：

| 工具链 | 数据库 | C1 | C2 | C3 | C4 | C5 | C6 | C7 | C8 | C9 |
|---|---|---|---|---|---|---|---|---|---|---|
| gcc@16.1.0 | 构建原始 | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| gcc@16.1.0 | 归一化 | ✓ 2.0s | ✓ | ✓ | ✓ libstdc++ 16 | ✗ | ✓ | ✓ | ✓ | ✓ |
| llvm@22.1.8 | 构建原始 | ✓ 0.2s | ✓ | ✓ | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ |
| llvm@22.1.8 | 归一化 | ✓ 1.9s | ✓ | ✓ | ✓ libc++ 22 | ✗ | ✓ | ✓ | ✓ | ✓ |

clangd 23.1.0：

| 工具链 | 数据库 | C1 | C2 | C3 | C4 | C5 | C6 | C7 | C8 | C9 |
|---|---|---|---|---|---|---|---|---|---|---|
| gcc@16.1.0 | 构建原始 | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| gcc@16.1.0 | 归一化 | ✓ 2.5s | ✓ | ✓ | ✓ libstdc++ 16 | ✗ 错误位置 | ✓ | ✓ | ✓ | ✓ |
| llvm@22.1.8 | 构建原始 | ✗ PCM 格式被拒 | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| llvm@22.1.8 | 归一化 | ✓ 3.5s | ✓ | ✓ | ✓ libc++ 22 | ✗ 错误位置 | ✓ | ✓ | ✓ | ✓ |
| x86_64-windows-gnu | 构建原始 | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| x86_64-windows-gnu | 归一化（E14 规则） | ✓ 2.3s | ✓ | ✓ | ✓ MinGW libstdc++ 16 | ✗ 错误位置 | ✓ | ✓ | ✓ | ✓ |
| 无编译器、无构建系统 | 推断（E15） | ✓ 2.1s | ✓ | ✓ | ✓ 工具包 libc++ | ✗ 错误位置 | ✓ | ✓ | ✓ | ✓ |
| 无编译器，Windows 语义 | 推断（E16，libc++ 23.1 + MinGW-w64） | ✓ 2.1s | ✓ | ✓ | ✓ 工具包 libc++ | ✗ 错误位置 | ✓ | ✓ | ✓ | ✓ |

说明：

- 首次诊断耗时包含 clangd 从源码构建 std 模块的时间。
- clangd 23 读取 Clang 22 构建的 PCM 时报错：`Module file '.../pcm.cache/std.pcm' uses an older format that is no longer supported`。
- C5 在 clangd 23 上返回 `detail.cppm:3`，即 `export namespace hello::detail {` 所在行，把模块名中的 `hello` 当成了命名空间。
- `hello::` 补全在 clangd 自建 `hello.greet` 的 BMI 时会额外给出 `detail`。早期一次不带 `--experimental-modules-support`、完全读取构建 PCM（Clang 22 默认 Reduced BMI）的运行中没有 `detail`。原因未深究，记录备查。

### E7 未保存修改的传播

同一归一化工程，clangd 22.1.8，只改变是否带 `--use-dirty-headers`：

| 参数 | C7（未保存） | C8（已保存） |
|---|---|---|
| 仅 `--experimental-modules-support` | ✗ | ✓ |
| 再加 `--use-dirty-headers` | ✓ | ✓ |

结论：clangd 的模块构建器默认从磁盘读源码，该隐藏参数可以让它使用编辑器缓冲区。
clangd 23.1.0 带该参数时 C7 同样通过。该参数属于隐藏选项，稳定性与大工程开销需要在 Phase 1 评估。

### E8 构建 BMI 的陈旧问题

mcpp LLVM 工程直接使用构建数据库（其中含 `-fprebuilt-module-path=<构建 pcm 目录>`）：

- clangd 22.1.8：C8 失败。磁盘上的接口已修改，但 clangd 仍然使用构建目录中的旧 PCM，直到重新构建。
- clangd 22.1.8 加 `--experimental-modules-support`：日志报 std 找不到源单元，并对 `-fmodule-file=` 等参数给出 “argument unused”；结果同样陈旧。
- clangd 23.1.0：PCM 版本不兼容，整体失败。

结论：**IDE 读取构建 BMI 会同时遭遇“陈旧”与“版本锁定”两个问题**，规范要求消费方默认不使用构建 BMI 是必要的。

### E9 预构建 std BMI 与 clangd 自建 BMI 能否混用（clangd 22.1.8）

做法：归一化数据库中去掉 std 源单元，改为给每个条目加 `-fmodule-file=std=<同版本 clang 构建的 std.pcm>`。

结果：`Failed to build module std; due to Don't get the module unit for module std`，随后 `module 'hello.greet' not found`。

结论：clangd 22 的模块构建器要求每个被导入模块都有源单元，缺一个就放弃全部前置模块，无法混用。
clangd 23 源码中新增了读取 `-fmodule-file=` 的后端，但本地缺少 Clang 23 编译器来生成兼容 PCM，**23.x 下未验证**。
由于 E12 的持久化缓存已经解决冷启动问题，这一项的优先级下降。

### E10 工具链隐式配置陷阱

现象：第一版通用归一化脚本只加了 `--gcc-install-dir` 与 `--sysroot`，GCC 工程在 clangd 中报：

```text
Scanning modules dependencies for .../bits/std.cc failed: fatal error: 'bits/stdc++.h' file not found
```

排查：mcpp 的 LLVM 发行包在 `clang++` 旁放置了 `clang++.cfg`，其中选择 libc++ 并加入 libc++ 头路径。
clang 驱动加载该配置后忽略了 `--gcc-install-dir`，`<string>` 等头文件从 libc++ 解析成功，只有 libstdc++ 内部头失败，**表面上几乎没有异常**。

修正后的 GCC → Clang 翻译参数：

```text
--no-default-config  --target=<g++ -dumpmachine>  -stdlib=libstdc++
--gcc-install-dir=<dirname(g++ -print-libgcc-file-name)>  --sysroot=<原值>
```

修正后 clang 的头文件搜索路径与 `g++ -v` 一致，仅编译器内建头换成 clang 自己的，这是预期行为。

结论：规范中的 `toolchain.stdlib`、`toolchain.target`、`toolchain.config-files` 必须显式存在；归一化器必须关闭驱动的隐式配置。

### E11 CMake 4.4.2 的 P2977 构建数据库

启用方式（实验门控 UUID 随 CMake 版本变化，4.4.2 为下值）：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE=70ef007e-b743-492d-9407-e35eeac03a40 \
  -DCMAKE_EXPORT_BUILD_DATABASE=ON
ninja -C build && ninja -C build cmake_build_database
```

GCC 16 下的输出（节选）：

```json
{ "version": 1, "revision": 0, "sets": [
  { "name": "greet@", "family-name": "greet", "visible-sets": [],
    "translation-units": [ {
      "source": "/abs/src/greet.cppm", "work-directory": "/abs/build",
      "arguments": ["g++", "-std=gnu++23", "-MD", "...", "-c", "/abs/src/greet.cppm"],
      "baseline-arguments": ["-std=gnu++23"], "local-arguments": ["-std=gnu++23"],
      "object": "CMakeFiles/greet.dir/src/greet.cppm.o", "private": false,
      "provides": { "hello.greet": "/abs/build/CMakeFiles/greet.dir/hello.greet.gcm" },
      "requires": ["hello.greet:detail"] } ] },
  { "name": "app@", "family-name": "app", "visible-sets": ["greet@"],
    "translation-units": [ { "source": "/abs/src/main.cpp", "private": true,
      "provides": {}, "requires": ["hello.greet"], "...": "..." } ] } ] }
```

对比同一构建的 `compile_commands.json`（GCC）：

```text
g++ -std=gnu++23 -fmodules-ts -fmodule-mapper=CMakeFiles/app.dir/src/main.cpp.o.modmap -MD -fdeps-format=p1689r5 -x c++ ...
```

以及 Clang 下的 `.modmap` 响应文件：

```text
-x c++-module
-fmodule-output="CMakeFiles/greet.dir/hello.greet.pcm"
-fmodule-file="hello.greet:detail=CMakeFiles/greet.dir/hello.greet-detail.pcm"
```

结论：

1. 构建数据库的 `arguments` 不含 BMI 定位参数，模块图（`provides`、`requires`、`visible-sets`）显式存在，比 `compile_commands.json` 更适合 IDE。
2. 它仍缺少工具链身份、标准库清单与单元角色，这正是 IDE Profile 要补的字段。
3. 该特性仍需实验门控 UUID，暂时不能作为用户默认路径，只能作为适配输入。

### E12 clangd 23.1 持久化模块缓存

对 GCC 归一化工程连续执行 `clangd --check`：

| 场景 | 前置模块准备耗时 | 总耗时 | 日志 |
|---|---|---|---|
| 缓存目录清空后（冷） | 2.88s | 3.07s | 构建 std、分区、主接口 |
| 第二次（温） | 0.62s | 0.69s | `Reusing persistent module std ...` 等三条复用记录 |

缓存布局：

```text
<编译数据库所在目录>/.cache/clangd/modules/
  std.cc-<源路径哈希>/<命令哈希>/std.pcm
  std.cc-<源路径哈希>/<命令哈希>/std-<时间戳>.pcm      版本化副本
  greet.cppm-<源路径哈希>/<命令哈希>/hello.greet.pcm
```

启动时会运行 GC，阈值默认 259200 秒，可用 `--modules-builder-versioned-gc-threshold-seconds` 调整。

结论：

1. 缓存根目录由编译数据库所在目录决定，lsp-mcpp 生成的引擎数据库放在哪里，缓存就在哪里，便于集中管理。
2. 缓存键包含完整命令，因此 std 模块应按“语义选项分组”注入，避免每种命令各建一份。
3. 跨工程共享 std 缓存目前不支持，列为上游改进项。

### E13 clangd 23.1 在模块无法解析时挂起请求

现象：clangd 22.1.8 下完整矩阵在 10 分钟内完成，clangd 23.1.0 下超过 10 分钟。
逐项计时显示，归一化配置 8 秒内跑完，时间全部耗在“构建原始数据库”这类失败配置的探针超时上。

定向复现（脚本 `assets/.../hang_probe.py`；GCC 原始数据库，`std` 没有源单元，参数为 `--experimental-modules-support --use-dirty-headers`）：

| 步骤 | clangd 22.1.8 | clangd 23.1.0 |
|---|---|---|
| 打开 `main.cpp`，请求补全 | 立即返回 | 立即返回 |
| 打开模块接口 `greet.cppm`，等待诊断 | 发布 1 条诊断 | **始终没有发布** |
| 对 `greet.cppm` 请求悬停 | 返回 | **30 秒内无响应** |

clangd 23 的日志停在为 `greet.cppm` 构建 preamble 之后，没有后续 AST 构建记录，进程 CPU 空闲。

结论：

1. clangd 23.1.0 中，**模块接口单元只要有一个导入无法解析，该文件的请求就可能永久挂起**。这是相对 22.1.8 的回归，尚未缩减为最小用例，也尚未向上游报告。
2. lsp-mcpp 不能假设引擎总会响应：需要在转发前用语法模块索引检查导入是否可解析，并为转发的请求设置超时与降级应答。
3. 这条结论提高了“归一化必须保证所有模块可解析”的优先级，也列入 Phase 1 的上游问题报告事项。

### E14 MinGW-w64 目标（Windows 上的 GCC）

做法：`mcpp build --target x86_64-windows-gnu` 在 Linux 主机上交叉构建测试工程，产出 `hello.exe`，再按 E6 的方式归一化并探测（clangd 23.1.0）。

第一次沿用 Linux 规则（`--gcc-install-dir`）失败：

```text
warning: argument unused during compilation: '--gcc-install-dir=.../lib/gcc/x86_64-w64-mingw32/16.1.0'
Failed to build module std; due to Failed to compile .../x86_64-w64-mingw32/include/c++/16.1.0/bits/std.cc
```

原因：clang 的 MinGW 驱动不使用 GCC 安装目录参数，而是通过 sysroot 定位 GCC 安装。修正后的翻译参数：

```text
--no-default-config  --target=x86_64-w64-mingw32  -stdlib=libstdc++
--sysroot=<MinGW 工具链根目录，即 g++ 所在 bin 的上一级>
```

修正后 clang 的头文件搜索路径指向 MinGW 自带的 libstdc++ 16，clang 22 预编译其 std 模块耗时 1.33 秒、产出 33.5 MB。探针结果见 E6 表中 `x86_64-windows-gnu` 行，9 项通过 8 项，`std::println` 跳转进入 MinGW 的 libstdc++。

结论：

1. 归一化规则必须按**目标平台**区分，而不仅是按编译器家族区分。这类知识应集中在归一化层，并尽量复用 mcpp 已有的工具链模型。
2. 本实验是 Linux 主机上的交叉目标。Windows 主机上的路径形式、大小写不敏感与盘符处理仍需复测。

### E15 没有编译器、没有构建系统时的“语义工具包”

问题：用户机器上没有任何编译器，工程也没有构建系统文件时，能否提供模块语义？

做法：

1. 只从现有发行包中复制**头文件与模块源码**，组成工具包，不含任何可执行文件：

| 目录 | 来源 | 内容 |
|---|---|---|
| `include/c++/v1`、`include/x86_64-unknown-linux-gnu/c++/v1` | LLVM 22.1.8 | libc++ 头文件与 `__config_site` |
| `share/libc++/v1` | LLVM 22.1.8 | `std.cppm`、`std.compat.cppm` 与 `std/*.inc` |
| `lib/x86_64-unknown-linux-gnu/libc++.modules.json` | LLVM 22.1.8 | 标准库模块清单 |
| `sysroot/usr/include` | glibc 2.44 与 Linux 5.11 头文件 | C 库与内核头 |

2. 删除测试工程的 `mcpp.toml`，用 `assets/.../infer_cdb.py` 仅凭源码内容推断单元角色，生成引擎数据库。数据库中的驱动路径故意指向不存在的文件。脚本从工具包根目录的 `kit.json` 读取 target、头文件目录与模块清单位置，格式见 E16。
3. 用 `env -i HOME=$HOME PATH=/nonexistent` 清空环境后运行 clangd 23.1.0 与探针。

结果：

| 项目 | 数值 |
|---|---|
| Linux 工具包文件数 | 3234 |
| Linux 工具包体积 | 28 MB，tar.gz 压缩后 3.7 MB |
| 首次诊断 | 2.1 秒 |
| 探针 | 9 项通过 8 项，C4 跳转进入工具包中的 libc++ |

同样方式验证了 Windows 语义所需的 MinGW-w64 纯头文件包：只用 `-nostdinc++ -nostdlibinc` 加显式包含目录，clang 能解析 `bits/stdc++.h`，并在 3.72 秒内预编译出 std 模块。

| 项目 | 数值 |
|---|---|
| MinGW-w64 工具包文件数 | 2537 |
| MinGW-w64 工具包体积 | 90 MB，tar.gz 压缩后 10.1 MB |

作为对比，clangd 23.1.0 自身的官方发行包：

| 平台 | 压缩包 | 解压后 |
|---|---|---|
| Linux | 118 MB | 225 MB，其中可执行文件 142 MB |
| macOS | 100 MB | 未测 |
| Windows | 30 MB | 未测 |

结论：

1. **语义功能不需要编译器可执行文件**，只需要 clangd、标准库头文件与 std 模块源码、C 库头文件。
2. 分发体积的大头是 clangd 本身，工具包很小。
3. 工具包内必须保留模块清单与源码的相对位置，因为清单用相对路径引用源码。
4. macOS 的 C 库头文件只存在于 macOS SDK 中，受 Apple 许可限制不能随工具包分发，本实验无法覆盖，需要用户已安装 Command Line Tools。
5. MSVC STL 依赖 Visual Studio 工具集与 Windows SDK 中的头文件，没有安装 Visual Studio 的 Windows 机器只能提供 MinGW 语义。

### E16 统一用 libc++ 的无编译器工具包，以及一体打包的体积

背景：review 决定“没有编译器时默认使用 libc++”，并且扩展一体打包。需要确认两件事：Windows 上 libc++ 工具包是否可行；clangd 加工具包打进扩展后体积是否可接受。

**Windows 语义的 libc++ 工具包。** llvm-mingw 20260826 对应 LLVM 23.1.0 正式版，与锁定的 clangd 版本一致。它的 Linux 主机发行包里已经包含：

| 内容 | 路径 |
|---|---|
| libc++ 头文件与 MinGW-w64 UCRT 头文件 | `generic-w64-mingw32/include`（`x86_64-w64-mingw32/include` 是指向它的符号链接） |
| libc++ std 模块源码 | `share/libc++/v1/std.cppm`、`std.compat.cppm` |
| MinGW 目标的模块清单 | `x86_64-w64-mingw32/lib/libc++.modules.json` |

只解出这些数据文件，加一个 `kit.json`（格式见设计文档的语义工具包约定），用 `assets/.../infer_cdb.py` 推断数据库，在清空环境变量的条件下运行 clangd 23.1.0。Linux 工具包用同一脚本重跑作为对照。

| 工具包 | 探针结果 | 体积 |
|---|---|---|
| Linux：libc++ 22.1.8 + glibc 与内核头 | 9 项通过 8 项，首次诊断 2.1 秒 | 28 MB，压缩后 3.7 MB |
| Windows 语义：libc++ 23.1.0 + MinGW-w64 UCRT 头 | 9 项通过 8 项，首次诊断 2.1 秒 | 113 MB，压缩后 12.1 MB；去掉 `.idl`、`.tlb`、`.def` 后 93 MB，压缩后 10.4 MB |

**clangd 负载裁剪。**

| 平台 | 官方包内容 | 裁剪方式 | 裁剪结果 |
|---|---|---|---|
| Linux x64 | 可执行文件 142 MB，未剥离符号；`lib/clang/23/lib` 下 67 MB 的 sanitizer 运行库；内建头文件 17 MB | 剥离符号，删除 sanitizer 运行库 | 76 MB，压缩后 22.8 MB；探针仍通过 8 项（实测） |
| Windows x64 | `clangd.exe` 49.5 MB；sanitizer 运行库 31.4 MB；内建头文件 15.5 MB | 删除 sanitizer 运行库 | 按官方压缩数据估算约 20.7 MB（未实测运行） |
| macOS | x86_64 与 arm64 通用二进制 168.9 MB；sanitizer 运行库 189.7 MB | 剥离符号，删除 sanitizer 运行库 | 通用二进制剥离后压缩约 41.8 MB；只取 arm64 预计约一半（未验证） |

其他事实：

- 官方 clangd 23.1.0 的 Linux 可执行文件动态链接系统 C 库，要求 glibc 2.18 以上。
- 官方只发布 Linux x64、Windows x64 与 macOS 通用二进制，没有 Linux 与 Windows 的 arm64 版本。

结论：

1. “无编译器默认 libc++”在 Linux 与 Windows 语义下都成立，macOS 按同一结构加 SDK 即可。三平台的无编译器语义统一为与 clangd 同版本的 libc++。
2. 一体打包后，Linux 扩展包的负载约 27 MB，Windows 约 31–33 MB。作为参照，微软 C/C++ 扩展的 Linux 包约 134 MB（见调研第 8 节）。
3. 裁剪只删除 clangd 用不到的文件，不改变 clangd 行为；裁剪步骤放进发布流水线并由一致性测试把关。

---

## 4. 对设计的直接结论

| 实验 | 设计决策 |
|---|---|
| E1、E4、E11 | IDE 不能以 `compile_commands.json` 作为模块工程的唯一输入，需要带模块图的统一描述 |
| E2、E3 | 依赖扫描层与标准库模块层复用 P1689 与 P3286，但消费方必须容错 |
| E5、E6 | 推荐“单一 Clang 语义引擎 + 归一化”路线，GCC 工程可以获得完整模块语义 |
| E6（C5） | 模块名导航、import 补全等模块级功能由 lsp-mcpp 的语法模块索引提供 |
| E7 | Phase 1 启用 `--use-dirty-headers`，并把“模块构建使用编辑器缓冲区”列入上游稳定化事项 |
| E8 | 规范规定消费方默认不得使用构建 BMI |
| E9、E12 | 锁定 clangd 23.x，依赖其持久化缓存；预构建 BMI 混用不作为 Phase 1 依赖 |
| E10 | 工具链描述必须显式包含 target、标准库与隐式配置；归一化器关闭驱动默认配置 |
| E13 | 引擎适配层为转发请求设置超时与降级应答；转发前用语法索引检查导入可解析性；向 clangd 上游报告回归 |
| E14 | 归一化规则按目标平台区分；MinGW 目标使用 sysroot 规则 |
| E15 | 零配置体验中“没有编译器”的场景由语义工具包解决；macOS 依赖 Command Line Tools，无 Visual Studio 的 Windows 使用 MinGW 运行时语义 |
| E16 | 无编译器语义统一为 libc++；clangd 负载裁剪后一体打包，扩展包体积在三十 MB 量级 |

## 5. 未验证事项

1. Windows 主机：MinGW 工程的路径与盘符处理；clang++ 面向 MSVC ABI 时 MSVC STL `std.ixx` 的构建；clang-cl 与 cl.exe 的 MSVC 风格命令翻译。均需要装有 Visual Studio 的 Windows 主机。
2. macOS 主机：LLVM clang 与 libc++ std 模块、macOS SDK 路径；未安装 Command Line Tools 时的体验。
3. 干净机器上的无编译器场景：Windows 与 macOS 需要不含编译器的虚拟机；Linux 已用清空环境变量的方式验证。
4. 大型工程性能：本测试工程只有 3 个模块单元，需要用 mcpp 自身源码（大量 C++23 模块）做基准。
5. 同名模块多变体、`#if` 包裹的 import、header unit：未覆盖，列入一致性测试用例 F5、F7。
6. clangd 23 下预构建 BMI 与自建 BMI 混用（E9）。
