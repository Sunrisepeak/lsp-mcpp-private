#!/usr/bin/env python3
"""Drives a built mcppls-model against tests/mock_openai.py and asserts
PROTOCOL.md's contract. Prints PASS/FAIL per check and exits non-zero on the
first failure — cleanup of the gateway and mock processes always runs (each
check that fails calls sys.exit(1), which still unwinds through the
try/finally blocks below).

Usage: run.py --gateway <path to the built mcppls-model executable>

Runs no real model provider and needs no API key: every case is served by
the local mock (model-gateway/README.md "Verification").
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

TEST_SCHEMA = {
    "type": "object",
    "required": ["answer", "score"],
    "properties": {
        "answer": {"type": "string"},
        "score": {"type": "integer"},
    },
}

SLOW_CANCEL_DELAY_SECONDS = 0.3  # send `cancel` this long after `complete`; mock's slow reply takes 4s


def check(name, condition, detail=""):
    if condition:
        print(f"PASS {name}", flush=True)
        return
    print(f"FAIL {name}" + (f": {detail}" if detail else ""), flush=True)
    sys.exit(1)


class Gateway:
    """A running mcppls-model, speaking one JSON object per line (PROTOCOL.md)."""

    def __init__(self, path, args):
        self.process = subprocess.Popen(
            [path, *args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )
        self._next_id = 1
        self._stderr_lines = []
        self._stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self._stderr_thread.start()

    def _drain_stderr(self):
        try:
            for line in self.process.stderr:
                self._stderr_lines.append(line.rstrip("\n"))
        except ValueError:
            pass  # the pipe was closed while reading, at shutdown

    def send_request(self, method, params=None):
        request_id = self._next_id
        self._next_id += 1
        payload = {"id": request_id, "method": method}
        if params is not None:
            payload["params"] = params
        self._write(payload)
        return request_id

    def send_notification(self, method, params=None):
        payload = {"method": method}
        if params is not None:
            payload["params"] = params
        self._write(payload)

    def _write(self, payload):
        self.process.stdin.write(json.dumps(payload) + "\n")
        self.process.stdin.flush()

    def read_response(self, timeout=15):
        """One JSON line from stdout, or None on timeout or a closed stream."""
        result = {}

        def _read():
            result["line"] = self.process.stdout.readline()

        reader = threading.Thread(target=_read, daemon=True)
        reader.start()
        reader.join(timeout)
        if reader.is_alive() or "line" not in result or not result["line"]:
            return None
        return json.loads(result["line"])

    def request(self, method, params=None, timeout=15):
        request_id = self.send_request(method, params)
        response = self.read_response(timeout)
        return request_id, response

    def close(self, timeout=5):
        try:
            self.process.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            return self.process.returncode

    def stderr_excerpt(self):
        return "\n".join(self._stderr_lines[-20:])


def complete_params(user_message, schema=None):
    params = {"messages": [{"role": "user", "content": user_message}]}
    if schema is not None:
        params["schema"] = schema
    return params


def start_mock(python, mock_path):
    mock = subprocess.Popen(
        [python, str(mock_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        bufsize=1,
    )
    port_line = mock.stdout.readline().strip()
    check("mock server prints a port", port_line.isdigit(), f"first line was {port_line!r}")
    return mock, int(port_line)


def stop_mock(mock, timeout=5):
    mock.terminate()
    try:
        mock.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        mock.kill()
        mock.wait()


def run_checks(gateway_path, endpoint):
    gw = Gateway(gateway_path, [
        "--provider", "openai",
        "--endpoint", endpoint,
        "--model", "test-model",
        "--timeout", "20",
    ])
    try:
        # initialize
        _, response = gw.request("initialize")
        check("initialize: responds", response is not None, gw.stderr_excerpt())
        result = response.get("result") or {}
        check("initialize: protocol is 1", result.get("protocol") == 1, str(result))
        check("initialize: gateway.name is mcppls-model", (result.get("gateway") or {}).get("name") == "mcppls-model", str(result))
        models = result.get("models") or []
        model_entry = next((m for m in models if m.get("id") == "test-model"), None)
        check("initialize: models lists the configured model", model_entry is not None, str(models))
        check("initialize: openai model has structuredOutput true", model_entry.get("structuredOutput") is True, str(model_entry))

        # plain completion, with usage
        _, response = gw.request("complete", complete_params("plain"))
        check("plain: responds", response is not None, gw.stderr_excerpt())
        result = response.get("result") or {}
        check("plain: content is non-empty text", isinstance(result.get("content"), str) and len(result["content"]) > 0, str(result))
        check("plain: finishReason is stop", result.get("finishReason") == "stop", str(result))
        usage = result.get("usage") or {}
        check("plain: usage.inputTokens > 0", isinstance(usage.get("inputTokens"), int) and usage["inputTokens"] > 0, str(usage))
        check("plain: usage.outputTokens > 0", isinstance(usage.get("outputTokens"), int) and usage["outputTokens"] > 0, str(usage))

        # schema completion returning parsed json
        _, response = gw.request("complete", complete_params("json-ok", TEST_SCHEMA))
        check("json-ok: responds", response is not None, gw.stderr_excerpt())
        result = response.get("result") or {}
        parsed = result.get("json")
        check("json-ok: result.json is present and matches the schema",
              isinstance(parsed, dict) and isinstance(parsed.get("answer"), str) and isinstance(parsed.get("score"), int),
              str(result))

        # a non-JSON reply against a schema -> 1004
        _, response = gw.request("complete", complete_params("json-bad", TEST_SCHEMA))
        check("json-bad: responds", response is not None, gw.stderr_excerpt())
        error = response.get("error") or {}
        check("json-bad: error code is 1004", error.get("code") == 1004, str(response))

        # provider HTTP failure -> 1001
        _, response = gw.request("complete", complete_params("http-500"))
        check("http-500: responds", response is not None, gw.stderr_excerpt())
        error = response.get("error") or {}
        check("http-500: error code is 1001", error.get("code") == 1001, str(response))

        # cancel a slow request -> 1003, promptly (well before the mock's sleep ends)
        request_id = gw.send_request("complete", complete_params("slow"))
        time.sleep(SLOW_CANCEL_DELAY_SECONDS)
        cancel_sent_at = time.monotonic()
        gw.send_notification("cancel", {"id": request_id})
        response = gw.read_response(timeout=15)
        cancel_latency = time.monotonic() - cancel_sent_at
        check("cancel: responds", response is not None, gw.stderr_excerpt())
        check("cancel: id matches the cancelled request", response.get("id") == request_id, str(response))
        error = response.get("error") or {}
        check("cancel: error code is 1003", error.get("code") == 1003, str(response))
        check("cancel: answered promptly (not after the mock's full sleep)", cancel_latency < 3.0, f"{cancel_latency:.1f}s")

        # shutdown -> result null; the process itself exits once stdin closes (checked below)
        _, response = gw.request("shutdown")
        check("shutdown: responds", response is not None, gw.stderr_excerpt())
        check("shutdown: result is null", "result" in response and response["result"] is None, str(response))
    finally:
        exit_code = gw.close()
        check("gateway exits 0 at end of input", exit_code == 0, f"exit code {exit_code}; stderr:\n{gw.stderr_excerpt()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gateway", required=True, help="Path to the built mcppls-model executable")
    args = parser.parse_args()

    gateway_path = str(Path(args.gateway).resolve())
    check("gateway executable exists", os.path.isfile(gateway_path), gateway_path)

    mock_path = Path(__file__).resolve().parent / "mock_openai.py"
    mock, port = start_mock(sys.executable, mock_path)
    try:
        run_checks(gateway_path, f"http://127.0.0.1:{port}/v1")
    finally:
        stop_mock(mock)

    print("PASS: all checks passed", flush=True)


if __name__ == "__main__":
    main()
