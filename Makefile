CC := gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -Og -g
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

TARGET := build/server
SANITIZE_TARGET := build/server-sanitize
SOURCES := src/server.c

SANITIZE_FLAGS := -fsanitize=address,undefined \
				  -fno-omit-frame-pointer \
				  -fno-sanitize-recover=undefined

.PHONY: all run sanitize run-sanitize clean

all: $(TARGET)

$(TARGET): $(SOURCES) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

sanitize: $(SANITIZE_TARGET)

$(SANITIZE_TARGET): $(SOURCES) | build
	$(CC) $(CPPFLAGS) $(SANITIZE_FLAGS) \
		$(LDFLAGS) -o $@ $^ $(LDLIBS)

build:
	mkdir -p $@

run: $(TARGET)
	./$(TARGET)

run-sanitize: sanitize
	./$(SANITIZE_TARGET)

clean:
	$(RM) $(TARGET) $(SANITIZE_TARGET)
