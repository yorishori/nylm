CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion -MMD -MP
LDFLAGS =
# System libraries (Arch packages: sqlite, cjson, openssl, taglib, libjpeg-turbo,
# libpng).
LDLIBS  = -lsqlite3 -lcjson -lcrypto -ltag_c -ljpeg -lpng16
# nylm-qobuz (the Qobuz service) also talks HTTPS; the server never does.
SSLLIBS = -lssl

# The Qobuz service's own files: only in nylm-qobuz, never in the server.
QOBUZ_SRC = src/https.c src/qobuz_service.c src/qobuz_main.c
SRC       = $(filter-out $(QOBUZ_SRC),$(wildcard src/*.c))

# `make` builds ./nylm and ./nylm-qobuz (optimised); `make debug` builds
# ./nylm-debug and ./nylm-qobuz-debug (sanitizers + symbols). Each has its
# own object directory.
REL_OBJ  = $(SRC:src/%.c=build/release/%.o)
DBG_OBJ  = $(SRC:src/%.c=build/debug/%.o)
QREL_OBJ = $(filter-out build/release/main.o,$(REL_OBJ)) $(QOBUZ_SRC:src/%.c=build/release/%.o)
QDBG_OBJ = $(filter-out build/debug/main.o,$(DBG_OBJ)) $(QOBUZ_SRC:src/%.c=build/debug/%.o)
SAN      = -fsanitize=address,undefined

release: nylm nylm-qobuz
debug: nylm-debug nylm-qobuz-debug

nylm: $(REL_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

nylm-debug: $(DBG_OBJ)
	$(CC) $(LDFLAGS) $(SAN) -o $@ $^ $(LDLIBS)

nylm-qobuz: $(QREL_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(SSLLIBS)

nylm-qobuz-debug: $(QDBG_OBJ)
	$(CC) $(LDFLAGS) $(SAN) -o $@ $^ $(LDLIBS) $(SSLLIBS)

build/release/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -O2 -DNDEBUG -c -o $@ $<

build/debug/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -O0 -g $(SAN) -c -o $@ $<

# Unit tests link every debug object except the two main()s.
TEST_SRC = $(wildcard tests/test_*.c)
TEST_BIN = $(TEST_SRC:tests/%.c=build/tests/%)
TEST_OBJ = $(filter-out build/debug/qobuz_main.o,$(QDBG_OBJ))

build/tests/%: tests/%.c tests/test.h $(TEST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -O0 -g $(SAN) -o $@ $< $(TEST_OBJ) $(LDLIBS) $(SSLLIBS)

test: nylm-debug nylm-qobuz-debug $(TEST_BIN)
	@for t in $(TEST_BIN); do $$t || exit 1; done
	tests/smoke.sh ./nylm-debug

# Development server on http://127.0.0.1:8080, data in ./dev-data.
# First time: NYLM_DATA=dev-data ./nylm-debug set-password
run: nylm-debug
	mkdir -p dev-data
	NYLM_DATA=dev-data ./nylm-debug

clean:
	rm -rf build nylm nylm-debug nylm-qobuz nylm-qobuz-debug

.PHONY: release debug run clean test

-include $(QREL_OBJ:.o=.d) $(QDBG_OBJ:.o=.d) build/release/main.d build/debug/main.d
