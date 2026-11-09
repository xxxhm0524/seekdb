CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -DNDEBUG -Wall -Wextra -fopenmp
LDFLAGS ?= -fopenmp
TARGET = minivectordb
SOURCES = src/main.cpp src/sql.cpp src/engine.cpp

all: $(TARGET)

$(TARGET): $(SOURCES) src/common.h src/sql.h src/engine.h
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
