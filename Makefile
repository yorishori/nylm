CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion -MMD -MP
LDFLAGS =
# System libraries (Arch packages: sqlite, cjson, openssl, taglib).
LDLIBS  = -lsqlite3 -lcjson -lcrypto -ltag_c

SRC = $(wildcard src/*.c)

# `make` builds ./nylm (optimised); `make debug` builds ./nylm-debug
# (sanitizers + symbols). Each has its own object directory.
REL_OBJ = $(SRC:src/%.c=build/release/%.o)
DBG_OBJ = $(SRC:src/%.c=build/debug/%.o)
SAN     = -fsanitize=address,undefined

release: nylm
debug: nylm-debug

nylm: $(REL_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

nylm-debug: $(DBG_OBJ)
	$(CC) $(LDFLAGS) $(SAN) -o $@ $^ $(LDLIBS)

build/release/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -O2 -DNDEBUG -c -o $@ $<

build/debug/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -O0 -g $(SAN) -c -o $@ $<

# Unit tests link every debug object except main.o.
TEST_SRC = $(wildcard tests/test_*.c)
TEST_BIN = $(TEST_SRC:tests/%.c=build/tests/%)
TEST_OBJ = $(filter-out build/debug/main.o,$(DBG_OBJ))

build/tests/%: tests/%.c tests/test.h $(TEST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -O0 -g $(SAN) -o $@ $< $(TEST_OBJ) $(LDLIBS)

test: nylm-debug $(TEST_BIN)
	@for t in $(TEST_BIN); do $$t || exit 1; done
	tests/smoke.sh ./nylm-debug

# Development server on http://127.0.0.1:8080, data in ./dev-data.
# First time: NYLM_DATA=dev-data ./nylm-debug set-password
run: nylm-debug
	mkdir -p dev-data
	NYLM_DATA=dev-data ./nylm-debug

clean:
	rm -rf build nylm nylm-debug

.PHONY: release debug run clean test

-include $(REL_OBJ:.o=.d) $(DBG_OBJ:.o=.d)
