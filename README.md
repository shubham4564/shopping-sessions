# Shopping Sessions

A C++20 implementation of the Databricks Shopping Sessions buildathon. It reads the mounted CSVs and manifest and writes `user_daily/part-0.parquet` and `campaign_daily/part-0.parquet`. Both files include a Parquet DATE column and carry their full schema when empty.

## Build and run

From the repository root on a Docker host, set `DATA` to the directory containing `manifest.json`. The output and scratch directories must be writable by UID 1000.

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

The binary honors `DATA_DIR`, `OUTPUT_DIR`, and `NCPU`. Its default paths are `/data` and `/output`. It keeps its working data in memory. Set `SHOP_TIMING=1` to print cumulative stage times to stderr.

## Verify the submission

On a Linux Docker host with Rust 1.85+, Python, make, curl, and unzip, run the following. The script checks the downloaded archives, runs the container on all 36 official fixtures, generates the public seed-1 data, and runs `make check-dev` against both published answer tables.

```sh
python3 -m pip install polars==1.32.3
scripts/verify-container.sh
```

For native testing on x86-64 Linux, install GCC 12+, make, and the specified Arrow wheel. Put the unzipped starter, generated dev data, and dev answer outside the repository.

```sh
python3 -m pip install pyarrow==20.0.0 polars==1.32.3
make native
make fixtures STARTER=../starter-duckdb
python3 tests/boundaries.py build/shopping-sessions
make run-dev DEV=../dev OUT=../out
make check-dev STARTER=../starter-duckdb OUT=../out ANSWER=../dev-answer
```

The additional regression test covers records crossing megabyte boundaries, a 650-link chain, UTF-8 and NUL in IDs, mixed-length click-ID ties, and arbitrarily long decimal fields.

## How the pipeline works

1. A disjoint-set forest joins every valid link. Each component tracks its byte-wise smallest ID, including IDs seen only in links.
2. Workers read memory-mapped files in chunks and validate the LF-delimited CSV grammar. Each event ID goes to one deduplication partition. The earliest ingested candidate wins even when its event data is invalid.
3. Surviving events and valid clicks are partitioned by canonical person. Workers sort events by time, deduplicate clicks, and use descending byte order for click IDs tied at a timestamp.
4. Each session belongs to the UTC day of its first event. Two moving indices maintain the inclusive seven-day click window. The latest eligible click supplies the credit, and a count for each active campaign gives it one assist per session.
5. The writer merges per-worker campaign totals and emits both tables with Apache Arrow and Parquet. Short ASCII identifiers use a lossless inline representation. All other identifiers are interned by their complete bytes and use the same byte-wise tie rules.

[Official rules, starter, generator, and dev answer](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions)
