CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -flto=auto -DNDEBUG -Wall -Wextra -fopenmp
LDFLAGS ?= -fopenmp
EXEEXT := $(if $(findstring mingw,$(shell $(CXX) -dumpmachine)),.exe,)
TARGET = minivectordb$(EXEEXT)
include src/sources.mk
SOURCES = src/main.cpp $(MVD_LIBRARY_SOURCES)
HEADERS = $(wildcard src/*.h src/*/*.h)
all: $(TARGET)
$(TARGET): $(SOURCES) $(HEADERS) src/sources.mk
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)
print-sources:
	@echo $(SOURCES)
clean:
	$(RM) minivectordb minivectordb.exe
.PHONY: all print-sources clean
