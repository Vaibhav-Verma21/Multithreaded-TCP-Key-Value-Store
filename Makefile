CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -pthread -O2

# Platform detection: Link ws2_32 on Windows/MinGW, standard POSIX on Linux/WSL
ifeq ($(OS),Windows_NT)
    LDLIBS ?= -lws2_32
    EXE ?= .exe
    CLEAN_CMD = cmd /c "del /q $(TARGET) $(TEST_TARGET) $(LOAD_TARGET) *.o *.exe 2>NUL"
else
    LDLIBS ?=
    EXE ?=
    CLEAN_CMD = rm -f $(TARGET) $(TEST_TARGET) $(LOAD_TARGET) *.o *.exe
endif

SRC = src/lru_cache.cpp src/thread_pool.cpp src/server.cpp
TARGET = kvstore$(EXE)
TEST_TARGET = test_lru$(EXE)
LOAD_TARGET = load_test$(EXE)

.PHONY: all test load_test clean tsan asan

all: $(TARGET)

$(TARGET): $(SRC) src/main.cpp
	$(CXX) $(CXXFLAGS) $(SRC) src/main.cpp -o $(TARGET) $(LDLIBS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): src/lru_cache.cpp tests/test_lru_cache.cpp
	$(CXX) $(CXXFLAGS) src/lru_cache.cpp tests/test_lru_cache.cpp -o $(TEST_TARGET) $(LDLIBS)

load_test: $(LOAD_TARGET)

$(LOAD_TARGET): tests/load_test.cpp
	$(CXX) $(CXXFLAGS) tests/load_test.cpp -o $(LOAD_TARGET) $(LDLIBS)

tsan: CXXFLAGS += -fsanitize=thread -g
tsan: clean $(TARGET) $(TEST_TARGET)

asan: CXXFLAGS += -fsanitize=address,undefined -g
asan: clean $(TARGET) $(TEST_TARGET)

clean:
	-$(CLEAN_CMD)
