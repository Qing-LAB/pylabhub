#!/usr/bin/env python3
"""
hubshell_client.py — OBSOLETE. Interactive admin client for pyLabHub.

    ┌──────────────────────────────────────────────────────────────────┐
    │  OBSOLETE — DOES NOT WORK AGAINST A CURRENT HUB.                 │
    │  Kept as a reference for the REPL ergonomics only.  Needs a      │
    │  full rewrite before it is useful again; do not treat anything   │
    │  below as a description of the current protocol.                 │
    └──────────────────────────────────────────────────────────────────┘

This client targets `pylabhub-hubshell`, a server that was retired and, before
that, never actually built.  It speaks a request shape — `{"token", "code"}`,
where `code` is Python evaluated inside the hub — that no current hub answers.

What replaced it, and what a rewrite has to do differently:

  * The server is `AdminService`, which serves a TYPED method surface,
    `{method, session_id, params}`, over a CURVE-encrypted ROUTER/DEALER
    session (HEP-CORE-0033 §11).  There is no code-evaluation path, by
    design — arbitrary code execution is not an admin method.
  * The client must be a CURVE client that pins the hub's public key, and
    must establish a session once with the admin token; every later message
    carries a sealed session id instead of the token.
  * The token is NOT something a human types.  It lives in the hub's
    encrypted vault, and the operator's credential is the vault password —
    the tool is expected to unlock the vault and move the token itself.  A
    `--token` command-line option is the wrong shape for the current design.
  * Administration is local-machine only, and the hub now refuses a
    non-loopback `admin.endpoint` at startup (§11.1).

The working reference implementation of the current wire protocol is the
C++ `AdminWireClient` used by the test suite.

Usage
-----
    python3 hubshell_client.py [options]

Options
-------
    --endpoint ENDPOINT   Admin shell endpoint (default: tcp://127.0.0.1:5600)
    --token TOKEN         Pre-shared token (required if hub is configured with one)
    --exec CODE           Execute a single snippet and exit (non-interactive)
    --file FILE           Execute a Python file inside the hub and exit

Quick examples
--------------
    # Open interactive REPL
    python3 hubshell_client.py

    # Query hub name
    python3 hubshell_client.py --exec "print(pylabhub.config()['name'])"

    # List active channels
    python3 hubshell_client.py --exec "
    for ch in pylabhub.channels('all'):
        print(ch['name'], '—', ch['consumer_count'], 'consumers, status:', ch['status'])
    "

    # Show full config
    python3 hubshell_client.py --exec "import json; print(json.dumps(pylabhub.config(), indent=2))"

    # Graceful hub shutdown
    python3 hubshell_client.py --exec "pylabhub.shutdown()"

Protocol
--------
Request  (JSON):  {"token": "...", "code": "python_source"}
Response (JSON):  {"success": bool, "output": str, "error": str}
"""

import argparse
import json
import sys

try:
    import zmq
