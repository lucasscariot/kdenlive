#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kdenlive contributors
# SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Stdio MCP front for Kdenlive's native HTTP MCP endpoint.

Agent harnesses register MCP servers when a session starts. The editor's
endpoint only exists while Kdenlive runs, so a session started before the
editor loses the desktop_* tools and agents fall back to hand-written HTTP
clients. This shim is always startable: it speaks MCP over stdio, forwards
every request to the editor's HTTP endpoint, and when the editor is not
running it launches it (kdenlive-live by default) and waits for the endpoint.

Only the Python standard library is used. Configure through the environment:

  KDENLIVE_MCP_URL     endpoint, default http://127.0.0.1:8765/mcp
  KDENLIVE_MCP_TOKEN   bearer token when access tokens are enabled
  KDENLIVE_MCP_LAUNCH  command that starts the editor, default "kdenlive-live";
                       empty disables launching
  KDENLIVE_MCP_WAIT    seconds to wait for the endpoint after launching, default 90

Register it like any stdio server, for example with Codex:
  codex mcp add kdenlive -- python3 /path/to/kdenlive-mcp-stdio.py
or Claude Code:
  claude mcp add --scope user kdenlive -- python3 /path/to/kdenlive-mcp-stdio.py

Run with --check to test the connection and print the tool count.
"""
import json
import os
import shlex
import subprocess
import sys
import time
import urllib.error
import urllib.request

URL = os.environ.get("KDENLIVE_MCP_URL", "http://127.0.0.1:8765/mcp")
TOKEN = os.environ.get("KDENLIVE_MCP_TOKEN", "")
LAUNCH = os.environ.get("KDENLIVE_MCP_LAUNCH", "kdenlive-live")
WAIT = float(os.environ.get("KDENLIVE_MCP_WAIT", "90"))
PROTOCOL = "2025-03-26"


def log(message):
    sys.stderr.write(f"kdenlive-mcp-stdio: {message}\n")
    sys.stderr.flush()


class Upstream:
    """One HTTP MCP session with the editor, re-initialised whenever it is lost."""

    def __init__(self):
        self.session = None
        self.counter = 0
        self.client_info = {"name": "kdenlive-mcp-stdio", "version": "1.0"}
        self.launched = False

    def post(self, body, timeout=180):
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if TOKEN:
            headers["Authorization"] = f"Bearer {TOKEN}"
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        request = urllib.request.Request(URL, json.dumps(body).encode(), headers)
        with urllib.request.urlopen(request, timeout=timeout) as response:
            self.session = response.headers.get("Mcp-Session-Id", self.session)
            data = response.read()
        return json.loads(data) if data else None

    def initialize(self):
        self.session = None
        self.counter += 1
        result = self.post({"jsonrpc": "2.0", "id": self.counter, "method": "initialize",
                            "params": {"protocolVersion": PROTOCOL, "capabilities": {}, "clientInfo": self.client_info}}, timeout=10)
        self.post({"jsonrpc": "2.0", "method": "notifications/initialized"}, timeout=10)
        return result.get("result", {})

    def reachable(self):
        try:
            self.initialize()
            return True
        except (urllib.error.URLError, OSError, ValueError):
            return False

    def ensure(self):
        """Connect, launching the editor once when nothing answers."""
        if self.session or self.reachable():
            return True
        if not LAUNCH or self.launched:
            return False
        self.launched = True
        log(f"editor not reachable at {URL}; launching {LAUNCH}")
        try:
            subprocess.Popen(shlex.split(LAUNCH), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        except OSError as error:
            log(f"cannot launch the editor: {error}")
            return False
        deadline = time.monotonic() + WAIT
        while time.monotonic() < deadline:
            if self.reachable():
                log("editor is up")
                return True
            time.sleep(1)
        log("the editor did not open its MCP endpoint in time; enable the MCP API in its settings")
        return False

    def call(self, method, params):
        """Forward one request; a lost session is renewed once."""
        for attempt in range(2):
            if not self.ensure():
                raise ConnectionError(f"Kdenlive's MCP endpoint is not reachable at {URL}")
            self.counter += 1
            body = {"jsonrpc": "2.0", "id": self.counter, "method": method}
            if params is not None:
                body["params"] = params
            try:
                return self.post(body)
            except urllib.error.HTTPError as error:
                if error.code in (404, 400) and attempt == 0:
                    # Session expired or the editor restarted: initialise again and retry once.
                    self.session = None
                    continue
                raise
            except (urllib.error.URLError, OSError):
                self.session = None
                if attempt == 1:
                    raise
        raise ConnectionError("Kdenlive's MCP endpoint stopped answering")


def write(message):
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


def error_response(request_id, code, message):
    return {"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}}


def serve():
    upstream = Upstream()
    instructions = ("Kdenlive's native MCP, reached through the stdio shim. The editor is launched on demand; the first call may take a "
                    "few seconds. Read desktop_state first.")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except ValueError:
            write(error_response(None, -32700, "Invalid JSON."))
            continue
        method = message.get("method")
        request_id = message.get("id")
        params = message.get("params")
        if method == "initialize":
            if isinstance(params, dict) and isinstance(params.get("clientInfo"), dict):
                upstream.client_info = {"name": str(params["clientInfo"].get("name", "agent"))[:64], "version": "stdio"}
            upstream_instructions = ""
            if upstream.ensure():
                try:
                    upstream_instructions = upstream.initialize().get("instructions", "")
                except (urllib.error.URLError, OSError, ValueError):
                    upstream_instructions = ""
            write({"jsonrpc": "2.0", "id": request_id, "result": {
                "protocolVersion": params.get("protocolVersion", PROTOCOL) if isinstance(params, dict) else PROTOCOL,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "kdenlive-native-stdio", "version": "1.0.0"},
                "instructions": (upstream_instructions + " " + instructions).strip()}})
            continue
        if request_id is None:
            # Notifications need no answer; the upstream session was initialised by ensure().
            continue
        if method == "ping":
            write({"jsonrpc": "2.0", "id": request_id, "result": {}})
            continue
        if method not in ("tools/list", "tools/call"):
            write(error_response(request_id, -32601, "Method not found."))
            continue
        try:
            reply = upstream.call(method, params)
        except ConnectionError as error:
            if method == "tools/list":
                write({"jsonrpc": "2.0", "id": request_id, "result": {"tools": []}})
            else:
                write(error_response(request_id, -32000, str(error)))
            continue
        except urllib.error.HTTPError as error:
            write(error_response(request_id, -32000, f"Editor answered HTTP {error.code}: {error.read().decode(errors='replace')[:500]}"))
            continue
        except (urllib.error.URLError, OSError) as error:
            write(error_response(request_id, -32000, f"Editor connection failed: {error}"))
            continue
        if reply is None:
            write({"jsonrpc": "2.0", "id": request_id, "result": {}})
            continue
        reply["id"] = request_id
        write(reply)


def check():
    upstream = Upstream()
    if not upstream.ensure():
        log("no editor")
        return 1
    tools = upstream.call("tools/list", None).get("result", {}).get("tools", [])
    state = upstream.call("tools/call", {"name": "desktop_state", "arguments": {}}).get("result", {})
    structured = state.get("structuredContent", {})
    print(json.dumps({"url": URL, "tools": len(tools), "ok": structured.get("ok"),
                      "documentUrl": structured.get("data", {}).get("documentUrl"),
                      "error": structured.get("error")}, indent=2))
    return 0


if __name__ == "__main__":
    if "--check" in sys.argv[1:]:
        sys.exit(check())
    serve()
