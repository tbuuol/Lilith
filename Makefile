# Compiler
CXX = g++

# Flags
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -pthread

# Target
TARGET = Lilith

# Sources
SRC := $(wildcard Code/*.cpp)

# Linux build (default)
all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lcurl -lssl -lcrypto

# Clean build artifacts
clean:
	rm -f $(TARGET) *.o
