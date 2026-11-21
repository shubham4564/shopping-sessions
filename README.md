# Shopping Sessions

A C++20 implementation of the Databricks Shopping Sessions buildathon. It reads the mounted CSV files and manifest and writes eight Parquet parts in `user_daily/` and one in `campaign_daily/`. Both tables retain their schema on empty inputs.

## Build and run

From the repository root on a Docker host, set `DATA` to the directory containing `manifest.json`. Make the output and scratch directories writable by UID 1000.

```sh
docker build --platform linux/amd64 -t shopping-sessions .
DATA=/absolute/path/to/data
mkdir -p out scratch
docker run --rm --platform linux/amd64 --network none --read-only \
  --tmpfs /tmp:rw,size=64m,mode=1777 --cap-drop ALL \
  --security-opt no-new-privileges --pids-limit 4096 \
  --cpus 8 --memory 24g --memory-swap 24g --user 1000:1000 \
  -v "$DATA:/data:ro" -v "$PWD/out:/output" -v "$PWD/scratch:/scratch" \
  shopping-sessions
```

The entry point checks CPU features at runtime. It selects a baseline, AVX2, or AVX-512 implementation and uses vendor-specific instruction scheduling on AVX-512 CPUs with VBMI2. It honors `DATA_DIR`, `OUTPUT_DIR`, and `NCPU`. Set `SHOP_TIMING=1` for cumulative stage times on stderr.

## Verify the tables

On a Linux Docker host with Rust 1.85+, Python, make, curl, and unzip, run the following. The script verifies archive checksums, checks all 36 official fixtures, exercises additional boundary cases, and compares both output tables with the published seed-1 answer.

```sh
python3 -m pip install polars==1.32.3
scripts/verify-container.sh
```

For a local build, install GCC 14+, make, and the LZ4 runtime (`liblz4-1` on Debian). Set the paths to the unzipped starter, generated data, and official answer.

```sh
make -j2 portable
make fixtures STARTER=../starter-duckdb
python3 tests/boundaries.py build/portable/shopping-sessions
python3 tests/dense.py build/portable/shopping-sessions
build/portable/shopping-sessions ../dev ../out
make check-dev STARTER=../starter-duckdb OUT=../out ANSWER=../dev-answer
```

## Measure elapsed time

Generate a practice-sized dataset with the official generator. The benchmark launches a fresh process for each run, includes Parquet writes and process exit, and uses a fresh output directory each time. Repeated runs reuse the input files and the host filesystem cache. CPU model, CPU placement, and storage affect elapsed time.

```sh
generate --seed 1837 --events 20000000 --clicks 5000000 --output ../bench20
NCPU=8 taskset -c 0-7 python3 scripts/benchmark.py build/portable/shopping-sessions ../bench20 --runs 7
```

## Pipeline

1. A disjoint-set forest joins IDs across all link files. Each connected component keeps its byte-wise smallest name. Events can be decoded while the links are being joined.
2. Workers read memory-mapped CSV chunks. Vectorized scans check row boundaries, delimiter counts, and quoting. The scalar parser handles irregular blocks, arbitrary UTF-8, literal NUL, and oversized records. Both paths validate timestamps against the manifest.
3. A reversible 64-bit permutation partitions event IDs. Each partition compares complete keys and keeps the earliest ingestion time, including invalid winners. A replacement can retract a hit already assigned to another person.
4. Workers group surviving events by person. For busy shoppers, time bins shorter than 30 minutes preserve the minimum and maximum event times. Comparing the last event in one occupied bin with the first event in the next gives the exact session boundary. Each session and all its purchases belong to the UTC date of its first event.
5. A click covers a contiguous range of session starts. Campaign bitsets record distinct assists. The latest eligible click supplies credit, with byte-wise click ID ordering for ties. Identical click copies produce the same credit and set the same assist bits.
6. Workers write completed user rows to Parquet as they aggregate. The flat writer emits standard data pages, nullable definition levels, UTF-8 and DATE annotations, and LZ4 raw compression. Campaign totals are combined across workers before their table is written. Wider time windows and large dimension tables use a full-width sort-based implementation.

[Official rules, generator, starter, and dev answer](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions)
