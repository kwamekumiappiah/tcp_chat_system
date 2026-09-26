CC       := gcc
CFLAGS   := -Wall -Wextra -Iinclude -pthread
SRC      := src/main.c src/server.c
BUILD_DIR:= build
TARGET   := $(BUILD_DIR)/server
PORT     ?= 8080

.PHONY: all build run test clean

all: build

build: $(TARGET)

$(TARGET): $(SRC)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

run: build
	./$(TARGET) $(PORT)

test: build
	python3 test/test_chat_server.py

clean:
	rm -rf $(BUILD_DIR)