# MCP Core - FreeBSD BSD make build
# Run from repository root with: make -C core [target]

ROOT=${.CURDIR:H}
BUILD=${ROOT}/build
OBJDIR=${BUILD}/obj/core
TEST_BINDIR=${BUILD}/tests
TEST_WORKDIR=${BUILD}/test-work
FIXTUREDIR=${TEST_WORKDIR}/plugins

CXX=clang++
CC=clang
AR=ar

CURL_CFLAGS!=pkg-config --cflags libcurl
CURL_LIBS!=pkg-config --libs libcurl
JSON_CFLAGS!=pkg-config --cflags json-c
JSON_LIBS!=pkg-config --libs json-c

CPPFLAGS=-I${.CURDIR}/include ${CURL_CFLAGS} ${JSON_CFLAGS}
CXXFLAGS=-std=c++17 -O2 -Wall -Wextra -Werror -pthread
CFLAGS=-std=c11 -O2 -Wall -Wextra -Werror
LDLIBS=${CURL_LIBS} ${JSON_LIBS} -pthread

CORE_SOURCES= \
	src/agent_bus.cpp \
	src/agent_protocol.cpp \
	src/agent_registry.cpp \
	src/agent_rpc.cpp \
	src/authorization.cpp \
	src/ipc_client.cpp \
	src/ipc_transport.cpp \
	src/json_rpc.cpp \
	src/llama_client.cpp \
	src/llama_planner.cpp \
	src/nakama_agent_transport.cpp \
	src/orchestrator.cpp \
	src/plan_submitter.cpp \
	src/plan_validator.cpp \
	src/plugin_manager.cpp \
	src/request_handler.cpp \
	src/stdio_bridge.cpp \
	src/stdio_transport.cpp

CORE_OBJECTS= \
	${OBJDIR}/agent_bus.o \
	${OBJDIR}/agent_protocol.o \
	${OBJDIR}/agent_registry.o \
	${OBJDIR}/agent_rpc.o \
	${OBJDIR}/authorization.o \
	${OBJDIR}/ipc_client.o \
	${OBJDIR}/ipc_transport.o \
	${OBJDIR}/json_rpc.o \
	${OBJDIR}/llama_client.o \
	${OBJDIR}/llama_planner.o \
	${OBJDIR}/nakama_agent_transport.o \
	${OBJDIR}/orchestrator.o \
	${OBJDIR}/plan_submitter.o \
	${OBJDIR}/plan_validator.o \
	${OBJDIR}/plugin_manager.o \
	${OBJDIR}/request_handler.o \
	${OBJDIR}/stdio_bridge.o \
	${OBJDIR}/stdio_transport.o

CORE_LIB=${BUILD}/lib/libmcp_core.a
CORE_BIN=${BUILD}/bin/mcp-core

TEST_SOURCES= \
	tests/authorization_test.cpp \
	tests/core_integration_test.cpp \
	tests/ipc_client_test.cpp \
	tests/ipc_transport_test.cpp \
	tests/json_rpc_test.cpp \
	tests/llama_client_test.cpp \
	tests/llama_planner_test.cpp \
	tests/nakama_agent_transport_test.cpp \
	tests/orchestrator_test.cpp \
	tests/plan_validator_test.cpp \
	tests/plugin_abi_layout_test.cpp \
	tests/plugin_manager_async_v2_backpressure_test.cpp \
	tests/plugin_manager_async_v2_concurrency_test.cpp \
	tests/plugin_manager_async_v2_concurrent_shutdown_test.cpp \
	tests/plugin_manager_async_v2_handler_drain_test.cpp \
	tests/plugin_manager_async_v2_shutdown_drain_test.cpp \
	tests/plugin_manager_async_v2_test.cpp \
	tests/plugin_manager_async_v2_unauthorized_test.cpp \
	tests/plugin_manager_async_v2_worker_shutdown_test.cpp \
	tests/plugin_manager_test.cpp \
	tests/request_handler_test.cpp \
	tests/stdio_bridge_test.cpp \
	tests/stdio_transport_test.cpp

TEST_BINS= \
	${TEST_BINDIR}/authorization_test \
	${TEST_BINDIR}/core_integration_test \
	${TEST_BINDIR}/ipc_client_test \
	${TEST_BINDIR}/ipc_transport_test \
	${TEST_BINDIR}/json_rpc_test \
	${TEST_BINDIR}/llama_client_test \
	${TEST_BINDIR}/llama_planner_test \
	${TEST_BINDIR}/nakama_agent_transport_test \
	${TEST_BINDIR}/orchestrator_test \
	${TEST_BINDIR}/plan_validator_test \
	${TEST_BINDIR}/plugin_abi_layout_test \
	${TEST_BINDIR}/plugin_manager_async_v2_backpressure_test \
	${TEST_BINDIR}/plugin_manager_async_v2_concurrency_test \
	${TEST_BINDIR}/plugin_manager_async_v2_concurrent_shutdown_test \
	${TEST_BINDIR}/plugin_manager_async_v2_handler_drain_test \
	${TEST_BINDIR}/plugin_manager_async_v2_shutdown_drain_test \
	${TEST_BINDIR}/plugin_manager_async_v2_test \
	${TEST_BINDIR}/plugin_manager_async_v2_unauthorized_test \
	${TEST_BINDIR}/plugin_manager_async_v2_worker_shutdown_test \
	${TEST_BINDIR}/plugin_manager_test \
	${TEST_BINDIR}/request_handler_test \
	${TEST_BINDIR}/stdio_bridge_test \
	${TEST_BINDIR}/stdio_transport_test \
	${TEST_BINDIR}/plugin_abi_c_test

