CXX ?= g++
CXXFLAGS ?= -O3 -DNDEBUG -flto
PYTHON ?= python3
IMAGE ?= shopping-sessions:latest
PLATFORM ?= linux/amd64
STARTER ?= ../starter-duckdb
FIXTURES ?= $(STARTER)/fixtures
DEV ?= dev
ANSWER ?= dev-answer
OUT ?= out

.PHONY: native portable build fixtures dev run-dev check-dev
HEADERS := $(wildcard src/*.hpp)
SOURCES := src/main.cpp src/output.cpp
COMPILE = $(CXX) $(CXXFLAGS) -std=c++20 -pthread -Wall -Wextra
LINK = -Wl,-rpath,'$$ORIGIN/lib' -l:liblz4.so.1

native: build/shopping-sessions
build/shopping-sessions: $(SOURCES) $(HEADERS)
	mkdir -p build
	$(COMPILE) -march=native $(SOURCES) $(LINK) -o $@

ARCH-base = -march=x86-64
ARCH-avx2 = -march=x86-64-v3
ARCH-avx512 = -march=x86-64-v4 -mtune=cascadelake
ARCH-vbmi2 = -march=x86-64-v4 -mavx512vbmi2 -mtune=icelake-server
ARCH-zen4 = -march=x86-64-v4 -mavx512vbmi2 -mtune=znver4
VARIANTS = base avx2 avx512 vbmi2 zen4
portable: build/portable/shopping-sessions $(addprefix build/portable/engine-,$(VARIANTS))

build/portable/engine-%: $(SOURCES) $(HEADERS)
	mkdir -p build/portable
	$(COMPILE) $(ARCH-$*) $(SOURCES) $(LINK) -o $@

build/portable/shopping-sessions: src/launcher.cpp
	mkdir -p build/portable
	$(CXX) -O2 -std=c++20 -march=x86-64 -Wall -Wextra $< -o $@

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
