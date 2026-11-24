"""Run the synthetic Windows PE compatibility suite against a built Leonida runtime.

Usage: python -I scripts/run_pe_compat_lab.py <runtime-build> [--ctest PATH]
Requires CTest 3.21+ on PATH or via --ctest. Build the RelWithDebInfo runtime
with Vulkan and Python enabled first. Only the named repository tests are run.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET


# CTest names in runtime/CMakeLists.txt. pe_loader uses tiny_dll and tiny_exe.
CONTRACTS = {
    "pe_loader": "PE headers, imports, exports, DLL deduplication and forced relocation",
    "run_tiny_bundled_load": "guest loads a bundled DLL",
    "loader_tls": "static TLS installation and callbacks",
    "dll_thread_calls": "per-thread DLL notifications",
    "run_startup_cmdline": "runtime initializes the guest command line",
    "xmem": "runtime memory allocation",
    "run_tiny_game_eh": "mapped guest unwinds a C++ exception",
    "run_tiny_frame": "guest frame through the Vulkan backend",
    "run_tiny_atexit": "guest shutdown callbacks",
    "run_tiny_game_exit_code": "guest exit status reaches the parent",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--suite", choices=("all", "cpu", "gpu"), default="all")
    args = parser.parse_args()
    contracts = {name: desc for name, desc in CONTRACTS.items() if
                 args.suite == "all" or (name == "run_tiny_frame") == (args.suite == "gpu")}
    if not (args.build / "CTestTestfile.cmake").is_file():
        parser.error("expected a configured runtime CTest build directory")
    pattern = "^(" + "|".join(map(re.escape, contracts)) + ")$"
    env = os.environ.copy()
    for key in ("LEONIDA_GAME_DIR", "LEONIDA_MOVIE"):
        env.pop(key, None)
    with tempfile.TemporaryDirectory() as scratch:
        report = Path(scratch) / "ctest.xml"
        result = subprocess.run(
            [args.ctest, "--test-dir", str(args.build), "-C", args.config,
             "-R", pattern, "-j", "1", "--timeout", "120", "--output-on-failure",
             "--output-junit", str(report)], env=env, check=False)
        if not report.is_file():
            return result.returncode or 1
        cases = ET.parse(report).findall(".//testcase")
    passed = set()
    for case in cases:
        name = case.get("name")
        if name in contracts and case.get("status") == "run" and all(
            case.find(tag) is None for tag in ("error", "failure", "skipped")
        ):
            passed.add(name)
    for name, contract in contracts.items():
        print(f"{'PASS' if name in passed else 'INCOMPLETE/FAIL'} {name} - {contract}")
    return result.returncode or int(passed != set(contracts))


if __name__ == "__main__":
    raise SystemExit(main())
