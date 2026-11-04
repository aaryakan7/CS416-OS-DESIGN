"""Run each scenario in a fresh process, with a timeout for scheduler hangs."""
import pathlib
import subprocess
import sys

build = pathlib.Path(sys.argv[1])
for scenario in ("returns", "preemption", "mutex", "joins"):
    result = subprocess.run([str(build / "runtime"), scenario], timeout=30,
                            capture_output=True, text=True)
    if result.returncode or "PASS" not in result.stdout:
        sys.exit(f"FAIL {scenario}\n{result.stdout}{result.stderr}")
    print(f"PASS {scenario}")
subprocess.run([str(build / "policy")], check=True, timeout=10)
