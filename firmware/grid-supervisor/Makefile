CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined

.PHONY: test clean
test: build/test_gs_core
	./build/test_gs_core test/native/fixtures/incident_2026-10-01.csv

build/test_gs_core: test/native/test_gs_core.cpp lib/gs_core/gs_core.cpp lib/gs_core/gs_core.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ test/native/test_gs_core.cpp lib/gs_core/gs_core.cpp

clean:
	rm -rf build
