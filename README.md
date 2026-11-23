# MCP Core

MCP Core is a modular MCP server and plugin host written in C++.

The project is developed for FreeBSD and provides a core runtime,
a plugin ABI, transport infrastructure, and independently built plugins.

## Project structure

- `core/include/mcp/` — public core interfaces and plugin ABI.
- `core/src/` — core implementation.
- `core/tests/` — core test sources.
- `plugins/hardware/` — hardware information plugin.
- `plugins/system/` — system information plugin.
- `plugins/nakama/` — Nakama integration and WebSocket transport.

## Plugin architecture

Plugins are implemented as shared libraries and use the public
interface defined in `core/include/mcp/plugin_abi.h`.

The ABI provides the compatibility boundary between the core runtime
and dynamically loaded plugins.

## Agent communication

The core includes components for agent communication:

- AgentBus
- AgentProtocol
- AgentRegistry
- AgentRPC
- AgentTransport

## Building on FreeBSD

The repository uses native BSD make. The root Makefile builds the Core
runtime and delegates plugin builds to their existing Makefiles.

Requirements:
- FreeBSD with BSD make
- Clang/LLVM with C++17 support
- `pkg-config`
- `libcurl` and `json-c` development files

Commands (run from the repository root):

```sh
make                 # Build Core: build/bin/mcp-core
make test            # Build and run the Core test suite
make test-agent      # Run orchestrator/planner/validator tests
make test-nakama     # Run the NakamaAgentTransport integration fixture
make test-plugins    # Run plugin ABI and PluginManager tests
make plugins         # Build existing hardware/system/Nakama plugins
make info            # Show compiler and build inventory
make clean           # Remove the generated build/ directory
```

Core objects, libraries, executables, test binaries and test-only plugin
fixtures are staged under `build/`. The test runner uses an isolated working
directory so tests that expect `core/tests/plugins` do not overwrite or
depend on generated files in the source tree.

Plugin Makefiles remain authoritative for their own dependencies and
outputs. Building a plugin does not install or deploy it. Deployment,
service restart and replacement of an installed plugin are separate,
explicit operations.

## Tests

C++ test sources are kept in the repository.

Compiled test binaries and generated test output are excluded from Git.

## Platform

Primary development platform: FreeBSD.

## Repository contents

This repository tracks source code, headers, Makefiles, tests, and
documentation. Locally compiled binaries and historical temporary
snapshots are not part of the initial source publication.
