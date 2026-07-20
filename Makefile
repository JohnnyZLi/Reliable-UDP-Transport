CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow
CPPFLAGS ?= -Isrc
LDFLAGS ?=

COMMON := src/protocol.cpp src/util.cpp
CLIENT := src/client.cpp $(COMMON)
SERVER := src/server.cpp $(COMMON)

.PHONY: all clean test

all: bin/myclient bin/myserver

bin:
	mkdir -p bin

bin/myclient: $(CLIENT) | bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CLIENT) $(LDFLAGS) -o $@

bin/myserver: $(SERVER) | bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SERVER) $(LDFLAGS) -o $@

clean:
	rm -f bin/myclient bin/myserver

test: all
	python3 tests/integration.py
