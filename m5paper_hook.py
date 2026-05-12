#!/usr/bin/env python3
"""
m5paper_hook.py — Claude Code Stop hook.

Claude Code calls this with the stop event JSON on stdin.
Extracts the last assistant message and POSTs it to the Mac bridge,
which forwards it to the M5Paper.

Registered in ~/.claude/settings.json Stop hooks.
"""

import json
import sys
import urllib.request

BRIDGE_URL = "http://127.0.0.1:8764/forward"



def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        return

    text = data.get("last_assistant_message", "").strip()
    if not text:
        return

    try:
        req = urllib.request.Request(
            BRIDGE_URL,
            data=text.encode("utf-8"),
            headers={"Content-Type": "text/plain; charset=utf-8"},
            method="POST",
        )
        urllib.request.urlopen(req, timeout=3)
    except Exception as e:
        # Non-fatal: don't block Claude Code
        print(f"m5paper_hook: {e}", file=sys.stderr)


if __name__ == "__main__":
    main()