.PHONY: all test test-agent test-nakama test-plugins test-binaries fixtures clean info

all: ${CORE_BIN}

${OBJDIR}/agent_bus.o: src/agent_bus.cpp
	@mkdir -p ${OBJDIR}
	${CXX} ${CPPFLAGS} ${CXXFLAGS} -c ${.ALLSRC} -o ${.TARGET}

.for name in agent_protocol agent_registry agent_rpc authorization ipc_client ipc_transport json_rpc llama_client llama_planner nakama_agent_transport orchestrator plan_submitter plan_validator plugin_manager request_handler stdio_bridge stdio_transport
${OBJDIR}/${name}.o: src/${name}.cpp
	@mkdir -p ${OBJDIR}
	${CXX} ${CPPFLAGS} ${CXXFLAGS} -c ${.ALLSRC} -o ${.TARGET}
.endfor

${CORE_LIB}: ${CORE_OBJECTS}
	@mkdir -p ${.TARGET:H}
	rm -f ${.TARGET}
	${AR} rcs ${.TARGET} ${CORE_OBJECTS}

${CORE_BIN}: src/main.cpp ${CORE_LIB}
	@mkdir -p ${.TARGET:H}
	${CXX} ${CPPFLAGS} ${CXXFLAGS} ${.CURDIR}/src/main.cpp ${CORE_LIB} ${LDLIBS} -o ${.TARGET}

.for name in authorization core_integration ipc_client ipc_transport json_rpc llama_client llama_planner nakama_agent_transport orchestrator plan_validator plugin_abi_layout plugin_manager_async_v2_backpressure plugin_manager_async_v2_concurrency plugin_manager_async_v2_concurrent_shutdown plugin_manager_async_v2_handler_drain plugin_manager_async_v2_shutdown_drain plugin_manager_async_v2_unauthorized plugin_manager_async_v2_worker_shutdown plugin_manager request_handler stdio_bridge stdio_transport
${TEST_BINDIR}/${name}_test: tests/${name}_test.cpp ${CORE_LIB}
	@mkdir -p ${TEST_BINDIR}
	${CXX} ${CPPFLAGS} ${CXXFLAGS} ${.CURDIR}/tests/${name}_test.cpp ${CORE_LIB} ${LDLIBS} -o ${.TARGET}
.endfor

${TEST_BINDIR}/plugin_manager_async_v2_test: tests/plugin_manager_async_v2_test.cpp ${CORE_LIB}
	@mkdir -p ${TEST_BINDIR}
	${CXX} ${CPPFLAGS} ${CXXFLAGS} ${.CURDIR}/tests/plugin_manager_async_v2_test.cpp ${CORE_LIB} ${LDLIBS} -o ${.TARGET}

# The C ABI test is compiled as C, then linked with the C++ driver.
${TEST_BINDIR}/plugin_abi_c_test.o: tests/plugin_abi_c_test.c
	@mkdir -p ${TEST_BINDIR}
	${CC} -I${.CURDIR}/include ${CFLAGS} -c ${.ALLSRC} -o ${.TARGET}

${TEST_BINDIR}/plugin_abi_c_test: ${TEST_BINDIR}/plugin_abi_c_test.o ${CORE_LIB}
	${CXX} ${CXXFLAGS} ${.ALLSRC} ${LDLIBS} -o ${.TARGET}

# Test plugins are staged only under build/. Existing source-tree fixtures
# are deliberately left untouched.
${TEST_WORKDIR}/core/tests/plugins/libmcp_test.so: tests/test_plugin.cpp
	@mkdir -p ${.TARGET:H}
	${CXX} -I${.CURDIR}/include ${CXXFLAGS} -fPIC -shared ${.ALLSRC} -o ${.TARGET}

${FIXTUREDIR}/async-v2/libmcp_async_v2_test.so: tests/async_v2_test_plugin.cpp
	@mkdir -p ${.TARGET:H}
	${CXX} -I${.CURDIR}/include ${CXXFLAGS} -fPIC -shared ${.ALLSRC} -o ${.TARGET}

# Deliberately unauthorized plugin: exercises authenticated publish denial.
${FIXTUREDIR}/unauthorized/libmcp_async_v2_unauthorized.so: tests/async_v2_test_plugin.cpp
	@mkdir -p ${.TARGET:H}
	${CXX} -I${.CURDIR}/include ${CXXFLAGS} -DTEST_EXPECT_UNAUTHORIZED '-DTEST_PLUGIN_ID="unauthorized"' -fPIC -shared ${.ALLSRC} -o ${.TARGET}

