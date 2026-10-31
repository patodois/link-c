# libFuzzer harnesses for the parsing entry points of rewrite_link.h. Not part of the default build:
#
#   make -C fuzz              build every harness into fuzz/build/ (needs clang with the libFuzzer runtime)
#   make -C fuzz corpus       write the seed corpora from tests/vectors/link into fuzz/build/corpus/<target>/
#   make -C fuzz smoke        build, then run every harness over its seeds once (no mutation): a regression check
#   make -C fuzz clean
#
# Run one for a while, for example:   fuzz/build/fuzz_wire -max_total_time=1800 fuzz/build/corpus/fuzz_wire
# See README.md.

CLANG   ?= clang
OUT     := build
SAN     := -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
CFLAGS  ?= -std=gnu99 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function
TARGETS := fuzz_wire fuzz_control fuzz_endpoint fuzz_pixel fuzz_session fuzz_udp

all: $(addprefix $(OUT)/,$(TARGETS)) $(OUT)/verdict

$(OUT):
	mkdir -p $(OUT)

# the helper of check_corpus.py (a plain program, no libFuzzer)
$(OUT)/verdict: verdict.c ../rewrite_link.h fuzz_common.h | $(OUT)
	$(CLANG) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $< -o $@ -lm

# every harness includes the implementation (so it can reach the static functions) and fuzz_common.h
$(OUT)/%: %.c ../rewrite_link.h fuzz_common.h | $(OUT)
	$(CLANG) $(CFLAGS) $(SAN) -pthread $< -o $@ -lm

corpus:
	python3 make_corpus.py $(OUT)/corpus

smoke: all corpus
	@set -e; for t in $(TARGETS); do echo "== $$t"; $(OUT)/$$t -runs=0 -max_len=1100000 $(OUT)/corpus/$$t; done

clean:
	rm -rf $(CURDIR)/$(OUT)

.PHONY: all corpus smoke clean
