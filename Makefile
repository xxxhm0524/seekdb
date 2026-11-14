CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -ffp-contract=off
CPPFLAGS ?= -Isrc -Ithird_party
EXEEXT := $(if $(findstring mingw,$(shell $(CXX) -dumpmachine)),.exe,)
SOURCES := $(wildcard src/*.cpp)
HEADERS := $(wildcard src/*.hpp third_party/rapidjson/*.h third_party/rapidjson/internal/*.h third_party/rapidjson/error/*.h third_party/rapidjson/msinttypes/*.h) third_party/json.hpp
all: minivectordb$(EXEEXT)
minivectordb$(EXEEXT): $(SOURCES) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) -o $@ $(LDFLAGS)
clean:
	$(RM) minivectordb minivectordb.exe
.PHONY: all clean