${FIXTUREDIR}/backpressure/libmcp_async_v2_backpressure.so: tests/async_v2_backpressure_test_plugin.cpp
	@mkdir -p ${.TARGET:H}
	${CXX} -I${.CURDIR}/include ${CXXFLAGS} -fPIC -shared ${.ALLSRC} -o ${.TARGET}

${FIXTUREDIR}/concurrency/libmcp_async_v2_concurrency.so: tests/async_v2_concurrency_test_plugin.cpp
	@mkdir -p ${.TARGET:H}
	${CXX} -I${.CURDIR}/include ${CXXFLAGS} -fPIC -shared ${.ALLSRC} -o ${.TARGET}

${FIXTUREDIR}/nakama/libmcp_nakama_transport_test.so: tests/nakama_transport_test_plugin.cpp
	@mkdir -p ${.TARGET:H}
	${CXX} -I${.CURDIR}/include ${CXXFLAGS} -fPIC -shared ${.ALLSRC} -o ${.TARGET}

fixtures: \
	${TEST_WORKDIR}/core/tests/plugins/libmcp_test.so \
	${FIXTUREDIR}/async-v2/libmcp_async_v2_test.so \
	${FIXTUREDIR}/backpressure/libmcp_async_v2_backpressure.so \
	${FIXTUREDIR}/concurrency/libmcp_async_v2_concurrency.so \
        ${FIXTUREDIR}/unauthorized/libmcp_async_v2_unauthorized.so \
	${FIXTUREDIR}/nakama/libmcp_nakama_transport_test.so

test-binaries: ${TEST_BINS}

test-agent: test-binaries
	@set -e; cd ${TEST_WORKDIR}; \
	${TEST_BINDIR}/orchestrator_test; \
	${TEST_BINDIR}/llama_planner_test; \
	${TEST_BINDIR}/plan_validator_test

test-nakama: ${TEST_BINDIR}/nakama_agent_transport_test ${FIXTUREDIR}/nakama/libmcp_nakama_transport_test.so
	@cd ${TEST_WORKDIR} && ${TEST_BINDIR}/nakama_agent_transport_test ${FIXTUREDIR}/nakama

test-plugins: test-binaries fixtures
	@set -e; cd ${TEST_WORKDIR}; \
	${TEST_BINDIR}/plugin_abi_c_test; \
	${TEST_BINDIR}/plugin_abi_layout_test; \
	${TEST_BINDIR}/plugin_manager_test; \
	${TEST_BINDIR}/plugin_manager_async_v2_test ${FIXTUREDIR}/async-v2; \
	${TEST_BINDIR}/plugin_manager_async_v2_backpressure_test ${FIXTUREDIR}/backpressure; \
	${TEST_BINDIR}/plugin_manager_async_v2_concurrency_test ${FIXTUREDIR}/concurrency; \
	${TEST_BINDIR}/plugin_manager_async_v2_concurrent_shutdown_test ${FIXTUREDIR}/async-v2; \
	${TEST_BINDIR}/plugin_manager_async_v2_handler_drain_test ${FIXTUREDIR}/concurrency; \
	${TEST_BINDIR}/plugin_manager_async_v2_shutdown_drain_test ${FIXTUREDIR}/concurrency; \
	${TEST_BINDIR}/plugin_manager_async_v2_unauthorized_test ${FIXTUREDIR}/unauthorized; \
	${TEST_BINDIR}/plugin_manager_async_v2_worker_shutdown_test ${FIXTUREDIR}/async-v2

test: test-binaries fixtures
	@set -e; \
	for t in ${TEST_BINS}; do \
		name=$$(basename "$$t"); \
		args=""; \
		case "$$name" in \
			nakama_agent_transport_test) args="${FIXTUREDIR}/nakama" ;; \
			plugin_manager_async_v2_test|plugin_manager_async_v2_concurrent_shutdown_test|plugin_manager_async_v2_worker_shutdown_test) args="${FIXTUREDIR}/async-v2" ;; \
                    plugin_manager_async_v2_unauthorized_test) args="${FIXTUREDIR}/unauthorized" ;; \
                    plugin_manager_async_v2_shutdown_drain_test) args="${FIXTUREDIR}/concurrency" ;; \
                    plugin_manager_async_v2_handler_drain_test) args="${FIXTUREDIR}/concurrency" ;; \
			plugin_manager_async_v2_backpressure_test) args="${FIXTUREDIR}/backpressure" ;; \
			plugin_manager_async_v2_concurrency_test) args="${FIXTUREDIR}/concurrency" ;; \
		esac; \
		echo "==> $$name"; \
		(cd ${TEST_WORKDIR} && "$$t" $$args); \
	done

info:
	@echo "Compiler: $$(${CXX} --version | head -n 1)"
	@echo "Core sources: ${CORE_SOURCES}"
	@echo "Test executables: ${TEST_BINS}"
	@echo "Build directory: ${BUILD}"

clean:
	rm -rf ${BUILD}
