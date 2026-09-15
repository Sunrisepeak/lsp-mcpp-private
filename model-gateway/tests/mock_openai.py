#!/usr/bin/env python3
"""A mock OpenAI-compatible chat-completions server for mcppls-model's tests.

Serves POST /v1/chat/completions on 127.0.0.1 with a port chosen by the OS,
prints that port on its own single stdout line, then serves until killed.
Python standard library only (see model-gateway/README.md "Tests").

The reply is chosen by the *last* user message's content (trimmed):
  plain     -> an ordinary text answer
  json-ok   -> a JSON object matching tests/run.py's TEST_SCHEMA
  json-bad  -> a reply that is not JSON (to exercise error 1004)
  http-500  -> HTTP 500 with an OpenAI-style error body (to exercise error 1001)
  slow      -> sleeps a few seconds before answering "plain" (to exercise cancel)
  anything else -> the same as "plain"
"""
import http.server
import json
import sys
import time

SLOW_SECONDS = 4


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # so a client's `Connection: close` is honored per-request

    def log_message(self, format_, *args):  # noqa: A002 - stdlib's own signature
        pass  # keep this process's stdout limited to the port line

    def log_error(self, format_, *args):
        pass  # a client that cancels mid-request leaves us writing to a closed
        # socket once SLOW_SECONDS elapses; that is expected, not a real error

    def do_POST(self):
        if self.path not in ("/v1/chat/completions", "/chat/completions"):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        length = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw.decode("utf-8"))
        except ValueError:
            body = {}

        behaviour = "plain"
        for message in reversed(body.get("messages") or []):
            if message.get("role") == "user":
                behaviour = (message.get("content") or "").strip() or "plain"
                break

        model = body.get("model") or "test-model"

        if behaviour == "http-500":
            self._reply_json(500, {"error": {"message": "mock provider failure", "type": "server_error"}})
            return

        if behaviour == "slow":
            time.sleep(SLOW_SECONDS)
            behaviour = "plain"

        if behaviour == "json-bad":
            content = "this is not json, sorry"
        elif behaviour == "json-ok":
            content = json.dumps({"answer": "42", "score": 7})
        else:
            content = "hello from the mock"

        self._reply_json(200, {
            "id": "mock-completion-1",
            "model": model,
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": content},
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 11, "completion_tokens": 5},
        })

    def _reply_json(self, status, payload):
        data = json.dumps(payload).encode("utf-8")
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass  # the client cancelled and closed its end; nothing to do


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass  # a cancelled "slow" request's late write races the client closing; not a real error


def main():
    server = Server(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    print(port, flush=True)
    try:
        server.serve_forever(poll_interval=0.1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
