CC := gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -Og -g
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

TARGET := server
SOURCES := server.c

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	$(RM) $(TARGET)
