#!/usr/bin/env python3
"""Fail-fast preflight checker for local/prod build dependencies."""
from __future__ import annotations

import shutil
import subprocess
import sys
from dataclasses import dataclass


@dataclass
class Check:
    name: str
    ok: bool
    detail: str


def run(cmd: list[str]) -> tuple[bool, str]:
    try:
        res = subprocess.run(cmd, check=False, capture_output=True, text=True)
    except FileNotFoundError:
        return False, f"missing executable: {cmd[0]}"
    out = (res.stdout or "") + (res.stderr or "")
    return res.returncode == 0, out.strip()


def pkg_config_cflags(module: str) -> list[str]:
    ok, out = run(["pkg-config", "--cflags-only-I", module])
    if not ok or not out:
        return []
    return out.split()


def has_pkg_config(module: str) -> Check:
    ok, out = run(["pkg-config", "--exists", module])
    if ok:
        ok_v, ver = run(["pkg-config", "--modversion", module])
        return Check(module, ok_v, ver if ok_v else "present but version lookup failed")
    return Check(module, False, out or "pkg-config module not found")


def has_header(header: str, include_flags: list[str] | None = None) -> Check:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        return Check(header, False, "no C++ compiler found in PATH")
    src = f"#include <{header}>\nint main(){{return 0;}}\n"
    cmd = [compiler, "-x", "c++", "-std=c++17", "-"]
    if include_flags:
        cmd.extend(include_flags)
    cmd.append("-fsyntax-only")
    proc = subprocess.run(
        cmd,
        input=src,
        text=True,
        capture_output=True,
        check=False,
    )
    if proc.returncode == 0:
        return Check(header, True, "header found")
    return Check(header, False, (proc.stderr or proc.stdout).strip())


def main() -> int:
    checks = [
        Check("cmake", shutil.which("cmake") is not None, "binary in PATH" if shutil.which("cmake") else "missing"),
        Check("pkg-config", shutil.which("pkg-config") is not None, "binary in PATH" if shutil.which("pkg-config") else "missing"),
        has_pkg_config("libwebsockets"),
        has_pkg_config("libcurl"),
        has_header("nlohmann/json.hpp", include_flags=pkg_config_cflags("nlohmann_json")),
    ]

    failed = [c for c in checks if not c.ok]
    for c in checks:
        status = "OK" if c.ok else "FAIL"
        print(f"[{status}] {c.name}: {c.detail}")

    if failed:
        print("\nPreflight failed. Install missing dependencies before bootstrap/build.")
        print("Ubuntu example: sudo apt-get install -y cmake pkg-config libwebsockets-dev libcurl4-openssl-dev nlohmann-json3-dev")
        return 1

    print("\nPreflight passed. Environment is ready for configure/build/bootstrap.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
