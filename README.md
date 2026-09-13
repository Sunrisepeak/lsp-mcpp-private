# lsp-mcpp

编译器无关的 C++ 模块语言服务器。

C++20 把 named modules 写进了语言标准，但 BMI 格式、依赖扫描、`import std` 的来源在 GCC、Clang、MSVC 之间各不相同，
导致同一份模块代码换一个编译器，编辑器里的跳转、补全、诊断就可能全部失效。
lsp-mcpp 把任何构建（mcpp、CMake、`compile_commands.json`，甚至什么都没有）归一化成同一种模块描述，
驱动锁定版本的 clangd，并补上模块级功能，让 C++ 模块在任何编译器、任何平台上都“装上就能用”。

## 状态

v1 开发中。设计与决策记录见 [.agents/docs](.agents/docs)：

- [设计方案](.agents/docs/2026-09-13-cxx-modules-unified-lsp-design.md)
- [规范草案](.agents/docs/2026-09-13-cxx-module-build-database-ide-profile-spec.md)
- [调研综述](.agents/docs/2026-09-13-cxx-modules-landscape-research.md)
- [本地实测](.agents/docs/2026-09-13-cxx-modules-lsp-experiments.md)

## 许可

Apache-2.0
