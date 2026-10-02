CXX ?= g++
CXXFLAGS ?= -std=c++17 -Ilib/gs_core -Ilib/gs_modbus -Ilib/gs_util -Ilib/gs_bus -Ilib/gs_led -Wall -Wextra -O1 -g -fsanitize=address,undefined

.PHONY: test clean
test: build/test_gs_core build/test_gs_modbus build/test_gs_util build/test_gs_bus build/test_gs_led
	./build/test_gs_core test/native/fixtures/incident_2026-10-01.csv
	./build/test_gs_modbus
	./build/test_gs_util
	./build/test_gs_bus
	./build/test_gs_led

build/test_gs_core: test/native/test_gs_core.cpp lib/gs_core/gs_core.cpp lib/gs_core/gs_core.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ test/native/test_gs_core.cpp lib/gs_core/gs_core.cpp

build/test_gs_modbus: test/native/test_gs_modbus.cpp lib/gs_modbus/gs_modbus.cpp lib/gs_modbus/gs_modbus.h lib/gs_core/gs_core.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ test/native/test_gs_modbus.cpp lib/gs_modbus/gs_modbus.cpp lib/gs_core/gs_core.cpp

build/test_gs_util: test/native/test_gs_util.cpp lib/gs_util/gs_util.cpp lib/gs_util/gs_util.h lib/gs_bus/gs_bus.cpp lib/gs_modbus/gs_modbus.cpp lib/gs_core/gs_core.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ test/native/test_gs_util.cpp lib/gs_util/gs_util.cpp lib/gs_bus/gs_bus.cpp lib/gs_modbus/gs_modbus.cpp lib/gs_core/gs_core.cpp

build/test_gs_bus: test/native/test_gs_bus.cpp lib/gs_bus/gs_bus.cpp lib/gs_bus/gs_bus.h lib/gs_modbus/gs_modbus.cpp lib/gs_core/gs_core.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ test/native/test_gs_bus.cpp lib/gs_bus/gs_bus.cpp lib/gs_modbus/gs_modbus.cpp lib/gs_core/gs_core.cpp

build/test_gs_led: test/native/test_gs_led.cpp lib/gs_led/gs_led.cpp lib/gs_led/gs_led.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ test/native/test_gs_led.cpp lib/gs_led/gs_led.cpp

clean:
	rm -rf build
