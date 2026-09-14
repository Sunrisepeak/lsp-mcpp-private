"""Writes build_database.json: an S1 document the workspace carries itself (usable plan W9.2),
with two sets ("variant1", "variant2") that both compile src/main.cpp, one with -DVARIANT=1 and
the other with -DVARIANT=2. No build system runs; this is the database a producer would have
written. The toolchain is the LLVM 22.1.8 this repository's other Linux fixtures already use, so
the compiler introspection the server does at load time (project/infer.cpp's enrich_database)
resolves it instead of guessing from a fake driver path.
"""
import json
import os
import pathlib

root = pathlib.Path.cwd()
llvm = pathlib.Path(os.environ["HOME"]) / ".mcpp" / "registry" / "data" / "xpkgs" / "xim-x-llvm" / "22.1.8"
clangxx = str(llvm / "bin" / "clang++")
source = str(root / "src" / "main.cpp")


def translation_unit(variant):
    return {
        "source": source,
        "work-directory": str(root),
        "arguments": [clangxx, "-std=c++23", f"-DVARIANT={variant}", "-c", source,
                      "-o", str(root / f"main-{variant}.o")],
        "local-arguments": [f"-DVARIANT={variant}"],
    }


def set_for(variant):
    return {
        "name": f"variant{variant}",
        "family-name": "probe",
        "visible-sets": [],
        "baseline-arguments": ["-std=c++23"],
        "ide": {"toolchain": "llvm-22.1.8", "configuration": f"variant{variant}", "kind": "executable"},
        "translation-units": [translation_unit(variant)],
    }


database = {
    "version": 1,
    "revision": 0,
    "ide": {
        "profile-version": "0.2.0",
        "generator": {"name": "lsp-mcpp-conformance", "version": "0.0.0"},
        "toolchains": {
            "llvm-22.1.8": {
                "family": "clang",
                "version": "22.1.8",
                "driver": clangxx,
                "target": "x86_64-unknown-linux-gnu",
            },
        },
    },
    "sets": [set_for(1), set_for(2)],
}
(root / "build_database.json").write_text(json.dumps(database, indent=2), encoding="utf-8")
