# Packaging

Everything lsp-mcpp ships besides its own source: a **payload** per platform,
holding the server, a trimmed clangd 23.1 and the `lsp-mcpp-kit` semantic kit.
The VS Code extension carries a payload inside its platform VSIX; the xlings
packages carry the same parts as two archives (design §15 and §17).

```
packaging/
  payload.lock.json        every upstream input, with its sha256
  scripts/
    fetch.py               download and verify inputs from the lock
    trim_clangd.py         reduce an official clangd release to what is shipped
    build_kit.py           assemble the semantic kit for one platform (kits/README.md)
    assemble_payload.py    put the three parts into the payload layout, and verify one
    xlings_artifacts.py    split release payloads into xlings-res archives
  xlings/                  xpkg descriptor templates for lsp-mcpp and lsp-mcpp-kit
  kits/README.md           what each platform's kit contains and why
```

The scripts use the Python standard library only, so they run unchanged on every
CI runner.

## The payload layout

The server and the VS Code extension both rely on this layout; `payload.json` is
its manifest.

```
<payload>/
  payload.json                      payload-version 1, platform, and the version
                                    and relative path of each part
  bin/lsp-mcpp[.exe]
  clangd/bin/clangd[.exe]
  clangd/lib/clang/<major>/include/  clang's builtin headers, found beside clangd
  kit/kit.json + kit data            spec S4
  licenses/                          lsp-mcpp and LLVM license texts
```

The server takes the payload named by `--payload`, or else the payload that
contains its own executable. `--clangd` and `--kit` replace one part of it. A
part that is still missing is looked for outside a payload: clangd on `PATH`,
and the kit installed by xlings.

## Inputs: `payload.lock.json`

| Entry | What | Used by |
|---|---|---|
| `clangd-linux`, `clangd-mac`, `clangd-windows` | clangd 23.1.0 release archives | `trim_clangd.py` |
| `llvm-project-src` | llvm-project 23.1.0 source archive | `build_kit.py`, recipe `libcxx-source` |
| `llvm-mingw` | llvm-mingw 20260826 (LLVM 23.1.0), UCRT, Linux x86_64 host | `build_kit.py`, recipe `llvm-mingw` |

`platforms` maps each payload platform to its clangd entry and its kit recipe,
source and target triple. Versions change here and nowhere else. Each sha256 was
checked against two independent downloads and against the digest GitHub records
for the release asset. The server's version is read from `mcpp.toml`.

## Building a payload

CI runs exactly these steps (`.github/workflows/ci.yml`, job `payload`); the
downloads are cached in `.payload-cache`.

```bash
mcpp build                                   # --target x86_64-windows-gnu | --target aarch64-macos
python3 packaging/scripts/trim_clangd.py --platform linux-x64 --out work/clangd --cache .payload-cache
python3 packaging/scripts/build_kit.py   --platform linux-x64 --out work/kit --cache .payload-cache --work work/kit-build
python3 packaging/scripts/assemble_payload.py --platform linux-x64 \
    --server target/<triple>/<fingerprint>/bin/lsp-mcpp --clangd work/clangd --kit work/kit --out payload
python3 packaging/scripts/assemble_payload.py --verify payload
```

| Platform | Host | Tools |
|---|---|---|
| `linux-x64` | Linux with `dpkg` and the `libc6-dev` and `linux-libc-dev` packages installed | cmake, ninja, a C and C++ compiler for libc++'s configure checks; `strip` if available |
| `win32-x64` | any; CI uses Linux and cross-builds the server | nothing beyond Python |
| `darwin-arm64` | macOS | cmake, ninja, and the Xcode command line tools (`lipo`, `strip`, `codesign`) |

The server is built with the **dev** profile. Optimized builds over
openkal-windows and openkal-macos misbehaved at startup (design §12.9, K7); the
fixes are released upstream, and payloads move to the release profile once the
runtime that carries them has been verified on every platform.

To run the VS Code extension against a local payload, put it at
`editors/vscode/payload` or point `LSP_MCPP_PAYLOAD` at it.

## What the verification checks

`assemble_payload.py --verify` fails on the first build that would not work on a
user's machine:

- `payload.json` has the expected version, platform and part paths, and every
  part has a version.
- The server and clangd exist and are executable, and clang's builtin headers
  are present for the clangd major version.
- `kit.json` names the same kit as the manifest, uses kit-version 1, and every
  include directory, sysroot and license it lists exists.
- The kit's module manifest provides `std`, and every module source and module
  include directory it names exists.
- **No two paths differ only in case.** A VSIX refuses such pairs and the file
  systems of Windows and macOS fold them into one. The Linux kernel headers
  carry several (`xt_mark.h` and `xt_MARK.h`); `build_kit.py` keeps the
  all-lowercase name and lists what it dropped.

## Sizes

Measured on the CI payloads of commit `70279c2`, in MiB. The server is a dev
build with debug information; `gz` is the payload directory compressed.

| Platform | Server | clangd | Kit | Total | gz | Kit gz | Files |
|---|---|---|---|---|---|---|---|
| linux-x64 | 30.6 | 74.5 | 20.8 | 125.9 | 35.6 | 3.9 | 3630 |
| win32-x64 | 58.8 | 64.9 | 93.3 | 217.1 | 41.9 | 10.4 | 3904 |
| darwin-arm64 | 7.2 | 76.7 | 12.4 | 96.3 | 25.6 | 1.7 | 2163 |

## Releases

A tag `v<version>` runs `.github/workflows/release.yml`: the full CI first, then
one job that checks the tag against `mcpp.toml` and the extension's
`package.json`, collects the three VSIX files and payloads, and publishes them
with a `SHA256SUMS` file as a GitHub release.

`xlings_artifacts.py` splits each payload into the two archives the xlings
ecosystem installs, named by the xlings-res convention, and renders
`xlings/*.lua.in` with their hashes:

```
lsp-mcpp-<version>-<os>-<arch>.tar.gz          the server and its license
lsp-mcpp-kit-<kit version>-<os>-<arch>.tar.gz  the semantic kit
```

clangd is not part of either archive: the xlings package depends on
`llvm-tools`, which puts clangd on `PATH`. The Visual Studio Marketplace, Open
VSX and xlings-res mirror steps run only when their tokens are configured.
