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

## Building

The plugins currently provide individual FreeBSD Makefiles.

The repository does not yet include a top-level build system for the
core runtime. Core build and test instructions will be documented
when that build process is consolidated.

Plugin build dependencies are defined by their respective Makefiles.

## Tests

C++ test sources are kept in the repository.

Compiled test binaries and generated test output are excluded from Git.

## Platform

Primary development platform: FreeBSD.

## Repository contents

This repository tracks source code, headers, Makefiles, tests, and
documentation. Locally compiled binaries and historical temporary
snapshots are not part of the initial source publication.
