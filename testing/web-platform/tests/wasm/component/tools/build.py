#!/usr/bin/env python3
"""Compile every .wat fixture into a sibling .wasm using wasm-tools.

Usage: build.py [--check] [--wasm-tools PATH]

The wasm-tools binary is taken from --wasm-tools, then the WASM_TOOLS
environment variable, then PATH.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOTS = [
    os.path.join(HERE, "jsapi", "components"),
    os.path.join(HERE, "esm-integration", "resources"),
]


def find_wat():
    for root in ROOTS:
        for dirpath, _, filenames in os.walk(root):
            for name in sorted(filenames):
                if name.endswith(".wat"):
                    yield os.path.normpath(os.path.join(dirpath, name))


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def build(wasm_tools, wat, check):
    wasm = wat[: -len(".wat")] + ".wasm"
    if check and os.path.exists(wasm) and os.path.getmtime(wasm) >= os.path.getmtime(wat):
        return True
    result = run([wasm_tools, "parse", wat, "-o", wasm])
    if result.returncode != 0:
        print(f"parse failed: {os.path.relpath(wat, HERE)}\n{result.stderr}", file=sys.stderr)
        return False
    result = run([wasm_tools, "validate", "--features", "all", wasm])
    if result.returncode != 0:
        print(f"validate failed: {os.path.relpath(wat, HERE)}\n{result.stderr}", file=sys.stderr)
        os.remove(wasm)
        return False
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="only rebuild stale outputs")
    parser.add_argument("--wasm-tools", default=os.environ.get("WASM_TOOLS", "wasm-tools"))
    args = parser.parse_args()

    ok = True
    built = 0
    for wat in find_wat():
        if build(args.wasm_tools, wat, args.check):
            built += 1
        else:
            ok = False
    print(f"{built} built")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
