TARGET = fastblur
CC = gcc
CFLAGS = -Wall -O3 -std=c11 #-DMEASURE_PERF_ENABLE
LDFLAGS = -lm
BIN = bin
SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:src/%.c=$(BIN)/%.o)
HDRS = $(wildcard src/*.h)

DBG = dbg
DBG_TARGET := $(DBG)/$(TARGET)
DBG_BIN := $(DBG)/$(BIN)
DBG_CFLAGS = -O0 -ggdb -DDEBUG

.PHONY: default all clean debug

default: $(TARGET)
all: default

$(BIN)/%.o: src/%.c $(HDRS) Makefile | $(BIN)
	$(CC) $(CFLAGS) -c $< -o $@

$(DBG):
$(BIN):
	mkdir -p $@

$(TARGET): $(OBJS)
	$(CC) $^ $(CFLAGS) $(LDFLAGS) -o $@

clean:
	-rm -f $(BIN)/*.o
	-rm -f $(TARGET)
	-rm -f $(DBG_BIN)/*.o
	-rm -f $(DBG_TARGET)

$(DBG_TARGET): $(SRCS) $(HDRS)| $(DBG)
	$(MAKE) $(MAKEFILE) TARGET="$(DBG_TARGET)" \
		BIN="$(DBG_BIN)" CFLAGS="$(CFLAGS) $(DBG_CFLAGS)"

debug: $(DBG_TARGET)
	gdb ./$<
