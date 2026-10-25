CC := gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -Og -g
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

TARGET := build/server
SOURCES := src/server.c

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SOURCES) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build:
	mkdir -p $@

run: $(TARGET)
	./$(TARGET)

clean:
	$(RM) $(TARGET)
