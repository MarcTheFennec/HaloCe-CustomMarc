# The host-side tests of the libsm64 module.
#
#   make            builds and runs every test against the best library it
#                   finds (a real libsm64, else the test double)
#   make double     builds the test double only
#   make test       builds and runs the tests against the test double
#   make test-real  builds and runs them against a real libsm64
#                   (LIBSM64_DIST=path to a directory holding libsm64.so)
#   make clean
#
# Everything here is plain C99 and needs nothing but a C compiler: the point
# is that the module's own logic (conversions, clock, input mapping, collision
# conversion, actor life cycle) is testable on any machine, with or without
# libsm64 and with or without an SM64 ROM.
#
# CC0 1.0 Universal, like the rest of the port.

CC      ?= cc
CFLAGS  ?= -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=

CORE    := ../src/sm64_port_core.c
INCLUDE := -I../include -I.

# A real libsm64, when one was built: the directory holding libsm64.so.
LIBSM64_DIST ?= ../../../build/linux

TESTS   := test_sm64_port
DOUBLE  := libsm64_test_double.so

.PHONY: all test test-real double clean

all: double
	$(MAKE) test-real || $(MAKE) test

double: $(DOUBLE)

$(DOUBLE): sm64_test_double.c libsm64.h
	$(CC) $(CFLAGS) -fPIC -shared -DSM64_LIB_EXPORT -o $@ $< $(INCLUDE) -lm

test_sm64_port: test_sm64_port.c sm64_test_blob.c $(CORE) ../include/sm64_port.h ../include/sm64_port_abi.h
	$(CC) $(CFLAGS) -o $@ test_sm64_port.c sm64_test_blob.c $(CORE) $(INCLUDE) -lm $(LDFLAGS)

# The module mirrors libsm64's structures; if these compile, the mirror and
# the real header agree on every field the module touches.
test_abi: test_abi.c ../include/sm64_port_abi.h ../include/sm64_port.h libsm64.h
	$(CC) $(CFLAGS) -o $@ test_abi.c $(INCLUDE)

check-header:
	@if [ -f ../../third_party/libsm64/src/libsm64.h ]; then \
		if diff -u ../../third_party/libsm64/src/libsm64.h libsm64.h > /dev/null; then \
			echo "tests/libsm64.h matches the vendored libsm64's"; \
		else \
			echo "WARNING: tests/libsm64.h differs from port/third_party/libsm64/src/libsm64.h"; \
			diff -u ../../third_party/libsm64/src/libsm64.h libsm64.h | head -40; \
			exit 1; \
		fi; \
	else \
		echo "port/third_party/libsm64 is not checked out: skipping the header comparison"; \
	fi

test: test_sm64_port double test_abi
	./test_sm64_port
	./test_abi
	$(MAKE) check-header

# Runs against a real libsm64 if there is one in $(LIBSM64_DIST).
test-real: test_sm64_port
	@if [ -f "$(LIBSM64_DIST)/libsm64.so" ]; then \
		SM64_PORT_TEST_LIBRARY="$(LIBSM64_DIST)/libsm64.so" ./test_sm64_port; \
	else \
		echo "no $(LIBSM64_DIST)/libsm64.so: skipping the real-library run"; \
		exit 1; \
	fi

clean:
	rm -f $(TESTS) test_abi $(DOUBLE) *.o
