"""Builds the fixture with clang-cl and the MSVC STL std module, as a build tool would, and writes compile_commands.json.

clang-cl passes /clang: arguments after its inputs, so an .ixx cannot be named a module interface; the std module is
compiled from a .cppm copy of std.ixx (usable plan E7).
"""
import json, os, pathlib, shutil, subprocess, sys

root = pathlib.Path.cwd()
build = root / "build"
build.mkdir(exist_ok=True)
tools = pathlib.Path(os.environ["VCToolsInstallDir"])
shutil.copyfile(tools / "modules" / "std.ixx", build / "std.cppm")
clangcl = shutil.which("clang-cl") or sys.exit("clang-cl is not on PATH")
common = [clangcl, "/nologo", "/std:c++latest", "/EHsc", "/MD", "/c"]
steps = [
    (None, [*common, "/clang:-Wno-reserved-module-identifier", "/clang:-Wno-include-angled-in-module-purview",
            f"/clang:-fmodule-output={build / 'std.pcm'}", f"/Fo{build / 'std.obj'}", str(build / "std.cppm")]),
    (root / "src" / "greet.cppm", [*common, f"/clang:-fmodule-output={build / 'greet.pcm'}", f"/clang:-fmodule-file=std={build / 'std.pcm'}",
                                   f"/Fo{build / 'greet.obj'}", str(root / "src" / "greet.cppm")]),
    (root / "src" / "main.cpp", [*common, f"/clang:-fmodule-file=std={build / 'std.pcm'}", f"/clang:-fmodule-file=greet={build / 'greet.pcm'}",
                                 f"/Fo{build / 'main.obj'}", str(root / "src" / "main.cpp")]),
]
database = []
for source, argv in steps:
    print("+", subprocess.list2cmdline(argv), flush=True)
    subprocess.run(argv, cwd=root, check=True)
    if source is not None:
        database.append({"directory": str(root), "file": str(source), "arguments": argv})
(root / "compile_commands.json").write_text(json.dumps(database, indent=2), encoding="utf-8")
