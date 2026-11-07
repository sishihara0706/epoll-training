CC := gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -Og -g

CPPFLAGS += -Iinclude
LOG_LEVEL ?= LOG_INFO
CPPFLAGS += -DLOG_LEVEL=$(LOG_LEVEL)

LDFLAGS ?=
LDLIBS ?=

TARGET := build/server
SANITIZE_TARGET := build/server-sanitize
SOURCES := src/server.c
IDLE_TEST_TARGET := build/server-idle-test
IDLE_TEST_SANITIZE_TARGET := build/server-idle-test-sanitize
IDLE_TEST_SECONDS := 4

SANITIZE_FLAGS := -fsanitize=address,undefined \
				  -fno-omit-frame-pointer \
				  -fno-sanitize-recover=undefined

.PHONY: all run sanitize run-sanitize idle-test-build idle-test-sanitize-build clean

all: $(TARGET)

$(TARGET): $(SOURCES) include/log.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)


sanitize: $(SANITIZE_TARGET)

$(SANITIZE_TARGET): $(SOURCES) include/log.h | build
	$(CC) $(CPPFLAGS) $(SANITIZE_FLAGS) \
		$(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

idle-test-build: $(IDLE_TEST_TARGET)

idle-test-sanitize-build: $(IDLE_TEST_SANITIZE_TARGET)

$(IDLE_TEST_TARGET): $(SOURCES) include/log.h | build
	$(CC) $(CPPFLAGS) -DIDLE_TIMEOUT_SECONDS=$(IDLE_TEST_SECONDS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

$(IDLE_TEST_SANITIZE_TARGET): $(SOURCES) include/log.h | build
	$(CC) $(CPPFLAGS) -DIDLE_TIMEOUT_SECONDS=$(IDLE_TEST_SECONDS) $(SANITIZE_FLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

build:
	mkdir -p $@

run: $(TARGET)
	./$(TARGET)

run-sanitize: sanitize
	./$(SANITIZE_TARGET)

clean:
	$(RM) $(TARGET) $(SANITIZE_TARGET) $(IDLE_TEST_TARGET) $(IDLE_TEST_SANITIZE_TARGET)
