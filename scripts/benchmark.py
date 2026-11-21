"""Time a complete local engine invocation with a fresh output directory per run."""
import argparse
from pathlib import Path
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("data", type=Path)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--work-dir", type=Path, default=Path.cwd())
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    program, data = args.executable.resolve(strict=True), args.data.resolve(strict=True)
    seconds = []
    for i in range(args.runs):
        with tempfile.TemporaryDirectory(dir=args.work_dir) as output:
            start = time.perf_counter()
            subprocess.run([program, data, output], check=True)
            seconds.append(time.perf_counter() - start)
        print(f"Run {i + 1}: {seconds[-1]:.4f} s", flush=True)
    print(f"Median {statistics.median(seconds):.4f} s")


if __name__ == "__main__":
    main()
