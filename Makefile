CXX     = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -g
TARGET  = scheduler

.PHONY: all clean

all: $(TARGET)

$(TARGET): scheduler.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
