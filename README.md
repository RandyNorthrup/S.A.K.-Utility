# S.A.K. Utility

**Swiss Army Knife Utility** is a Windows desktop toolkit for PC technicians and IT support workflows, focused on migration, maintenance, and recovery tasks.

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://isocpp.org/)
[![Qt 6.5.3](https://img.shields.io/badge/Qt-6.5.3-green.svg)](https://www.qt.io/)
[![Windows](https://img.shields.io/badge/Platform-Windows%2010%2F11-blue.svg)](https://www.microsoft.com/windows)
[![Build Status](https://img.shields.io/github/actions/workflow/status/RandyNorthrup/S.A.K.-Utility/build-release.yml?branch=main)](https://github.com/RandyNorthrup/S.A.K.-Utility/actions)

---

## About

S.A.K. Utility provides a consolidated set of technician tools in a single, portable UI for Windows 10/11. It is built in C++23 with Qt 6.5.3 and uses CMake for builds.

**Author:** Randy Northrup

**Current version:** 0.5.6

---

## Highlights

- Portable desktop application with no installer required.
- Technician-focused workflows for migration, backup/restore, maintenance, and recovery.
- Multi-panel UI with progress tracking and detailed logs for long operations.

---

## Core Features

- **Quick Actions** — One‑click maintenance and recovery tasks with progress and logging.
- **User Profile Backup & Restore** — Guided wizards for scanning users, selecting data, and restoring profiles.
- **Application Migration** — Scan installed apps and build install sets via embedded Chocolatey.
- **Network Transfer (LAN)** — Secure source/destination transfers with optional orchestrator mode for multi‑PC deployments.
- **Directory Organizer** — Organize files by extension with preview mode and collision strategies.
- **Duplicate Finder** — Hash‑based duplicate detection with safe actions.
- **Image Flasher** — Write bootable media with optional Windows ISO download support.

---

## System Requirements

- Windows 10 or Windows 11 (x64)
- Visual Studio 2022 Build Tools or full Visual Studio (for building)
- CMake 3.28+
- Qt 6.5.3 (MSVC toolchain)

---

## Quick Start (Users)

1. Download the latest release from [Releases](https://github.com/RandyNorthrup/S.A.K.-Utility/releases).
2. Extract to a folder (example: C:\Tools\SAK-Utility).
3. Run sak_utility.exe.
4. (Optional) Create an empty portable.ini next to the executable to enable portable settings.

---

## Build (Developers)

### Configure & Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The executable will be at build/Release/sak_utility.exe.

---

## Configuration & Data

- Portable settings: create an empty portable.ini next to the executable.
- Logs are written to an _logs directory under the current working directory.

---

## Documentation

- Network Transfer details: [docs/NETWORK_TRANSFER.md](docs/NETWORK_TRANSFER.md)
- Quick Actions notes: [docs/QUICK_ACTIONS_IMPLEMENTATION.md](docs/QUICK_ACTIONS_IMPLEMENTATION.md)
- Windows maintenance commands reference: [docs/WINDOWS_MAINTENANCE_COMMANDS_REFERENCE.md](docs/WINDOWS_MAINTENANCE_COMMANDS_REFERENCE.md)

---

## Contributing

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) for coding standards, workflow, and guidelines.

---

## License

This project is licensed under the GNU General Public License v2. See [LICENSE](LICENSE) for details.
