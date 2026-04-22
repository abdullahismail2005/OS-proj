CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

# Uncomment the one LIBS line that matches your GUI choice:
# LIBS = -lsfml-graphics -lsfml-window -lsfml-audio -lsfml-network -lsfml-system -lrt
# LIBS = $(shell sdl2-config --libs) -lrt
# LIBS = -lglfw -lGL -lrt
LIBS = -lncurses -lrt

# NOTE: The per-process folders are named arbiter/ hip/ asp/, which collide
# with the binary names the provided template wanted to emit at repo root.
# To satisfy both "source folder named arbiter/" and a working build we emit
# binaries under build/ — the daily workflow becomes:
#     make
#     ./build/arbiter & ./build/hip & ./build/asp &
TARGETS = build/arbiter build/hip build/asp

all: clean $(TARGETS)
	@echo Build complete.

build:
	mkdir -p build

build/arbiter: arbiter/arbiter.cpp | build
	$(CXX) $(CXXFLAGS) arbiter/*.cpp -o $@ $(LIBS)

build/hip: hip/hip.cpp | build
	$(CXX) $(CXXFLAGS) hip/*.cpp -o $@ $(LIBS)

build/asp: asp/asp.cpp | build
	$(CXX) $(CXXFLAGS) asp/*.cpp -o $@ $(LIBS)

clean:
	rm -rf build

.PHONY: all clean
