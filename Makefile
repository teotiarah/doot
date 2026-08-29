# doot -- build.
#
# GNU Make for development; a single amalgamated .c for distribution (D045).
# `make unity` proves that `cc build/doot.c` alone builds the project, which is
# the tested form of D035. Run `make help` for the target list.

CC      ?= cc
AR      ?= ar

STD      := -std=c99 -pedantic
WARN     := -Wall -Wextra -Werror \
            -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes \
            -Wold-style-definition -Wvla -Wcast-qual -Wwrite-strings -Wpointer-arith \
            -Wredundant-decls -Wswitch-enum -Wundef -Wdouble-promotion -Wformat=2 \
            -Wstrict-overflow=2
# NDEBUG is never defined: assertions are always on (D048).
COMMON   := $(STD) $(WARN) -MMD -MP
LDLIBS   :=

REV      := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
DEFS     := -DDOOT_BUILD_REV=\"$(REV)\"

DEBUG_FLAGS := -g3 -O0
REL_FLAGS   := -O2 -g1
SAN_FLAGS   := -g3 -O1 -fno-omit-frame-pointer \
               -fsanitize=address,undefined -fno-sanitize-recover=all

PROFILE ?= debug
ifeq ($(PROFILE),release)
  PROFILE_FLAGS := $(REL_FLAGS)
else ifeq ($(PROFILE),asan)
  PROFILE_FLAGS := $(SAN_FLAGS)
else
  PROFILE_FLAGS := $(DEBUG_FLAGS)
endif

OUT      := build/$(PROFILE)
CFLAGS   := $(COMMON) $(PROFILE_FLAGS) $(DEFS)

LIB_SRCS  := $(wildcard src/base/*.c)
CLI_SRCS  := $(wildcard src/cli/*.c)
TEST_SRCS := $(wildcard tests/unit/*.c)

LIB_OBJS  := $(LIB_SRCS:%.c=$(OUT)/%.o)
CLI_OBJS  := $(CLI_SRCS:%.c=$(OUT)/%.o)
TEST_OBJS := $(TEST_SRCS:%.c=$(OUT)/%.o)

DOOT      := $(OUT)/doot
DOOT_TEST := $(OUT)/doot_test

FUZZ_SRCS    := $(wildcard fuzz/fuzz_*.c)
FUZZ_TARGETS := $(patsubst fuzz/%.c,build/fuzz/%,$(FUZZ_SRCS))
FUZZ_CC      ?= clang
FUZZ_FLAGS   := $(STD) -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all

FMT_FILES := $(LIB_SRCS) $(CLI_SRCS) $(TEST_SRCS) $(FUZZ_SRCS) \
             $(wildcard src/base/*.h) $(wildcard src/cli/*.h) $(wildcard tests/unit/*.h)

.PHONY: all release debug asan test test-asan unity fuzz fuzz-smoke fmt fmt-check tidy \
        docs check clean help

all: $(DOOT)

debug:
	@$(MAKE) --no-print-directory PROFILE=debug all

release:
	@$(MAKE) --no-print-directory PROFILE=release all

# The target paths are spelled out rather than using $(DOOT_TEST): that variable
# expands in the outer make, where PROFILE is still the default.
asan:
	@$(MAKE) --no-print-directory PROFILE=asan build/asan/doot build/asan/doot_test

$(DOOT): $(LIB_OBJS) $(CLI_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(DOOT_TEST): $(LIB_OBJS) $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- tests ---------------------------------------------------------------

test: $(DOOT_TEST)
	$(DOOT_TEST) $(FILTER)

test-asan:
	@$(MAKE) --no-print-directory PROFILE=asan build/asan/doot_test
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
	UBSAN_OPTIONS=print_stacktrace=1 build/asan/doot_test $(FILTER)

# ---- amalgamation --------------------------------------------------------
# The gate that keeps D035 true: one translation unit, one command, no make.

unity:
	@tools/amalgamate.sh build/doot.c
	$(CC) $(STD) -O2 -o build/doot-unity build/doot.c $(LDLIBS)
	@build/doot-unity --version >/dev/null
	@echo "unity build ok: cc build/doot.c -> build/doot-unity"

# ---- fuzzing -------------------------------------------------------------

fuzz: $(FUZZ_TARGETS)

build/fuzz/%: fuzz/%.c $(LIB_SRCS)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_FLAGS) -o $@ $< $(LIB_SRCS)

# Short run per target plus every committed regression input. Long runs are a
# nightly job, not a per-commit gate.
FUZZ_SECONDS ?= 60
fuzz-smoke: fuzz
	@for f in $(FUZZ_TARGETS); do \
	  name=$$(basename $$f); \
	  echo "== $$name ($(FUZZ_SECONDS)s)"; \
	  mkdir -p build/corpus/$$name; \
	  cp -n fuzz/corpus/$$name/* build/corpus/$$name/ 2>/dev/null || true; \
	  $$f build/corpus/$$name -max_total_time=$(FUZZ_SECONDS) -print_final_stats=1 || exit 1; \
	  regs=$$(find fuzz/regressions/$$name -type f ! -name '.*' 2>/dev/null); \
	  if [ -n "$$regs" ]; then \
	    echo "== $$name regressions"; \
	    $$f $$regs || exit 1; \
	  fi; \
	done

# ---- style and docs ------------------------------------------------------

fmt:
	clang-format -i $(FMT_FILES)

fmt-check:
	clang-format --dry-run --Werror $(FMT_FILES)

tidy:
	clang-tidy $(LIB_SRCS) $(CLI_SRCS) -- $(STD) $(DEFS)

docs:
	tools/check-docs.sh

# Everything CI runs, in the order that fails fastest.
check: fmt-check docs all test unity test-asan
	@echo "all checks passed"

clean:
	rm -rf build

help:
	@echo "doot build targets"
	@echo "  make               debug build            -> build/debug/doot"
	@echo "  make release       optimized build        -> build/release/doot"
	@echo "  make test          unit tests             (FILTER=name to narrow)"
	@echo "  make test-asan     unit tests under ASan + UBSan + LSan"
	@echo "  make unity         amalgamate, then build it with \$$(CC) alone"
	@echo "  make fuzz          build libFuzzer targets"
	@echo "  make fuzz-smoke    short fuzz run + committed regressions"
	@echo "  make fmt           format sources"
	@echo "  make fmt-check     verify formatting"
	@echo "  make tidy          clang-tidy"
	@echo "  make docs          check documentation cross-references"
	@echo "  make check         everything CI runs"
	@echo "  make clean         remove build/"

-include $(LIB_OBJS:.o=.d) $(CLI_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
