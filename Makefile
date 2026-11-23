# MCP Core - native FreeBSD BSD make entry point

CORE_DIR=core
PLUGIN_DIRS=plugins/hardware plugins/system plugins/nakama

.PHONY: all core test test-agent test-nakama test-plugins plugins clean info

all: core

core:
	${MAKE} -C ${CORE_DIR} all

test:
	${MAKE} -C ${CORE_DIR} test

test-agent:
	${MAKE} -C ${CORE_DIR} test-agent

test-nakama:
	${MAKE} -C ${CORE_DIR} test-nakama

test-plugins:
	${MAKE} -C ${CORE_DIR} test-plugins

plugins:
	@set -e; \
	for d in ${PLUGIN_DIRS}; do \
		if [ -f "$$d/Makefile" ]; then \
			echo "==> Building $$d"; \
			${MAKE} -C "$$d"; \
		fi; \
	done

clean:
	${MAKE} -C ${CORE_DIR} clean

info:
	@echo "Platform: $$(uname -srm)"
	@echo "Make: BSD make"
	@echo "Compiler: $$(${CXX} --version 2>/dev/null | head -n 1)"
	@echo "Core sources: $$(find core/src -maxdepth 1 -name '*.cpp' | wc -l | tr -d ' ')"
	@echo "Plugin Makefiles: ${PLUGIN_DIRS}"
	@echo "Build output: build/"
