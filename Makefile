CC := gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -Og -g
CPPFLAGS ?= -Iinclude -DLOG_LEVEL=LOG_INFO
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

$(TARGET): $(SOURCES) include/log.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)


sanitize: $(SANITIZE_TARGET)

$(SANITIZE_TARGET): $(SOURCES) include/log.h | build
	$(CC) $(CPPFLAGS) $(SANITIZE_FLAGS) \
		$(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

build:
	mkdir -p $@

run: $(TARGET)
	./$(TARGET)

run-sanitize: sanitize
	./$(SANITIZE_TARGET)

clean:
	$(RM) $(TARGET) $(SANITIZE_TARGET)
