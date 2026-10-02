CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion -MMD -MP \
          -isystem vendor/cjson -isystem vendor/sqlite
LDFLAGS =
LDLIBS  =

SRC = $(wildcard src/*.c)

# `make` builds ./nylm (optimised); `make debug` builds ./nylm-debug
# (sanitizers + symbols). Each has its own object directory.
REL_OBJ = $(SRC:src/%.c=build/release/%.o)
DBG_OBJ = $(SRC:src/%.c=build/debug/%.o)
SAN     = -fsanitize=address,undefined

# Vendored code: upstream's own warnings are not ours to fix, so it gets
# plain flags and is built once for both variants.
VENDOR_OBJ = build/vendor/cJSON.o
VENDOR_CFLAGS = -O2 -w

release: nylm
debug: nylm-debug

nylm: $(REL_OBJ) $(VENDOR_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

nylm-debug: $(DBG_OBJ) $(VENDOR_OBJ)
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

run: nylm-debug
	./nylm-debug

clean:
	rm -rf build nylm nylm-debug

.PHONY: release debug run clean

-include $(REL_OBJ:.o=.d) $(DBG_OBJ:.o=.d)
