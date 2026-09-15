#!/usr/bin/env python3
"""Review fixtures: whether `mcppls review` finds what a change breaks, and nothing a clean change does not.

    python3 bench/review.py --server PATH/mcppls --payload PAYLOAD [--fixture ID ...] [--keep] [--report FILE]

Each bench/review/<id>/ holds review.json and change/. The runner copies review.json's "project" (a conformance
fixture, relative to the repository root) to a scratch directory, commits it to a fresh git repository, writes
change/ over it and removes review.json's "remove" paths, then runs `mcppls review --format json` there and matches
the findings (overall design 10.3, work item RV5):

- every entry of "must" is matched by a finding of its "rule", in its "file" and on its "line" when those are given,
  with evidence in "evidence-file" when that is given;
- no finding matches an entry of "must-not" (the same fields, all optional but "rule").

A fixture passes when both hold. The summary counts, per rule, matched "must" entries (true positives), unmatched ones
(false negatives) and findings that match a "must-not" entry (false positives), and gives precision and recall.
Python standard library only; no network and no model.
"""
import argparse, hashlib, json, os, pathlib, shutil, subprocess, sys, tempfile, time

REPOSITORY = pathlib.Path(__file__).resolve().parent.parent
FIXTURES = REPOSITORY / "bench" / "review"


def matches(finding, entry):
    if finding.get("rule") != entry["rule"]:
        return False
    if "origin" in entry and finding.get("origin") != entry["origin"]:
        return False
    location = finding.get("location", {})
    if "file" in entry and location.get("file") != entry["file"]:
        return False
    if "line" in entry and location.get("line") != entry["line"]:
        return False
    if "evidence-file" in entry and not any(e.get("location", {}).get("file") == entry["evidence-file"] for e in finding.get("evidence", [])):
        return False
    return True


