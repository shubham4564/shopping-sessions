#!/bin/sh
# Download the published checker, build the public dev set, and compare both
# Parquet tables after running the image under the starter's restrictions.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
base=https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files
for name in starter-duckdb generator dev-answer; do
    curl --fail --silent --show-error --location --retry 2 "$base/$name.zip" -o "$name.zip"
done
sha256sum -c <<'SUMS'
b84ff8726c43749043c33c250548a9ac9987b9132d28ac818a47515bd128f77c  starter-duckdb.zip
13724cadbac5a99d847e27cea911cd2e338ff972b46c2039da87452011bb101e  generator.zip
7eb8e1f0b1f431d178d28ac9437ce85809c44eb6393fdd42fb59857f30834fa9  dev-answer.zip
SUMS
for name in starter-duckdb generator dev-answer; do unzip -q "$name.zip"; done
export IMAGE="${IMAGE:-shopping-sessions:latest}" PLATFORM=linux/amd64
available=$(nproc)
if [ "$available" -gt 8 ]; then available=8; fi
export CPUS="${CPUS:-$available}" MEM="${MEM:-24g}"
docker build --platform "$PLATFORM" -t "$IMAGE" "$repo"
docker image inspect --format 'Image {{.Os}}/{{.Architecture}}, {{.Size}} bytes' "$IMAGE"
runner="$work/starter-duckdb/scripts/docker_run.sh"
python3 starter-duckdb/scripts/run_fixtures.py starter-duckdb/fixtures -- "$runner"
python3 "$repo/tests/boundaries.py" "$runner"
python3 "$repo/tests/dense.py" "$runner"
cargo build --locked --release --manifest-path generator/Cargo.toml
generator/target/release/generate --seed 1 --events 1000000 --clicks 250000 --output dev
"$runner" dev out
make -C "$repo" check-dev OUT="$work/out" STARTER="$work/starter-duckdb" ANSWER="$work/dev-answer"
