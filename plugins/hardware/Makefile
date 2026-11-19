CXX=clang++

CXXFLAGS=-std=c++17 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Werror
CPPFLAGS=-I../../core/include

LDFLAGS=-shared
LDLIBS=-ldevinfo -lusb -lgeom

TARGET=libmcp_hardware.so
SOURCE=hardware_plugin.cpp

all: $(TARGET)

$(TARGET): $(SOURCE) ../../core/include/mcp/plugin_abi.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SOURCE) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
