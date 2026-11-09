CXXFLAGS ?= -O3 -std=c++20 -Wall -Wextra -Wpedantic -ffp-contract=off

.PHONY: all test clean
all: build/minivectordb

build/minivectordb: src/main.cpp src/database.hpp src/store.hpp src/sql.hpp src/distance.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) src/main.cpp -o $@

test: build/minivectordb
	python3 -m unittest discover -s tests -v

clean:
	rm -rf build