def snapshot(directory):
    """Every file under `directory`, the repository's own included, with its content digest."""
    files = {}
    for path in sorted(directory.rglob("*")):
        if path.is_file():
            files[str(path.relative_to(directory))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return files


def git(workspace, *arguments):
    subprocess.run(["git", "-c", "user.name=mcppls", "-c", "user.email=mcppls@example.invalid", "-c", "commit.gpgsign=false", *arguments],
                   cwd=workspace, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def run_fixture(fixture, server, payload, scratch, timeout, mock_model):
    meta = json.loads((fixture / "review.json").read_text(encoding="utf-8"))
    workspace = scratch / meta["id"]
    model = meta.get("model")
    if model and not mock_model:
        return {"id": meta["id"], "ok": True, "skipped": "needs --mock-model", "seconds": 0.0, "problems": [], "findings": [], "meta": meta}
    shutil.copytree(REPOSITORY / meta["project"], workspace, ignore=shutil.ignore_patterns("scenario.json"))
    git(workspace, "init", "-q")
    git(workspace, "add", "-A")
    git(workspace, "commit", "-qm", "base")
    for source in (fixture / "change").rglob("*"):
        if source.is_file():
            target = workspace / source.relative_to(fixture / "change")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
    for removed in meta.get("remove", []):
        path = workspace / removed
        shutil.rmtree(path) if path.is_dir() else path.unlink(missing_ok=True)

    environment = dict(os.environ, MCPPLS_CACHE_DIR=str(scratch / "cache"))
    command = [server, "review", "--format", "json", "--root", str(workspace), "--timeout", str(timeout)]
    if payload:
        command += ["--payload", payload]
    if model:
        # A scripted gateway answers in place of a model: the model step runs end to end, and nothing leaves the machine.
        script = scratch / f"{meta['id']}-model.json"
        script.write_text(json.dumps(model["script"]), encoding="utf-8")
        environment["MCPPLS_MOCK_MODEL_SCRIPT"] = str(script)
        command += ["--model", "gateway", "--model-gateway", mock_model, "--model-name", model["script"].get("model", "mock-model")]
    before = snapshot(workspace)
    started = time.monotonic()
    completed = subprocess.run(command, cwd=workspace, env=environment, capture_output=True, text=True, timeout=timeout + 60)
    seconds = time.monotonic() - started
    unchanged = snapshot(workspace) == before
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError:
        return {"id": meta["id"], "ok": False, "seconds": seconds, "problems": [f"the output is not JSON (exit {completed.returncode}): {completed.stdout[:300]} {completed.stderr[-500:]}"],
                "findings": [], "meta": meta}
    if "error" in result:
        return {"id": meta["id"], "ok": False, "seconds": seconds, "problems": [f"review failed: {result['error']}"], "findings": [], "meta": meta}
    findings = result.get("findings", [])
    problems = []
    if not unchanged:
        problems.append("the review changed the workspace")
    matched_must = []
    for entry in meta.get("must", []):
        hit = any(matches(finding, entry) for finding in findings)
        matched_must.append(hit)
        if not hit:
            problems.append(f"missing {json.dumps(entry)}")
    false_positives = []
    for entry in meta.get("must-not", []):
        for finding in findings:
            if matches(finding, entry):
                false_positives.append(finding)
                problems.append(f"unexpected {finding['rule']} at {finding['location']['file']}:{finding['location']['line']}: {finding['message']}")
    return {"id": meta["id"], "ok": not problems, "seconds": seconds, "problems": problems, "findings": findings, "meta": meta,
            "matched": matched_must, "false-positives": false_positives, "complete": result.get("complete", True)}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--server", required=True)
    parser.add_argument("--payload", default="")
    parser.add_argument("--fixture", action="append", default=[])
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--report", default="")
    parser.add_argument("--mock-model", default="", help="mcppls-mock-model, for fixtures whose review.json scripts a model")
    arguments = parser.parse_args()
    server = str(pathlib.Path(arguments.server).resolve())
    payload = str(pathlib.Path(arguments.payload).resolve()) if arguments.payload else ""
    mock_model = str(pathlib.Path(arguments.mock_model).resolve()) if arguments.mock_model else ""
    fixtures = sorted(p for p in FIXTURES.iterdir() if (p / "review.json").is_file())
    if arguments.fixture:
        fixtures = [p for p in fixtures if p.name in arguments.fixture]
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="mcppls-review-"))
    results = []
    try:
        for fixture in fixtures:
            outcome = run_fixture(fixture, server, payload, scratch, arguments.timeout, mock_model)
            results.append(outcome)
            if outcome.get("skipped"):
                print(f"SKIP {outcome['id']} ({outcome['skipped']})", flush=True)
                continue
            print(f"{'PASS' if outcome['ok'] else 'FAIL'} {outcome['id']} ({outcome['seconds']:.1f}s, {len(outcome['findings'])} finding(s))", flush=True)
            for problem in outcome["problems"]:
                print(f"  {problem}", flush=True)
    finally:
        if not arguments.keep:
            shutil.rmtree(scratch, ignore_errors=True)

    per_rule = {}
    for outcome in results:
        for entry, hit in zip(outcome["meta"].get("must", []), outcome.get("matched", [])):
            stats = per_rule.setdefault(entry["rule"], {"tp": 0, "fn": 0, "fp": 0})
            stats["tp" if hit else "fn"] += 1
        for finding in outcome.get("false-positives", []):
            per_rule.setdefault(finding["rule"], {"tp": 0, "fn": 0, "fp": 0})["fp"] += 1
    print("\nrule                                  precision  recall   tp fn fp")
    for rule, stats in sorted(per_rule.items()):
        precision = stats["tp"] / (stats["tp"] + stats["fp"]) if stats["tp"] + stats["fp"] else 1.0
        recall = stats["tp"] / (stats["tp"] + stats["fn"]) if stats["tp"] + stats["fn"] else 1.0
        stats.update(precision=precision, recall=recall)
        print(f"{rule:<38}{precision:>9.0%}{recall:>8.0%}  {stats['tp']:>3}{stats['fn']:>3}{stats['fp']:>3}")
    failed = [r["id"] for r in results if not r["ok"]]
    print(f"\n{len(results) - len(failed)}/{len(results)} review fixtures passed")
    if arguments.report:
        pathlib.Path(arguments.report).write_text(json.dumps({"fixtures": [{k: v for k, v in r.items() if k != "meta"} for r in results], "rules": per_rule}, indent=2),
                                                  encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
