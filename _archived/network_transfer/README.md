# Network Transfer — Archived Code

## Overview

This folder contains the complete **Network Transfer** panel and all supporting
code, removed from S.A.K. Utility. The feature provided peer-to-peer encrypted
LAN file transfer with three operating modes: Source, Destination, and
Orchestrator (one-to-many deployment).

This code was archived for potential future reuse in other projects.

## Feature Summary

- **Source Mode** — Scan local user profiles, discover peers via UDP broadcast,
  connect over TCP, send encrypted file data with chunk-level integrity.
- **Destination Mode** — Listen for incoming transfers, approve/reject requests,
  receive files with resume support, restore to target locations.
- **Orchestrator Mode** — Centralized multi-PC deployment. Map source users to
  destination PCs, manage concurrent transfer queues, track deployment progress
  across the fleet.

### Security

- AES-256-GCM encryption per chunk (Windows BCrypt API)
- PBKDF2 key derivation from shared password
- Challenge/response peer authentication
- SHA-256 integrity verification per file

### Reliability

- Periodic transfer checkpointing for resume on interruption
- Bandwidth throttling (configurable limit or unlimited)
- Automatic peer discovery with timeout/expiry

## Directory Structure

```
_archived/network_transfer/
├── README.md                          ← This file
├── src/
│   ├── core/                          ← Core logic (21 files)
│   │   ├── assignment_queue_store.cpp
│   │   ├── deployment_history.cpp
│   │   ├── deployment_manager.cpp
│   │   ├── deployment_summary_report.cpp
│   │   ├── destination_registry.cpp
│   │   ├── mapping_engine.cpp
│   │   ├── migration_orchestrator.cpp
│   │   ├── network_connection_manager.cpp
│   │   ├── network_transfer_controller.cpp
│   │   ├── network_transfer_protocol.cpp
│   │   ├── network_transfer_report.cpp
│   │   ├── network_transfer_security.cpp
│   │   ├── network_transfer_types.cpp
│   │   ├── network_transfer_worker.cpp
│   │   ├── orchestration_client.cpp
│   │   ├── orchestration_discovery_service.cpp
│   │   ├── orchestration_protocol.cpp
│   │   ├── orchestration_server.cpp
│   │   ├── orchestration_types.cpp
│   │   ├── parallel_transfer_manager.cpp
│   │   └── peer_discovery_service.cpp
│   └── gui/                           ← UI panel (3 files, split pattern)
│       ├── network_transfer_panel.cpp
│       ├── network_transfer_panel_orchestrator.cpp
│       └── network_transfer_panel_transfer.cpp
├── include/sak/                       ← Headers (24 files)
│   ├── assignment_queue_store.h
│   ├── deployment_history.h
│   ├── deployment_manager.h
│   ├── deployment_summary_report.h
│   ├── destination_registry.h
│   ├── mapping_engine.h
│   ├── migration_orchestrator.h
│   ├── network_connection_manager.h
│   ├── network_constants.h            ← Copy (original still in project)
│   ├── network_transfer_controller.h
│   ├── network_transfer_panel.h
│   ├── network_transfer_protocol.h
│   ├── network_transfer_report.h
│   ├── network_transfer_security.h
│   ├── network_transfer_types.h
│   ├── network_transfer_worker.h
│   ├── orchestration_client.h
│   ├── orchestration_discovery_service.h
│   ├── orchestration_protocol.h
│   ├── orchestration_server.h
│   ├── orchestration_server_interface.h
│   ├── orchestration_types.h
│   ├── parallel_transfer_manager.h
│   └── peer_discovery_service.h
├── tests/
│   ├── unit/                          ← Unit tests (22 files)
│   │   ├── test_assignment_queue_store.cpp
│   │   ├── test_deployment_history.cpp
│   │   ├── test_deployment_manager.cpp
│   │   ├── test_deployment_summary_report.cpp
│   │   ├── test_destination_registry.cpp
│   │   ├── test_mapping_engine.cpp
│   │   ├── test_migration_orchestrator.cpp
│   │   ├── test_network_connection.cpp
│   │   ├── test_network_transfer_controller.cpp
│   │   ├── test_network_transfer_report.cpp
│   │   ├── test_network_transfer_worker.cpp
│   │   ├── test_orchestration_client.cpp
│   │   ├── test_orchestration_discovery_service.cpp
│   │   ├── test_orchestration_protocol.cpp
│   │   ├── test_orchestration_server.cpp
│   │   ├── test_orchestration_types.cpp
│   │   ├── test_parallel_transfer_manager.cpp
│   │   ├── test_parallel_transfer_manager_stress.cpp
│   │   ├── test_peer_discovery.cpp
│   │   ├── test_transfer_protocol.cpp
│   │   ├── test_transfer_security.cpp
│   │   └── test_transfer_types.cpp
│   └── integration/                   ← Integration test (1 file)
│       └── test_network_transfer_workflow.cpp
├── resources/icons/
│   └── panel_network_transfer.svg     ← Panel tab icon
└── docs/
    └── NETWORK_TRANSFER_MODES.md      ← Feature documentation
```

## Architecture

### Key Patterns

- **Panel + Controller** — `NetworkTransferPanel` (thin UI) delegates all logic
  to `NetworkTransferController`. The panel is split across three `.cpp` files
  using Qt's partial-class split pattern for manageability.
- **Worker thread** — `NetworkTransferWorker` (subclass of `WorkerBase`) runs on
  a dedicated `QThread`. All network I/O happens off the GUI thread.
  Communication is via signals/slots only.
- **Protocol layer** — `NetworkTransferProtocol` handles framed message
  serialization (magic bytes, version, type, length-prefixed payload).
- **Security layer** — `NetworkTransferSecurity` wraps Windows BCrypt for
  AES-256-GCM encryption, PBKDF2 key derivation, and challenge/response auth.
- **Orchestration** — `OrchestrationServer` + `OrchestrationClient` implement a
  multi-PC deployment protocol. `MappingEngine` maps source users to destination
  PCs. `DeploymentManager` coordinates concurrent transfer jobs.

### Dependencies

- **Qt 6.5+** — Core, Widgets, Network, Concurrent
- **Windows BCrypt API** — AES-256-GCM encryption, PBKDF2 key derivation
- **`network_constants.h`** — Shared constants file (buffer sizes, port numbers).
  Note: this file remains in the main project as it is also used by other
  components (ISO downloader, image flasher, etc.). A copy is included here for
  reference.

## Reuse Notes

To integrate this code into a new project:

1. Copy `src/core/`, `src/gui/`, and `include/sak/` into your project tree.
2. Add all `.cpp` files to your CMakeLists.txt.
3. Ensure Qt6 Network and Concurrent modules are available.
4. The `ConfigManager` integration (settings persistence) will need adaptation
   to your project's configuration system.
5. The `WorkerBase` base class and logging (`sak::logInfo`, etc.) are
   project-specific — either bring them along or replace with equivalents.
6. Tests use the Qt Test framework and can be re-added to CTest.

## Original Project

**S.A.K. Utility** — Swiss Army Knife Utility for Windows PC technicians.
Copyright (C) Randy Northrup 2025. Licensed under AGPL-3.0-or-later.
