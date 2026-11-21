CXX ?= g++
CXXFLAGS ?= -O3 -DNDEBUG
PYTHON ?= python3
ARROW_DIR ?= $(shell $(PYTHON) -c 'import importlib.util as u; from pathlib import Path; print(Path(u.find_spec("pyarrow").origin).parent)')
IMAGE ?= shopping-sessions:latest
PLATFORM ?= linux/amd64
STARTER ?= ../starter-duckdb
FIXTURES ?= $(STARTER)/fixtures
DEV ?= dev
ANSWER ?= dev-answer
OUT ?= out

.PHONY: native build fixtures dev run-dev check-dev
native: build/shopping-sessions

build/shopping-sessions: src/main.cpp src/output.cpp src/output.hpp src/input.hpp src/model.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -std=c++20 -pthread -Wall -Wextra -isystem "$(ARROW_DIR)/include" src/main.cpp src/output.cpp -L"$(ARROW_DIR)" -Wl,-rpath,'$$ORIGIN/lib' -l:libparquet.so.2000 -l:libarrow.so.2000 -o $@
	ln -sfn "$(abspath $(ARROW_DIR))" build/lib

build:
	docker buildx build --platform $(PLATFORM) --load -t $(IMAGE) .

fixtures: native
	$(PYTHON) "$(STARTER)/scripts/run_fixtures.py" "$(FIXTURES)" -- $(CURDIR)/build/shopping-sessions

dev:
	generate --seed 1 --events 1000000 --clicks 250000 --output "$(DEV)"

run-dev: native
	build/shopping-sessions "$(DEV)" "$(OUT)"

check-dev:
	$(PYTHON) "$(STARTER)/scripts/compare.py" "$(OUT)" "$(ANSWER)"
