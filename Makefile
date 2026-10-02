CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion -MMD -MP \
          -isystem vendor/cjson -isystem vendor/sqlite
LDFLAGS =
LDLIBS  = -lcrypto -lm

SRC = $(wildcard src/*.c)

# `make` builds ./nylm (optimised); `make debug` builds ./nylm-debug
# (sanitizers + symbols). Each has its own object directory.
REL_OBJ = $(SRC:src/%.c=build/release/%.o)
DBG_OBJ = $(SRC:src/%.c=build/debug/%.o)
SAN     = -fsanitize=address,undefined

# Vendored code: upstream's own warnings are not ours to fix, so it gets
# plain flags and is built once for both variants.
VENDOR_OBJ = build/vendor/cJSON.o build/vendor/sqlite3.o
VENDOR_CFLAGS = -O2 -w
SQLITE_FLAGS = -DSQLITE_THREADSAFE=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION \
               -DSQLITE_DEFAULT_FOREIGN_KEYS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
               -DSQLITE_OMIT_DEPRECATED -DSQLITE_LIKE_DOESNT_MATCH_BLOBS

# migrations/*.sql are compiled into the binary as strings.
MIGRATIONS = $(sort $(wildcard migrations/*.sql))
GEN_OBJ    = build/gen/migrations.o

release: nylm
debug: nylm-debug

nylm: $(REL_OBJ) $(VENDOR_OBJ) $(GEN_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

nylm-debug: $(DBG_OBJ) $(VENDOR_OBJ) $(GEN_OBJ)
	$(CC) $(LDFLAGS) $(SAN) -o $@ $^ $(LDLIBS)

build/release/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -O2 -DNDEBUG -c -o $@ $<

build/debug/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -O0 -g $(SAN) -c -o $@ $<

build/vendor/cJSON.o: vendor/cjson/cJSON.c
	@mkdir -p $(@D)
	$(CC) $(VENDOR_CFLAGS) -c -o $@ $<

build/vendor/sqlite3.o: vendor/sqlite/sqlite3.c
	@mkdir -p $(@D)
	$(CC) $(VENDOR_CFLAGS) $(SQLITE_FLAGS) -c -o $@ $<

build/gen/migrations.c: $(MIGRATIONS) tools/embed-migrations.sh
	@mkdir -p $(@D)
	tools/embed-migrations.sh $@ $(MIGRATIONS)

build/gen/migrations.o: build/gen/migrations.c
	$(CC) -std=c11 -c -o $@ $<

# Unit tests link every debug object except main.o.
TEST_SRC = $(wildcard tests/test_*.c)
TEST_BIN = $(TEST_SRC:tests/%.c=build/tests/%)
TEST_OBJ = $(filter-out build/debug/main.o,$(DBG_OBJ)) $(VENDOR_OBJ) $(GEN_OBJ)

build/tests/%: tests/%.c tests/test.h $(TEST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -O0 -g $(SAN) -o $@ $< $(TEST_OBJ) $(LDLIBS)

test: nylm-debug $(TEST_BIN)
	@for t in $(TEST_BIN); do $$t || exit 1; done
	tests/smoke.sh ./nylm-debug

# Development server on http://127.0.0.1:8080.
run: nylm-debug
	./nylm-debug

clean:
	rm -rf build nylm nylm-debug

.PHONY: release debug run clean test

-include $(REL_OBJ:.o=.d) $(DBG_OBJ:.o=.d)
