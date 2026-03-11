# Network Transfer Modes

S.A.K. Utility provides three network transfer modes for migrating user profiles,
files, and settings between Windows PCs on a local network. Each mode serves a
specific workflow, from simple one-to-one transfers to enterprise-scale
multi-PC deployments.

---

## Source (Send)

**Purpose:** Send data FROM this PC to another machine on the network.

Use the Source mode on the **old PC** — the one that has the user profiles, files,
and settings you want to migrate to a new machine.

### When to Use

- You are replacing a user's PC and need to copy their documents, desktop,
  favorites, and application settings to the new machine.
- You are migrating a single user (or a few users) from one PC to another.
- Both PCs are on the same local network.

### How It Works

1. **Scan Users** — Detects all Windows user profiles on this machine and shows
   their profile path and approximate size.
2. **Customize Selection** — Click any user to choose exactly which folders to
   include (Documents, Desktop, Downloads, AppData, etc.) and exclude
   specific subfolders or file types.
3. **Additional Data** — Optionally scan and include installed application lists,
   application data, WiFi profiles, and Ethernet configurations.
4. **Discover Peers** — Find destination machines on the network via automatic
   LAN discovery, or enter the destination's IP address and port manually.
5. **Transfer** — Sends a manifest to the destination for approval, then streams
   the selected data with optional encryption, compression, and bandwidth
   limiting.

### Key Features

- Per-user folder customization (include/exclude at any depth)
- Automatic peer discovery on the local network
- Manual IP + port for cross-subnet connections
- AES encryption with passphrase
- Compression to reduce transfer size
- Resumable transfers (automatic retry on network interruption)
- Real-time speed and progress tracking

---

## Destination (Receive)

**Purpose:** Receive data on this PC from a source machine or orchestrator.

Use the Destination mode on the **new PC** — the one that should receive the
migrated user profiles and settings.

### When to Use

- You have set up a new PC and want to pull data from the old one.
- An orchestrator is coordinating a deployment and this PC is one of the targets.
- You want to receive assignment queues from an orchestrator for batch processing.

### How It Works

1. **Set Destination Path** — Choose the base directory where incoming profiles
   will be stored (default: a timestamped folder on the desktop).
2. **Wait for Connection** — The destination listens for incoming connections
   from a source or orchestrator.
3. **Review Manifest** — When a source connects, the destination shows a
   summary of what will be transferred (users, folders, total size). You can
   approve or reject the transfer.
4. **Receive Data** — Files stream in with progress tracking. The destination
   writes files to the chosen base directory, organized by user.
5. **Auto-Restore** — If the "Apply Restore" option is enabled, user profiles
   are automatically applied to the local machine after transfer completes
   (creates local users, copies files to the correct paths).

### Orchestrator Connection

The destination can also connect to a remote orchestrator server. When
connected, it receives deployment assignments — instructions to prepare for
incoming data from a specific source. This enables centrally managed
multi-PC deployments where the technician controls everything from the
orchestrator UI.

### Key Features

- Manifest preview before accepting any data
- Auto-approve mode for unattended deployments
- Assignment queue for batch processing
- Post-transfer profile restoration
- Bandwidth monitoring

---

## Orchestrator (Deploy)

**Purpose:** Manage large-scale deployments from one source to multiple
destination PCs simultaneously.

Use the Orchestrator mode when you need to deploy profiles to **many machines
at once** — for example, setting up a computer lab, provisioning a fleet of
new employee PCs, or rolling out a standardized user environment.

### When to Use

- You are deploying the same set of user profiles to 5, 10, or 50+ PCs.
- You want centralized control over which users go to which destinations.
- You need deployment reports, job tracking, and retry/cancel capabilities.
- You are a technician or sysadmin managing a multi-PC rollout.

### How It Works

1. **Start Server** — Launch the orchestrator server on a chosen port. This
   machine becomes the central command station.
2. **Scan Source Users** — Discover the user profiles on the source machine
   (which can be this machine or a connected remote).
3. **Register Destinations** — Destination PCs connect to the orchestrator
   automatically. They appear in the destinations table as they register.
4. **Configure Mapping** — Choose how users map to destinations:
   - **Broadcast** — Every user goes to every destination.
   - **Round-Robin** — Users are distributed evenly across destinations.
   - **Custom Rules** — Drag specific users to specific destinations, or
     define rules in the custom mapping table.
5. **Set Limits** — Configure maximum concurrent jobs, global bandwidth cap,
   and per-job bandwidth limits.
6. **Deploy** — Start the deployment. The orchestrator sends assignments to
   each destination and streams data in parallel.
7. **Monitor** — Watch real-time progress for each job. Pause, resume, retry,
   or cancel individual jobs as needed.
8. **Report** — After completion, export deployment history as JSON, CSV, or
   PDF for documentation and audit.

### Deployment Templates

Save your mapping configuration, bandwidth settings, and concurrency limits
as a template. Load it later to repeat the same deployment pattern without
reconfiguring everything.

### Recovery

If the orchestrator or any destination crashes mid-deployment, use the
"Recover Last Deployment" button to resume from where it left off. The
system persists job state to disk so nothing is lost.

### Key Features

- Parallel transfers to many destinations simultaneously
- Three mapping strategies (broadcast, round-robin, custom)
- Drag-and-drop user-to-destination assignment
- Per-job pause, resume, retry, and cancel
- Global and per-job bandwidth throttling
- Deployment templates for repeatable configurations
- Crash recovery with automatic state persistence
- Full deployment history with JSON/CSV/PDF export
- Status legend with color-coded job states

---

## Security Options

All three modes share the same security configuration:

| Setting | Description |
|---|---|
| Encryption | AES-256 encryption with a shared passphrase |
| Compression | Compress data in-transit to reduce bandwidth |
| Resumable | Automatically resume interrupted transfers |
| Chunk Size | Size of each transfer chunk (affects memory usage) |
| Bandwidth Limit | Maximum transfer speed in MB/s (0 = unlimited) |
| Permission Mode | How file permissions are handled on the destination |

Access security settings from the padlock button at the bottom of any mode
dialog. Both source and destination must use the same passphrase for encrypted
transfers.

---

## Network Settings

Configure network behavior from the gear button at the bottom of any mode
dialog:

- **Listen Port** — Port for incoming connections (default: 45000)
- **Discovery Port** — Port for LAN peer discovery broadcasts
- **Relay Server** — Optional relay for NAT traversal
- **Auto-discover** — Enable/disable automatic peer discovery
- **IPv6** — Enable IPv6 support for dual-stack networks

---

## Quick Reference

| Scenario | Mode | This PC Is... |
|---|---|---|
| Migrate one user to a new PC | Source → Destination | Old PC uses Source, new PC uses Destination |
| Set up 20 new lab PCs | Orchestrator + Destinations | Control station uses Orchestrator, each lab PC uses Destination |
| Receive data from IT department | Destination | Your PC uses Destination, IT uses Source or Orchestrator |
| Create a deployment report | Orchestrator | After deployment, export from Orchestrator |
