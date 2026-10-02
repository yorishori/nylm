CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion -MMD -MP
LDFLAGS =

SRC = $(wildcard src/*.c)

# `make` builds ./nylm (optimised); `make debug` builds ./nylm-debug
# (sanitizers + symbols). Each has its own object directory.
REL_OBJ = $(SRC:src/%.c=build/release/%.o)
DBG_OBJ = $(SRC:src/%.c=build/debug/%.o)
SAN     = -fsanitize=address,undefined

release: nylm
debug: nylm-debug

nylm: $(REL_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

nylm-debug: $(DBG_OBJ)
	$(CC) $(LDFLAGS) $(SAN) -o $@ $^

build/release/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -O2 -DNDEBUG -c -o $@ $<

build/debug/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -O0 -g $(SAN) -c -o $@ $<

run: nylm-debug
	./nylm-debug

clean:
	rm -rf build nylm nylm-debug

.PHONY: release debug run clean

-include $(REL_OBJ:.o=.d) $(DBG_OBJ:.o=.d)
