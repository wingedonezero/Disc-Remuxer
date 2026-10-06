# disc-remuxer: one build for everything.
#
#   make            = make debug
#   make debug      build every library and program into build/debug/,
#                   the finished files into dist/debug/
#   make release    the same into build/release/ and dist/release/
#   make test       the debug build, then every test (tests/, pytest);
#                   PYTEST_ARGS="-k vc1" passes arguments to pytest
#   make clean      remove build/ and dist/
#
# Every library is built from libs/ and linked statically, so dist/<mode>/
# holds programs that run on their own. Nothing installed on the host is
# picked up (FFmpeg's autodetection is off; every library is ours).
#
# Needs: a C compiler, make, nasm, pkg-config, cmake, rsync; for the tests
# python3 with venv and its headers (pip fetches the test tools once).

MODE ?= debug

.PHONY: all debug release test clean
all: debug

debug:
	@$(MAKE) --no-print-directory MODE=debug dist
release:
	@$(MAKE) --no-print-directory MODE=release dist
test:
	@$(MAKE) --no-print-directory MODE=debug check
clean:
	rm -rf build dist

ifneq ($(filter dist libs check,$(MAKECMDGOALS)),)
include mk/build.mk
endif
