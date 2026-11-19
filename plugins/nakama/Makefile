CXX=clang++

CURL_CFLAGS!=pkg-config --cflags libcurl
CURL_LIBS!=pkg-config --libs libcurl
JSON_CFLAGS!=pkg-config --cflags json-c
JSON_LIBS!=pkg-config --libs json-c

CXXFLAGS=-std=c++17 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Werror -pthread
CPPFLAGS=-Iinclude -I../../core/include ${CURL_CFLAGS}

LDFLAGS=-shared -pthread
LDLIBS=${CURL_LIBS}

TRANSPORT_TARGET=libmcp_nakama_transport.so
PLUGIN_TARGET=libmcp_nakama.so

TRANSPORT_SOURCES=src/websocket_transport.cpp
PLUGIN_SOURCES=src/nakama_plugin.cpp src/websocket_transport.cpp src/http_auth.cpp src/receive_dispatcher.cpp
HEADERS=include/nakama/websocket_transport.hpp include/nakama/http_auth.hpp include/nakama/receive_dispatcher.hpp

all: ${TRANSPORT_TARGET} ${PLUGIN_TARGET}

${TRANSPORT_TARGET}: ${TRANSPORT_SOURCES} ${HEADERS} Makefile
	${CXX} ${CPPFLAGS} ${CXXFLAGS} ${LDFLAGS} -o ${.TARGET} ${TRANSPORT_SOURCES} ${LDLIBS}

${PLUGIN_TARGET}: ${PLUGIN_SOURCES} ${HEADERS} ../../core/include/mcp/plugin_abi.h Makefile
	${CXX} ${CPPFLAGS} ${JSON_CFLAGS} ${CXXFLAGS} ${LDFLAGS} -o ${.TARGET} ${PLUGIN_SOURCES} ${LDLIBS} ${JSON_LIBS}

clean:
	rm -f ${TRANSPORT_TARGET} ${PLUGIN_TARGET}

.PHONY: all clean
