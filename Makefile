CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -DNDEBUG -Wall -Wextra -fopenmp
LDFLAGS ?= -fopenmp
TARGET = minivectordb
SOURCES = src/main.cpp src/sql.cpp src/engine.cpp

all: $(TARGET)

$(TARGET): $(SOURCES) src/common.h src/sql.h src/engine.h src/ann.h src/tx_spool.h src/page_image_codec.h src/compact_cache.h src/external_sort.h src/exact_l2.h
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
