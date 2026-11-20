# MCP Core — TODO

> Project status: active development on FreeBSD.
> Last verified: 2026-11-20.
> Repository: https://github.com/aleksander-haugas/mcp-core

This file tracks implementation and validation work. A checked item means it has been implemented or explicitly verified; planned work remains unchecked until tested.

## Current verified baseline

- [x] MCP Core daemon is installed and running as the dedicated `mcp` user.
- [x] `warbotz` retains administrative access through the MCP socket during development.
- [x] Core loads the Hardware, System, and Nakama plugins in the current deployment.
- [x] Nakama configuration loader accepts the active configuration (`configuration_state=VALID`).
- [x] Nakama WebSocket connection and authentication succeed (`transport_state=CONNECTED`).
- [x] `nakama_ping` returns `pong: true`.
- [x] Nakama connection survives closure of the STDIO client that initiated it.
- [x] A separate STDIO client can query status and successfully ping over the existing connection.
- [x] Initial source repository published on GitHub; build artifacts and shared-library binaries are excluded from version control.
- [x] ABI 1.0 remains the compatibility baseline.

## 1. Nakama transport stability

- [ ] Run a sustained-connection test against the running daemon (multiple minutes initially).
- [ ] Run repeated status and ping checks from independent STDIO client sessions.
- [ ] Record latency, CID progression, errors, and any unexpected transport state changes.
- [ ] Verify behavior when the remote Nakama endpoint closes the WebSocket.
- [ ] Verify reconnect behavior after an intentional remote disconnect or temporary network interruption.
- [ ] Verify that repeated connect requests are safe and do not create duplicate WebSocket connections.
- [ ] Verify explicit disconnect behavior and confirm that a later connect can establish a fresh session.
- [ ] Add reproducible automated regression tests for the verified lifecycle.

## 2. Plugin ABI — asynchronous messages

- [ ] Finalize the optional `MCP_PLUGIN_EXT_ASYNC_MESSAGES` extension contract.
- [ ] Preserve ABI 1.0 behavior and existing plugin compatibility; do not require existing plugins to implement the extension.
- [ ] Define versioning, structure sizes, capability discovery, ownership, and lifetime rules for the extension.
- [ ] Define thread-safety and callback/reentrancy rules.
- [ ] Define message ownership, payload limits, delivery/error semantics, and shutdown behavior.
- [ ] Add host-side extension discovery and validation in Core.
- [ ] Add plugin-side opt-in support in Nakama.
- [ ] Add ABI compatibility tests covering legacy plugins that do not expose the extension.
- [ ] Add tests for malformed, unsupported, and truncated extension structures.

## 3. NakamaAgentTransport ↔ AgentBus

- [ ] Implement the internal asynchronous message channel between Core and plugins.
- [ ] Connect NakamaAgentTransport to AgentBus through that channel.
- [ ] Keep AgentBus independent of MCP tool registration and tool execution.
- [ ] Reuse the existing Nakama WebSocket connection; do not create a second WebSocket connection for AgentBus.
- [ ] Define inbound and outbound message routing, correlation, and error propagation.
- [ ] Define backpressure and bounded-queue behavior so a slow consumer cannot exhaust memory.
- [ ] Ensure message delivery does not block the Core request/response path.
- [ ] Add integration tests for plugin-to-Core and Core-to-plugin messages.
- [ ] Test concurrent messages, ordering guarantees (where required), malformed payloads, and consumer failures.
- [ ] Verify that ordinary MCP tool calls continue to work while asynchronous messages are flowing.

## 4. Plugin lifecycle and concurrency safety

- [ ] Audit plugin callback lifetimes and prevent callbacks into unloaded plugin code.
- [ ] Audit synchronization around Nakama transport state and asynchronous message dispatch.
- [ ] Define behavior for plugin errors and transport failures without taking down the Core daemon.
- [ ] Verify that plugin shutdown stops new asynchronous deliveries and safely releases plugin-owned resources.
- [ ] Add stress tests for concurrent tool calls, status checks, ping operations, and asynchronous message traffic.

## 5. Build, installation, and deployment

- [ ] Design and document a reproducible top-level build workflow for Core and selected plugins.
- [ ] Document per-component build prerequisites and FreeBSD-specific dependencies.
- [ ] Define an explicit, auditable plugin installation/deployment procedure.
- [ ] Ensure deployed plugin libraries correspond to the source revision being tested.
- [ ] Document service configuration, runtime user/group, socket permissions, plugin directory permissions, and persistent configuration paths.
- [ ] Add a safe deployment/rollback procedure; do not replace a loaded plugin library in place.
- [ ] Document configuration validation without exposing credentials or secret values.
- [ ] Add build and test instructions to the README once the build workflow is established.

## 6. Graceful shutdown — later phase

This is intentionally deferred until asynchronous messaging and plugin lifecycle rules are stable.

- [ ] Stop accepting new operations.
- [ ] Wait for active tool calls and asynchronous callbacks to finish, with a defined timeout policy.
- [ ] Stop new plugin message delivery and drain or cancel queued messages according to documented policy.
- [ ] Invoke plugin shutdown callbacks in a safe order.
- [ ] Unload plugin libraries only after no calls or callbacks can reference their code.
- [ ] Close IPC resources and release daemon-owned resources.
- [ ] Add shutdown race, timeout, and failure-path tests.

## 7. Tests and release hygiene

- [ ] Keep development tests and test plugins while implementation is in progress.
- [ ] Ensure tests cover both the legacy ABI 1.0 path and the optional asynchronous extension.
- [ ] Run the complete regression suite on the target FreeBSD environment.
- [ ] Review logs and verify no secrets are emitted.
- [ ] Remove temporary test artifacts, test binaries, and test-only plugins only after development and validation are complete.
- [ ] Verify the final source tree contains no generated binaries, credentials, or runtime state.
- [ ] Tag and document a validated release.

## Working rules

- Preserve ABI 1.0 compatibility.
- Do not introduce a second Nakama WebSocket connection for AgentBus.
- Keep AgentBus decoupled from MCP tools.
- Keep the daemon running as `mcp`; retain `warbotz` administrative access until daemon autonomy is fully validated.
- Do not implement graceful shutdown until the later lifecycle phase above.
- Do not remove tests or test plugins prematurely.
- Inspect changes and review the diff before committing or deploying.
- Do not restart the production daemon or replace loaded plugin libraries without an explicit validation plan.
