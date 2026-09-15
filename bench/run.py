#!/usr/bin/env python3
"""mcppls agent task benchmark runner (design doc §10.2, work item A2).

Python standard library only — no third-party dependencies, so this runs
unchanged in CI and on a contributor's machine.

Subcommands
-----------
validate  For every task: copy its project, run its `check` commands and
          expect at least one to fail (the baseline must NOT already solve
          the task), apply its `reference` patch to a second copy, run the
          checks again and expect all of them to pass. No agent, no network,
          no API key. This is what CI's `bench-validate` job runs.

run       Point a coding agent (Claude Code or GitHub Copilot CLI) at each
          task, headless, under one "arm" (no tooling beyond grep, the
          official clangd-lsp integration, mcppls-lsp, or mcppls-mcp), and
          record whether the checks pass afterward, plus timing and, when
          the agent's own JSON output reports them, turns and tokens. This
          spends API credits / model usage on every invocation: it refuses
          to run without --i-understand-this-uses-api-credits, and nothing
          in this repository's own tooling (CI included) invokes it.

See bench/README.md for the task and result formats.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

BENCH_DIR = Path(__file__).resolve().parent
DEFAULT_TASKS_DIR = BENCH_DIR / "tasks"
REQUIRED_TASK_FIELDS = ("id", "title", "prompt", "project", "check", "reference", "shape")
VALID_SHAPES = ("split", "all-cppm")
VALID_ARMS = ("grep", "clangd-lsp", "mcppls-lsp", "mcppls-mcp")
VALID_AGENTS = ("claude-code", "copilot-cli")
DEFAULT_CHECK_TIMEOUT_S = 300


class TaskError(Exception):
    """A task.json (or its project/reference files) is malformed."""


# --------------------------------------------------------------------------
# Task loading
# --------------------------------------------------------------------------

@dataclass
class Task:
    id: str
    title: str
    prompt: str
    check: list
    shape: str
    toolchain: Optional[str]
    dir: Path
    project_dir: Path
    reference_path: Path


def load_task(task_dir: Path) -> Task:
    task_json = task_dir / "task.json"
    if not task_json.is_file():
        raise TaskError(f"{task_dir}: no task.json")
    try:
        raw = json.loads(task_json.read_text())
    except json.JSONDecodeError as exc:
        raise TaskError(f"{task_json}: invalid JSON ({exc})") from exc

    missing = [f for f in REQUIRED_TASK_FIELDS if f not in raw]
    if missing:
        raise TaskError(f"{task_json}: missing field(s) {missing}")
    if raw["id"] != task_dir.name:
        raise TaskError(f"{task_json}: id {raw['id']!r} != directory name {task_dir.name!r}")
    if raw["shape"] not in VALID_SHAPES:
        raise TaskError(f"{task_json}: shape must be one of {VALID_SHAPES}, got {raw['shape']!r}")
    if not isinstance(raw["check"], list) or not raw["check"]:
        raise TaskError(f"{task_json}: 'check' must be a non-empty list of commands")
    for command in raw["check"]:
        if not isinstance(command, list) or not command or not all(isinstance(p, str) for p in command):
            raise TaskError(f"{task_json}: each 'check' entry must be a non-empty list of strings, got {command!r}")

    project_dir = task_dir / raw["project"]
    if not project_dir.is_dir():
        raise TaskError(f"{task_json}: project directory {project_dir} does not exist")
    if not (project_dir / "mcpp.toml").is_file():
        raise TaskError(f"{task_json}: {project_dir} has no mcpp.toml")
    reference_path = task_dir / raw["reference"]
    if not reference_path.is_file():
        raise TaskError(f"{task_json}: reference patch {reference_path} does not exist")

    return Task(
        id=raw["id"], title=raw["title"], prompt=raw["prompt"], check=raw["check"],
        shape=raw["shape"], toolchain=raw.get("toolchain"),
        dir=task_dir, project_dir=project_dir, reference_path=reference_path,
    )


def discover_tasks(tasks_dir: Path, only: Optional[str] = None) -> list:
    task_dirs = sorted(p.parent for p in tasks_dir.glob("*/task.json"))
    if only:
        task_dirs = [d for d in task_dirs if d.name == only]
        if not task_dirs:
            raise TaskError(f"no task named {only!r} under {tasks_dir}")
    return [load_task(d) for d in task_dirs]


# --------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------

@dataclass
class CheckOutcome:
    ok: bool
    command: Optional[list] = None
    returncode: Optional[int] = None
    stdout: str = ""
    stderr: str = ""

    def describe(self) -> str:
        if self.ok:
            return "all checks passed"
        where = " ".join(self.command) if self.command else "?"
        tail = "\n".join((self.stdout + "\n" + self.stderr).strip().splitlines()[-15:])
        return f"`{where}` exited {self.returncode}\n{tail}"


def run_checks(commands: list, cwd: Path, timeout: int = DEFAULT_CHECK_TIMEOUT_S) -> CheckOutcome:
    """Runs each command in order, like a `&&` chain: stops at the first
    failure (non-zero exit, missing executable, or timeout)."""
    for command in commands:
        try:
            proc = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        except FileNotFoundError as exc:
            return CheckOutcome(False, command, None, "", str(exc))
        except subprocess.TimeoutExpired as exc:
            stdout = exc.stdout if isinstance(exc.stdout, str) else (exc.stdout or b"").decode("utf-8", "replace")
            stderr = exc.stderr if isinstance(exc.stderr, str) else (exc.stderr or b"").decode("utf-8", "replace")
            return CheckOutcome(False, command, None, stdout, stderr + f"\n[timed out after {timeout}s]")
        if proc.returncode != 0:
            return CheckOutcome(False, command, proc.returncode, proc.stdout, proc.stderr)
    return CheckOutcome(True)


def copy_project(project_dir: Path, dest: Path) -> None:
    """Copies a task's project, stripped of any build output a previous
    `mcpp build`/`mcpp test` left behind (target/, and the compile_commands.json
    mcpp writes at the project root) so every run starts from source only."""
    shutil.copytree(project_dir, dest)
    stale_target = dest / "target"
    if stale_target.exists():
        shutil.rmtree(stale_target)
    stale_cdb = dest / "compile_commands.json"
    if stale_cdb.exists():
        stale_cdb.unlink()


# --------------------------------------------------------------------------
# Patch application: `git apply`, falling back to a small pure-Python
# unified-diff applier when git is unavailable or refuses the patch. Every
# reference.patch is generated the same way (see bench/README.md, "Authoring
# a task"): `git diff --no-index a b` between two directories literally
# named `a` and `b`, so every path in the patch reads `a/a/<relpath>` and
# `b/b/<relpath>` — git's own a/, b/ prefixes around the two directory
# names. Applying against a project copy's own root therefore strips 2
# leading path components, not the usual 1. Patches under bench/tasks/ are
# generated by this project, not accepted from elsewhere, so the fallback
# does not need to be fuzzy: it trusts hunk context to match exactly.
# --------------------------------------------------------------------------

_HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
PATCH_STRIP_COMPONENTS = 2


def apply_patch(patch_path: Path, project_copy: Path) -> tuple:
    """Returns (ok, detail)."""
    git = shutil.which("git")
    git_error = None
    if git:
        proc = subprocess.run(
            [git, "apply", f"-p{PATCH_STRIP_COMPONENTS}", "--whitespace=nowarn", str(patch_path.resolve())],
            cwd=project_copy, capture_output=True, text=True,
        )
        if proc.returncode == 0:
            return True, ""
        git_error = proc.stderr.strip()
    try:
        _apply_unified_diff(patch_path.read_text(), project_copy)
        return True, ""
    except Exception as exc:  # pure-Python fallback failed too
        detail = f"pure-Python patch apply: {exc}"
        if git_error:
            detail = f"git apply: {git_error}\n{detail}"
        return False, detail


def _diff_path(header_rest: str):
    header_rest = header_rest.split("\t", 1)[0].strip()
    if header_rest == "/dev/null":
        return None
    parts = header_rest.split("/", PATCH_STRIP_COMPONENTS)
    return parts[PATCH_STRIP_COMPONENTS] if len(parts) > PATCH_STRIP_COMPONENTS else header_rest


def _apply_unified_diff(patch_text: str, root: Path) -> None:
    lines = patch_text.splitlines(keepends=True)
    i, n = 0, len(lines)
    while i < n:
        if not lines[i].startswith("--- "):
            i += 1
            continue
        old_header = lines[i]
        i += 1
        if i >= n or not lines[i].startswith("+++ "):
            raise ValueError(f"'+++' expected after {old_header!r}")
        new_header = lines[i]
        i += 1
        old_path = _diff_path(old_header[4:])
        new_path = _diff_path(new_header[4:])
        hunks = []
        while i < n and lines[i].startswith("@@"):
            m = _HUNK_RE.match(lines[i])
            if not m:
                raise ValueError(f"malformed hunk header: {lines[i]!r}")
            old_start = int(m.group(1))
            i += 1
            body = []
            while i < n and lines[i][:1] in (" ", "-", "+"):
                body.append(lines[i])
                i += 1
                if i < n and lines[i].startswith("\\"):
                    i += 1  # "\ No newline at end of file"
            hunks.append((old_start, body))
        _apply_file_hunks(root, old_path, new_path, hunks)


def _apply_file_hunks(root: Path, old_path, new_path, hunks) -> None:
    target_rel = new_path if new_path is not None else old_path
    if target_rel is None:
        raise ValueError("a hunk names neither an old nor a new file")
    original = (root / old_path).read_text().splitlines(keepends=True) if old_path is not None else []
    result: list = []
    cursor = 0
    for old_start, body in hunks:
        start0 = max(old_start - 1, 0) if old_path is not None else 0
        result.extend(original[cursor:start0])
        cursor = start0
        for hline in body:
            tag, content = hline[0], hline[1:]
            if tag == " ":
                if cursor >= len(original) or original[cursor] != content:
                    raise ValueError(f"context mismatch in {target_rel} at original line {cursor + 1}")
                result.append(content)
                cursor += 1
            elif tag == "-":
                if cursor >= len(original) or original[cursor] != content:
                    raise ValueError(f"deletion mismatch in {target_rel} at original line {cursor + 1}")
                cursor += 1
            elif tag == "+":
                result.append(content)
    result.extend(original[cursor:])
    target = root / target_rel
    if new_path is None:
        if target.exists():
            target.unlink()
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("".join(result))


# --------------------------------------------------------------------------
# validate
# --------------------------------------------------------------------------

def validate_task(task: Task, tmp_root: Path, timeout: int) -> tuple:
    """Returns (ok, detail)."""
    baseline = tmp_root / f"{task.id}-baseline"
    copy_project(task.project_dir, baseline)
    baseline_outcome = run_checks(task.check, baseline, timeout)
    if baseline_outcome.ok:
        return False, "baseline already satisfies every check (it must fail at least one)"

    patched = tmp_root / f"{task.id}-patched"
    copy_project(task.project_dir, patched)
    applied, apply_detail = apply_patch(task.reference_path, patched)
    if not applied:
        return False, f"reference patch did not apply: {apply_detail}"
    patched_outcome = run_checks(task.check, patched, timeout)
    if not patched_outcome.ok:
        return False, f"reference patch applied but checks still fail: {patched_outcome.describe()}"
    return True, "baseline fails, patched passes"


def cmd_validate(args: argparse.Namespace) -> int:
    tasks_dir = Path(args.tasks) if args.tasks else DEFAULT_TASKS_DIR
    if shutil.which("mcpp") is None:
        print("error: `mcpp` is not on PATH (see README.md for setup)", file=sys.stderr)
        return 2
    try:
        tasks = discover_tasks(tasks_dir, args.only)
    except TaskError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if not tasks:
        print(f"error: no tasks found under {tasks_dir}", file=sys.stderr)
        return 2

    failures = 0
    with tempfile.TemporaryDirectory(prefix="mcppls-bench-") as tmp:
        tmp_root = Path(tmp)
        for task in tasks:
            start = time.monotonic()
            try:
                ok, detail = validate_task(task, tmp_root, args.timeout)
            except Exception as exc:  # keep going; report every task
                ok, detail = False, f"unexpected error: {exc}"
            elapsed = time.monotonic() - start
            status = "PASS" if ok else "FAIL"
            print(f"{status} {task.id} ({task.shape}, {elapsed:.1f}s)")
            if not ok:
                failures += 1
                for line in detail.splitlines():
                    print(f"     {line}")
            # Free disk space between tasks; each copy carries a target/ build dir.
            shutil.rmtree(tmp_root / f"{task.id}-baseline", ignore_errors=True)
            shutil.rmtree(tmp_root / f"{task.id}-patched", ignore_errors=True)

    total = len(tasks)
    print(f"\n{total - failures}/{total} tasks validated")
    return 1 if failures else 0


# --------------------------------------------------------------------------
# run — spends API credits / model usage. Never invoked by this repository's
# own tooling or CI. See bench/README.md for the exact per-arm setup and,
# importantly, which parts of this are unverified (never having been run).
# --------------------------------------------------------------------------

SAFETY_FLAG = "--i-understand-this-uses-api-credits"


@dataclass
class RunRecord:
    task: str
    shape: str
    agent: str
    arm: str
    repeat: int
    success: bool
    wall_time_s: float
    agent_exit_code: Optional[int]
    turns: Optional[int] = None
    input_tokens: Optional[int] = None
    output_tokens: Optional[int] = None
    cost_usd: Optional[float] = None
    files_changed_outside_scope: list = field(default_factory=list)
    check_detail: str = ""
    agent_stderr_tail: str = ""


def _snapshot(root: Path) -> dict:
    """relpath -> (size, mtime_ns), skipping target/ (build output) and .git/."""
    out = {}
    for p in root.rglob("*"):
        if not p.is_file():
            continue
        rel = p.relative_to(root)
        if rel.parts and rel.parts[0] in ("target", ".git"):
            continue
        st = p.stat()
        out[rel.as_posix()] = (st.st_size, st.st_mtime_ns)
    return out


def _changed_outside_scope(before: dict, after: dict) -> list:
    changed = sorted(set(before) ^ set(after)) + sorted(
        p for p in (set(before) & set(after)) if before[p] != after[p]
    )
    changed = sorted(set(changed))
    return [p for p in changed if not (p.startswith("src/") or p.startswith("tests/"))]


def _build_claude_code_command(task: Task, project_copy: Path, arm: str) -> list:
    """See bench/README.md "Claude Code arms" for what each arm assumes is
    already installed, and which parts are unverified."""
    cmd = ["claude", "-p", task.prompt, "--output-format", "json",
           "--permission-mode", "acceptEdits", "--permission-prompts", "none"]
    if arm == "grep":
        cmd += ["--bare"]
    elif arm == "mcppls-lsp":
        plugin_dir = (BENCH_DIR.parent / "editors" / "claude-code" / "mcppls-lsp").resolve()
        cmd += ["--bare", "--plugin-dir", str(plugin_dir)]
    elif arm == "mcppls-mcp":
        # The MCP tools alone (`mcppls mcp`, S5 section 6), without the LSP plugin. UNVERIFIED:
        # never run here, since a run spends credits — see bench/README.md.
        config = project_copy.parent / "mcp-config.json"
        config.write_text(json.dumps({"mcpServers": {"mcppls": {"command": "mcppls", "args": ["mcp"]}}}, indent=2))
        cmd += ["--bare", "--mcp-config", str(config), "--strict-mcp-config", "--allowedTools", "mcp__mcppls"]
    elif arm == "clangd-lsp":
        # No local copy of the official clangd-lsp plugin to point --bare
        # --plugin-dir at, so this arm runs a normal (non-bare) session and
        # relies on the operator having installed clangd-lsp@claude-plugins-official
        # at user scope beforehand (`claude plugin install clangd-lsp@claude-plugins-official`)
        # and clangd itself being on PATH. UNVERIFIED — see bench/README.md.
        pass
    else:
        raise ValueError(f"unsupported arm for claude-code: {arm}")
    return cmd


_COPILOT_CLI_SERVER_CONFIGS = {
    # design 8.2 pairs mcppls-lsp against clangd-lsp as arms; Copilot CLI has
    # no plugin marketplace, so the clangd-lsp arm is a plain lsp.json entry
    # for clangd itself instead of a named "plugin". Both use the extension
    # set from editors/copilot-cli/lsp.json (trimmed here to the common
    # cases; see that file for the full, documented-fields-only list).
    "mcppls-lsp": ("mcppls", {
        "command": "mcppls", "args": ["serve"],
        "fileExtensions": {".cpp": "cpp", ".cppm": "cpp", ".h": "cpp", ".c": "c"},
    }),
    "clangd-lsp": ("clangd", {
        "command": "clangd", "args": ["--background-index"],
        "fileExtensions": {".cpp": "cpp", ".cppm": "cpp", ".h": "cpp", ".c": "c"},
    }),
}


def _build_copilot_cli_command(task: Task, project_copy: Path, arm: str) -> list:
    if arm == "mcppls-mcp":
        # UNVERIFIED like the Claude Code arm: the MCP server for one session, and no LSP registration.
        config = project_copy.parent / "mcp-config.json"
        config.write_text(json.dumps({"mcpServers": {"mcppls": {"type": "local", "command": "mcppls", "args": ["mcp"], "tools": ["*"]}}}, indent=2))
        return ["copilot", "-p", task.prompt, "--allow-all-tools", "--additional-mcp-config", f"@{config}"]
    if arm == "grep":
        pass  # no .github/lsp.json written for this arm: no LSP tooling at all
    elif arm in _COPILOT_CLI_SERVER_CONFIGS:
        server_name, server_config = _COPILOT_CLI_SERVER_CONFIGS[arm]
        github_dir = project_copy / ".github"
        github_dir.mkdir(parents=True, exist_ok=True)
        (github_dir / "lsp.json").write_text(json.dumps({"lspServers": {server_name: server_config}}, indent=2))
    else:
        raise ValueError(f"unsupported arm for copilot-cli: {arm}")
    return ["copilot", "-p", task.prompt, "--allow-all-tools"]


def run_one(agent: str, arm: str, task: Task, repeat_index: int, timeout: int) -> RunRecord:
    with tempfile.TemporaryDirectory(prefix=f"mcppls-bench-run-{task.id}-") as tmp:
        project_copy = Path(tmp) / "project"
        copy_project(task.project_dir, project_copy)
        before = _snapshot(project_copy)

        if agent == "claude-code":
            cmd = _build_claude_code_command(task, project_copy, arm)
        elif agent == "copilot-cli":
            cmd = _build_copilot_cli_command(task, project_copy, arm)
        else:
            raise ValueError(f"unsupported agent {agent!r}")

        start = time.monotonic()
        try:
            proc = subprocess.run(cmd, cwd=project_copy, capture_output=True, text=True, timeout=timeout)
            exit_code, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
        except subprocess.TimeoutExpired as exc:
            exit_code = None
            stdout = exc.stdout or ""
            stderr = (exc.stderr or "") + f"\n[agent timed out after {timeout}s]"
        wall_time = time.monotonic() - start

        turns = input_tokens = output_tokens = cost_usd = None
        if agent == "claude-code":
            # `claude -p --output-format json` prints one JSON object; see
            # bench/README.md for which of these fields are confirmed vs.
            # assumed from typical Agent SDK result-message shapes.
            try:
                payload = json.loads(stdout.strip().splitlines()[-1]) if stdout.strip() else {}
                turns = payload.get("num_turns")
                usage = payload.get("usage") or {}
                input_tokens = usage.get("input_tokens")
                output_tokens = usage.get("output_tokens")
                cost_usd = payload.get("total_cost_usd")
            except (json.JSONDecodeError, IndexError):
                pass
        # copilot-cli: no documented JSON output flag as of this writing, so
        # turns/tokens/cost stay None for that agent (see bench/README.md).

        after = _snapshot(project_copy)
        outside = _changed_outside_scope(before, after)

        check_outcome = run_checks(task.check, project_copy, timeout=DEFAULT_CHECK_TIMEOUT_S)
        return RunRecord(
            task=task.id, shape=task.shape, agent=agent, arm=arm, repeat=repeat_index,
            success=check_outcome.ok, wall_time_s=wall_time, agent_exit_code=exit_code,
            turns=turns, input_tokens=input_tokens, output_tokens=output_tokens, cost_usd=cost_usd,
            files_changed_outside_scope=outside, check_detail=check_outcome.describe(),
            agent_stderr_tail="\n".join(stderr.strip().splitlines()[-20:]),
        )


def cmd_run(args: argparse.Namespace) -> int:
    if not args.i_understand_this_uses_api_credits:
        print(
            f"error: refusing to run — pass {SAFETY_FLAG} to confirm you understand\n"
            "this invokes a real coding agent, which spends API credits or model\n"
            "usage on every task/arm/repeat. Nothing in this repository's own\n"
            "tooling or CI passes this flag; run it yourself, deliberately.",
            file=sys.stderr,
        )
        return 2
    # --agent/--arm are already restricted to VALID_AGENTS/VALID_ARMS by argparse `choices`.
    tasks_dir = Path(args.tasks) if args.tasks else DEFAULT_TASKS_DIR
    try:
        tasks = discover_tasks(tasks_dir, args.only)
    except TaskError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    agent_binary = "claude" if args.agent == "claude-code" else "copilot"
    if shutil.which(agent_binary) is None:
        print(f"error: `{agent_binary}` is not on PATH", file=sys.stderr)
        return 2

    runs = []
    for task in tasks:
        for repeat_index in range(1, args.repeat + 1):
            print(f"running {task.id} arm={args.arm} agent={args.agent} repeat={repeat_index}/{args.repeat}...",
                  file=sys.stderr)
            record = run_one(args.agent, args.arm, task, repeat_index, args.timeout)
            runs.append(record)
            print(f"  -> {'PASS' if record.success else 'FAIL'} in {record.wall_time_s:.1f}s", file=sys.stderr)

    successes = sum(1 for r in runs if r.success)
    out = {
        "agent": args.agent,
        "arm": args.arm,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "run_id": str(uuid.uuid4()),
        "tasks_dir": str(tasks_dir),
        "repeat": args.repeat,
        "summary": {"runs": len(runs), "successes": successes, "success_rate": successes / len(runs) if runs else 0},
        "runs": [r.__dict__ for r in runs],
    }
    Path(args.out).write_text(json.dumps(out, indent=2))
    print(f"wrote {args.out}: {successes}/{len(runs)} runs succeeded")
    return 0


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="mcppls agent task benchmark (design §10.2, A2)")
    sub = parser.add_subparsers(dest="command", required=True)

    p_validate = sub.add_parser("validate", help="check every task's baseline/reference pair without an agent")
    p_validate.add_argument("--tasks", help=f"tasks directory (default: {DEFAULT_TASKS_DIR})")
    p_validate.add_argument("--only", help="validate a single task id")
    p_validate.add_argument("--timeout", type=int, default=DEFAULT_CHECK_TIMEOUT_S,
                             help="seconds allowed per check command (default: %(default)s)")
    p_validate.set_defaults(func=cmd_validate)

    p_run = sub.add_parser("run", help="run a coding agent against every task (spends API credits)")
    p_run.add_argument("--agent", required=True, choices=VALID_AGENTS)
    p_run.add_argument("--arm", required=True, choices=VALID_ARMS)
    p_run.add_argument("--repeat", type=int, default=5, help="repeats per task (default: %(default)s)")
    p_run.add_argument("--out", required=True, help="path to write the results JSON to")
    p_run.add_argument("--tasks", help=f"tasks directory (default: {DEFAULT_TASKS_DIR})")
    p_run.add_argument("--only", help="run a single task id")
    p_run.add_argument("--timeout", type=int, default=900, help="seconds allowed per agent invocation (default: %(default)s)")
    p_run.add_argument(SAFETY_FLAG, dest="i_understand_this_uses_api_credits", action="store_true",
                        help="required: confirms you understand this spends API credits / model usage")
    p_run.set_defaults(func=cmd_run)

    return parser


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