except ImportError:
    print("ERROR: pyzmq not installed. Run: pip install pyzmq", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# HubClient
# ---------------------------------------------------------------------------

class HubClient:
    """Thin ZMQ REQ client for the hubshell admin shell."""

    def __init__(self, endpoint: str, token: str = ""):
        self._endpoint = endpoint
        self._token = token
        self._ctx = zmq.Context.instance()
        self._sock = self._ctx.socket(zmq.REQ)
        self._sock.setsockopt(zmq.RCVTIMEO, 10_000)   # 10s receive timeout
        self._sock.setsockopt(zmq.SNDTIMEO, 5_000)    # 5s send timeout
        self._sock.setsockopt(zmq.LINGER, 0)           # don't block on close
        self._sock.connect(endpoint)

    def exec(self, code: str) -> dict:
        """
        Send Python code to the hub for execution.

        Returns a dict with keys:
            success (bool), output (str), error (str)
        """
        payload = {"code": code}
        if self._token:
            payload["token"] = self._token

        self._sock.send_string(json.dumps(payload))
        raw = self._sock.recv_string()
        return json.loads(raw)

    def close(self):
        self._sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


# ---------------------------------------------------------------------------
# Output formatting
# ---------------------------------------------------------------------------

def _print_result(result: dict):
    if result.get("output"):
        print(result["output"], end="")
    if not result["success"]:
        print(f"\033[91mError: {result['error']}\033[0m", file=sys.stderr)


# ---------------------------------------------------------------------------
# Built-in helper commands (typed at the REPL prompt)
# ---------------------------------------------------------------------------

HELP_TEXT = """\
pyLabHub admin shell — connected to {endpoint}

Built-in pylabhub module commands:
  pylabhub.config()          — flat dict with all hub settings + paths
  pylabhub.channels()        — channel dict by status (ready/pending/closing/all)
  pylabhub.channels('ready') — shortcut for ready channels only
  pylabhub.reset()           — clear interpreter namespace
  pylabhub.shutdown()        — graceful hub shutdown

REPL shortcuts:
  :channels    — print channel table
  :config      — print hub config
  :help        — this message
  :quit / EOF  — exit the client (hub keeps running)
"""

CHANNELS_CODE = """\
channels = pylabhub.channels('all')
if not channels:
    print("(no active channels)")
else:
    fmt = "{:<30} {:>6}  {:>8}  {:<12}"
    print(fmt.format("channel", "consum.", "pid", "status"))
    print("-" * 62)
    for ch in channels:
        print(fmt.format(ch['name'][:29], ch['consumer_count'],
                         ch['producer_pid'], ch['status']))
"""

CONFIG_CODE = """\
import json
print(json.dumps(pylabhub.config(), indent=2))
"""


# ---------------------------------------------------------------------------
# Interactive REPL
# ---------------------------------------------------------------------------

def _repl(client: HubClient, endpoint: str):
    print(f"pyLabHub admin shell ({endpoint})")
    print("Type Python code, :help for commands, or Ctrl-D to quit.\n")

    buf = []
    while True:
        prompt = "... " if buf else ">>> "
        try:
            line = input(prompt)
        except EOFError:
            print()
            break
        except KeyboardInterrupt:
            print()
            buf.clear()
            continue

        # Built-in shortcuts
        stripped = line.strip()
        if not buf:
            if stripped == ":help":
                print(HELP_TEXT.format(endpoint=endpoint))
                continue
            if stripped == ":channels":
                line = CHANNELS_CODE
                buf = []
            elif stripped == ":config":
                line = CONFIG_CODE
                buf = []
            elif stripped in (":quit", ":exit", ":q"):
                break
            else:
                buf.append(line)
                # Check if the line ends a block (simple heuristic)
                if line and not line.endswith(":") and not line.startswith(" "):
                    code = "\n".join(buf)
                    buf.clear()
                    try:
                        result = client.exec(code)
                        _print_result(result)
                    except zmq.ZMQError as e:
                        print(f"\033[91mConnection error: {e}\033[0m", file=sys.stderr)
                continue

        if stripped in (":channels", ":config"):
            # Already replaced line; fall through to send
            code = line
        else:
            buf.append(line)
            # Blank line or non-indented line terminates a block
            if not line or (buf and not line[0].isspace() and len(buf) > 1):
                code = "\n".join(buf)
                buf.clear()
            else:
                continue

        try:
            result = client.exec(code)
            _print_result(result)
        except zmq.ZMQError as e:
            print(f"\033[91mConnection error: {e}\033[0m", file=sys.stderr)
            break


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="pyLabHub admin client",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--endpoint", default="tcp://127.0.0.1:5600",
        help="Admin shell ZMQ endpoint (default: tcp://127.0.0.1:5600)",
    )
    parser.add_argument(
        "--token", default="",
        help="Pre-shared authentication token (leave empty if not configured)",
    )
    parser.add_argument(
        "--exec", metavar="CODE",
        help="Execute a single Python snippet and exit",
    )
    parser.add_argument(
        "--file", metavar="FILE",
        help="Execute a Python file inside the hub and exit",
    )
    args = parser.parse_args()

    with HubClient(args.endpoint, args.token) as client:
        try:
            if args.exec:
                result = client.exec(args.exec)
                _print_result(result)
                sys.exit(0 if result["success"] else 1)

            if args.file:
                with open(args.file) as f:
                    code = f.read()
                result = client.exec(code)
                _print_result(result)
                sys.exit(0 if result["success"] else 1)

            _repl(client, args.endpoint)

        except zmq.ZMQError as e:
            print(f"Connection error: {e}", file=sys.stderr)
            print(f"Is pylabhub-hubshell running and listening on {args.endpoint}?",
                  file=sys.stderr)
            sys.exit(1)


if __name__ == "__main__":
    main()
