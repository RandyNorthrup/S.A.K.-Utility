# Network Transfer Panel - Comprehensive Implementation Plan

**Version**: 2.0  
**Date**: December 13, 2025  
**Status**: Planning Phase  
**Target Release**: v0.7.0 (Phase 1), v0.8.0 (Phase 2), v0.9.0 (Phase 3)

---

## 🎯 Executive Summary

The Network Transfer Panel will enable direct PC-to-PC migration of user profiles and applications over local networks (Phase 1), the internet (Phase 2), and **multi-PC deployments** (Phase 3). This feature eliminates the need for intermediate storage devices and enables real-time migration workflows ranging from simple 1-to-1 transfers to complex many-to-many deployments for PC technicians and IT administrators.

### Key Objectives
- ✅ **Direct PC-to-PC transfer** - No intermediate storage required
- ✅ **Multi-PC deployments** - 1-to-many, many-to-many, and mapped migrations
- ✅ **Real-time progress** - Live monitoring on both source and destination
- ✅ **Encrypted transmission** - AES-256-GCM for data in transit
- ✅ **Resume capability** - Handle network interruptions gracefully
- ✅ **Bandwidth control** - QoS and throttling options
- ✅ **Firewall-friendly** - UPnP and manual port forwarding support
- ✅ **PXE-style orchestration** - Centralized deployment management

---

## 📊 Project Scope

### Phase 1: Local Network Transfer (v0.7.0 - Q2 2026)
**Goal**: Enable transfers over trusted local networks (LAN)

**Features**:
- TCP-based file transfer with streaming
- Automatic peer discovery via UDP broadcast
- Manual IP/port connection option
- User profile data transfer (Documents, Desktop, Pictures, etc.)
- Application list transfer and remote Chocolatey installation
- AES-256-GCM encryption for data in transit
- Resume capability for interrupted transfers
- Real-time progress monitoring on both PCs

**Protocols**:
- **Discovery**: UDP broadcast on port 54321
- **Control**: TCP on port 54322 (JSON messages)
- **Data**: TCP streaming on port 54323 (encrypted)

### Phase 2: Internet Transfer (v0.8.0 - Q3 2026)
**Goal**: Enable transfers over the internet via NAT traversal

**Features**:
- STUN/TURN server support for NAT traversal
- Hole punching for direct P2P connections
- Relay server fallback when direct connection fails
- Connection code pairing (6-digit codes)
- End-to-end encryption (independent of transport)
- Compression for bandwidth optimization

**Protocols**:
- **WebRTC DataChannels** - NAT traversal + encryption
- **QUIC** (alternative) - Modern UDP-based transport
- **Relay Server** - Hosted fallback for difficult NATs

### Phase 3: Multi-PC Deployment (v0.9.0 - Q4 2026)
**Goal**: Enable simultaneous migration to multiple PCs (PXE-style orchestration)

**Features**:
- **1-to-Many**: Migrate one user profile to multiple destination PCs
- **Many-to-Many**: Migrate multiple users to multiple destination PCs
- **Mapped Deployment**: User 1 → PC 1, User 2 → PC 2, User N → PC N
- **App Deployment**: Deploy same app list to 10+ PCs simultaneously
- **Orchestration Server**: Central coordinator for multi-PC migrations
- **Batch Processing**: Queue migrations, prioritize transfers
- **Load Balancing**: Distribute bandwidth across destinations
- **Deployment Templates**: Save/load migration configurations
- **Health Monitoring**: Real-time status of all destination PCs

**Use Cases**:
1. **Office Rollout**: Migrate 20 users to 20 new PCs (1:1 mapping)
2. **Standard Build**: Deploy same user template to 50 PCs
3. **Lab Setup**: Deploy standard apps to all lab workstations
4. **Disaster Recovery**: Restore multiple users from backup to new hardware

**Protocols**:
- **Orchestration Protocol** - Central server coordinates transfers
- **Multicast** (optional) - Same data to multiple destinations
- **Priority Queue** - Critical migrations first
- **Health Check** - Monitor destination PC readiness

---

## 🏗️ Architecture Overview

### Component Hierarchy

```
NetworkTransferPanel (QWidget)
├─ NetworkTransferController (QObject)
│  ├─ Mode: Source / Destination / Orchestrator (Phase 3)
│  ├─ State Machine: Idle → Discovering → Connected → Transferring → Complete
│  └─ Orchestrates: Discovery, Connection, Transfer, Cleanup
│
├─ PeerDiscoveryService (QObject)
│  ├─ QUdpSocket - UDP broadcast sender/receiver
│  ├─ Announces: Hostname, IP, Port, Protocol Version
│  └─ Discovers: Available peers on network
│
├─ NetworkConnectionManager (QObject)
│  ├─ QTcpServer - Listens for incoming connections (Destination mode)
│  ├─ QTcpSocket - Connects to remote peer (Source mode)
│  ├─ Handshake: Version check, capabilities exchange
│  └─ Authentication: Shared secret, certificate pinning (Phase 2)
│
├─ NetworkTransferWorker (QThread)
│  ├─ Sends: User profiles, app lists, metadata
│  ├─ Receives: Transfer requests, acknowledgments
│  ├─ Encryption: AES-256-GCM per chunk (64 KB chunks)
│  ├─ Progress: Real-time bytes sent/received, ETA
│  └─ Resume: Checkpoint-based resumption
│
├─ TransferProtocol (Static Utility)
│  ├─ Message Types: HELLO, AUTH, FILE_LIST, FILE_DATA, ACK, ERROR
│  ├─ Serialization: JSON for control, binary for data
│  └─ Versioning: Protocol v1.0 with backward compatibility
│
├─ TransferSecurityManager (QObject)
│  ├─ Key Exchange: ECDH (Elliptic Curve Diffie-Hellman)
│  ├─ Encryption: AES-256-GCM with authenticated encryption
│  ├─ Integrity: SHA-256 checksums per file
│  └─ Authentication: TLS 1.3 for control channel (Phase 2)
│
└─ MigrationOrchestrator (QObject) [PHASE 3]
   ├─ DeploymentManager - Manages multi-PC migrations
   │  ├─ Deployment types: 1-to-many, many-to-many, mapped
   │  ├─ Queue management: Priority, batch processing
   │  └─ Load balancing: Bandwidth distribution
   │
   ├─ DestinationRegistry - Tracks all destination PCs
   │  ├─ PC discovery and registration
   │  ├─ Health monitoring (CPU, RAM, disk, network)
   │  └─ Readiness checks (free space, permissions)
   │
   ├─ MappingEngine - User/App to PC assignment
   │  ├─ 1:N mapping (one source to many destinations)
   │  ├─ N:N mapping (multiple sources to multiple destinations)
   │  └─ Custom rules (User1→PC1, User2→PC2, etc.)
   │
   └─ ProgressAggregator - Multi-transfer monitoring
      ├─ Overall deployment progress
      ├─ Per-PC transfer status
      └─ Failure recovery and retry logic
```

---

## 🛠️ Technical Specifications

### Network Protocol Design

#### Discovery Protocol (UDP Broadcast - Port 54321)

**Announcement Packet** (JSON, sent every 2 seconds):
```json
{
  "protocol_version": "1.0",
  "message_type": "ANNOUNCE",
  "timestamp": 1734134400,
  "peer_info": {
    "hostname": "DESKTOP-ABC123",
    "os": "Windows 11 Pro",
    "app_version": "0.7.0",
    "ip_address": "192.168.1.100",
    "tcp_port": 54322,
    "mode": "destination",
    "capabilities": ["user_profiles", "app_migration", "resume"]
  }
}
```

**Discovery Response** (unicast UDP reply):
```json
{
  "protocol_version": "1.0",
  "message_type": "DISCOVERY_REPLY",
  "timestamp": 1734134401,
  "peer_info": { /* same as ANNOUNCE */ }
}
```

---

#### Connection Protocol (TCP - Port 54322)

**Phase 1: Handshake**
```json
{
  "protocol_version": "1.0",
  "message_type": "HELLO",
  "peer_id": "uuid-1234-5678-90ab-cdef",
  "hostname": "SOURCE-PC",
  "capabilities": ["user_profiles", "app_migration", "resume", "compression"]
}
```

**Phase 2: Authentication** (Phase 1: Optional, Phase 2: Mandatory)
```json
{
  "message_type": "AUTH_CHALLENGE",
  "challenge": "base64_encoded_random_bytes",
  "public_key": "base64_encoded_ecdh_public_key"
}
```

**Phase 3: Transfer Manifest**
```json
{
  "message_type": "TRANSFER_MANIFEST",
  "transfer_id": "uuid-transfer-123",
  "manifest": {
    "user_profiles": [
      {
        "username": "John",
        "folders": ["Documents", "Desktop", "Pictures"],
        "total_files": 15234,
        "total_bytes": 5368709120,
        "checksum": "sha256_hash"
      }
    ],
    "applications": [
      {
        "name": "Google Chrome",
        "version": "120.0.6099.130",
        "choco_package": "googlechrome",
        "version_locked": true
      }
    ]
  }
}
```

**Phase 4: Transfer Control**
```json
{
  "message_type": "FILE_TRANSFER_START",
  "file_id": "uuid-file-456",
  "path": "C:\\Users\\John\\Documents\\report.docx",
  "size": 2048576,
  "checksum": "sha256_hash",
  "chunk_size": 65536,
  "compression": "gzip"
}
```

**Phase 5: Data Transfer** (Binary Protocol - Port 54323)
```
[Header: 16 bytes]
├─ Magic: 0x53414B4E (SAKN)
├─ Version: 1 byte (0x01)
├─ Flags: 1 byte (compression, encryption, last_chunk)
├─ Chunk ID: 4 bytes (uint32)
├─ Chunk Size: 4 bytes (uint32)
└─ Checksum: 4 bytes (CRC32)

[Encrypted Data: variable length]
├─ IV: 12 bytes (GCM nonce)
├─ Ciphertext: chunk_size bytes
└─ Auth Tag: 16 bytes (GCM tag)
```

**Phase 6: Acknowledgment**
```json
{
  "message_type": "FILE_TRANSFER_ACK",
  "file_id": "uuid-file-456",
  "chunks_received": [0, 1, 2, 3, /* ... */, 31],
  "status": "complete"
}
```

---

### Security Architecture

#### Encryption Layers

**Phase 1 (Local Network)**:
- **Transport**: TCP (plaintext acceptable on trusted LANs)
- **Data Encryption**: AES-256-GCM per 64KB chunk
- **Key Exchange**: Pre-shared secret (user enters matching code)
- **Integrity**: SHA-256 per file

**Phase 2 (Internet)**:
- **Transport**: TLS 1.3 for control channel
- **Data Encryption**: AES-256-GCM (same as Phase 1)
- **Key Exchange**: ECDH (Elliptic Curve Diffie-Hellman)
- **Integrity**: SHA-256 per file + HMAC per chunk
- **Authentication**: Certificate pinning or connection codes

#### Connection Code System (Phase 2)

**Generation**:
```
Connection Code = 6-digit numeric code (000000-999999)
├─ Generated on destination PC
├─ Valid for 10 minutes
├─ Single-use (expires after successful connection)
└─ Stored on relay server with peer ID

User enters code on source PC
├─ Code sent to relay server
├─ Server returns peer connection info (IP, port, public key)
└─ Direct P2P connection established via hole punching
```

**Relay Server API** (Phase 2):
```
POST /api/v1/register
{
  "code": "123456",
  "peer_id": "uuid",
  "public_key": "base64_ecdh_key",
  "external_ip": "1.2.3.4",
  "external_port": 54323
}

GET /api/v1/lookup?code=123456
{
  "peer_id": "uuid",
  "public_key": "base64_ecdh_key",
  "external_ip": "1.2.3.4",
  "external_port": 54323,
  "expires_at": 1734134400
}
```

---

### Data Transfer Flow

#### Source PC (Sender) Flow

```
1. User clicks "Start as Source"
   ├─ Scan local user profiles (UserProfileBackupWizard logic)
   ├─ Scan installed apps (AppScanner logic)
   ├─ Build transfer manifest
   └─ Display summary (users, apps, total size)

2. User selects data to transfer
   ├─ Check/uncheck users
   ├─ Customize folders per user
   ├─ Select apps to install on destination
   └─ Apply smart filters

3. Discovery phase
   ├─ Enable UDP broadcast announcements
   ├─ Listen for destination peers
   ├─ Display discovered peers in list
   └─ User selects destination PC

4. Connection phase
   ├─ Establish TCP connection to destination
   ├─ Perform handshake
   ├─ Exchange capabilities
   └─ (Optional) Enter shared secret for encryption

5. Transfer phase
   ├─ Send manifest to destination
   ├─ Wait for approval
   ├─ Stream files with encryption
   ├─ Update progress bar (files/bytes/ETA)
   └─ Handle ACKs and retries

6. Completion
   ├─ Send completion message
   ├─ Wait for verification
   ├─ Display success summary
   └─ Offer to generate report
```

#### Destination PC (Receiver) Flow

```
1. User clicks "Start as Destination"
   ├─ Start UDP broadcast announcements
   ├─ Start TCP listener on port 54322
   ├─ Display waiting screen with IP address
   └─ Show connection code (Phase 2)

2. Connection accepted
   ├─ Incoming connection detected
   ├─ Perform handshake
   ├─ Validate protocol version
   └─ (Optional) Verify shared secret

3. Manifest review
   ├─ Receive transfer manifest from source
   ├─ Display summary (users, apps, size)
   ├─ User approves or rejects transfer
   └─ Send approval to source

4. Transfer phase
   ├─ Receive encrypted file chunks
   ├─ Decrypt and write to disk
   ├─ Send ACKs for completed files
   ├─ Update progress bar
   └─ Handle resume on disconnect

5. Application installation
   ├─ Parse app list from manifest
   ├─ Install via ChocolateyManager
   ├─ Monitor installation progress
   └─ Report failures

6. Completion
   ├─ Verify all files received (SHA-256)
   ├─ Restore ACLs on user folders
   ├─ Display completion summary
   └─ Log transfer details
```

---

## 🎨 User Interface Design

### NetworkTransferPanel Layout

```
┌─────────────────────────────────────────────────────────────┐
│ Network Transfer Panel                                      │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌────────────────────────────────────────────────────────┐│
│  │  Select Mode:                                          ││
│  │  ○ Source (Send data from this PC)                    ││
│  │  ○ Destination (Receive data on this PC)              ││
│  └────────────────────────────────────────────────────────┘│
│                                                             │
│  ┌─────────────────────── SOURCE MODE ────────────────────┐│
│  │                                                         ││
│  │  Step 1: Select Data to Transfer                       ││
│  │  ┌─────────────────────────────────────────────────┐  ││
│  │  │ Users:                                          │  ││
│  │  │ ☑ John (5.2 GB)                                │  ││
│  │  │ ☑ Sarah (3.8 GB)                               │  ││
│  │  │ ☐ Admin (120 MB)                               │  ││
│  │  │                                                 │  ││
│  │  │ Applications:                                   │  ││
│  │  │ ☑ Google Chrome 120.0.6099.130                 │  ││
│  │  │ ☑ Microsoft Office 2021                        │  ││
│  │  │ ☑ Adobe Acrobat Reader DC                      │  ││
│  │  │                                                 │  ││
│  │  │ Total Size: 9.4 GB                             │  ││
│  │  └─────────────────────────────────────────────────┘  ││
│  │                                                         ││
│  │  Step 2: Find Destination PC                           ││
│  │  ┌─────────────────────────────────────────────────┐  ││
│  │  │ Discovering peers... [Refresh]                  │  ││
│  │  │                                                 │  ││
│  │  │ Hostname        IP Address      Status          │  ││
│  │  │ ─────────────────────────────────────────────   │  ││
│  │  │ WORK-PC-02    192.168.1.105   Ready            │◀─── Selected
│  │  │ LAPTOP-HOME   192.168.1.142   Busy             │  ││
│  │  │                                                 │  ││
│  │  │ Or enter manually:                              │  ││
│  │  │ IP: [192.168.1.___] Port: [54322]              │  ││
│  │  └─────────────────────────────────────────────────┘  ││
│  │                                                         ││
│  │  Encryption: ☑ Enable  Secret: [●●●●●●●●] (optional)  ││
│  │                                                         ││
│  │  [Connect]  [Cancel]                                   ││
│  └─────────────────────────────────────────────────────────┘│
│                                                             │
│  ┌────────────────── DESTINATION MODE ────────────────────┐│
│  │                                                         ││
│  │  Waiting for connection...                             ││
│  │                                                         ││
│  │  Your PC is discoverable as:                           ││
│  │  ┌─────────────────────────────────────────────────┐  ││
│  │  │  Hostname:   WORK-PC-02                         │  ││
│  │  │  IP Address: 192.168.1.105                      │  ││
│  │  │  Port:       54322                              │  ││
│  │  │                                                 │  ││
│  │  │  Connection Code: [123 456] 🔄 ◀─── Phase 2     │  ││
│  │  └─────────────────────────────────────────────────┘  ││
│  │                                                         ││
│  │  Encryption: ☑ Enable  Secret: [●●●●●●●●] (optional)  ││
│  │                                                         ││
│  │  [Stop Listening]                                      ││
│  └─────────────────────────────────────────────────────────┘│
│                                                             │
│  ┌─────────────────── TRANSFER PROGRESS ──────────────────┐│
│  │                                                         ││
│  │  Connected to: WORK-PC-02 (192.168.1.105)             ││
│  │                                                         ││
│  │  Current: Transferring John\Documents\report.docx      ││
│  │  Progress: ████████████████░░░░░░░░░░░  67%           ││
│  │  Speed: 45.3 MB/s  |  ETA: 2m 34s                     ││
│  │                                                         ││
│  │  Overall Progress:                                     ││
│  │  Files:  1,234 / 2,145  (58%)                         ││
│  │  Bytes:  5.4 GB / 9.4 GB  (57%)                       ││
│  │                                                         ││
│  │  ┌─────────────────────────────────────────────────┐  ││
│  │  │ [LOG]                                           │  ││
│  │  │ 14:32:15 - Connected to peer                    │  ││
│  │  │ 14:32:18 - Manifest approved by destination     │  ││
│  │  │ 14:32:20 - Starting file transfer...            │  ││
│  │  │ 14:33:45 - John\Documents\report.docx (67%)     │  ││
│  │  │ ...                                             │  ││
│  │  └─────────────────────────────────────────────────┘  ││
│  │                                                         ││
│  │  [Pause]  [Cancel Transfer]                            ││
│  └─────────────────────────────────────────────────────────┘│
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 📂 File Structure

### New Files to Create

#### Headers (`include/sak/`)

```
network_transfer_panel.h           # Main UI panel
network_transfer_controller.h      # State machine orchestrator
peer_discovery_service.h           # UDP broadcast discovery
network_connection_manager.h       # TCP connection handling
network_transfer_worker.h          # Background transfer thread
transfer_protocol.h                # Protocol definitions
transfer_security_manager.h        # Encryption/authentication
network_error_codes.h              # Network-specific errors
```

#### Implementation (`src/`)

```
gui/network_transfer_panel.cpp
core/network_transfer_controller.cpp
core/peer_discovery_service.cpp
core/network_connection_manager.cpp
threading/network_transfer_worker.cpp
core/transfer_protocol.cpp
core/transfer_security_manager.cpp
```

#### Tests (`tests/`)

```
test_peer_discovery.cpp            # UDP broadcast tests
test_network_connection.cpp        # TCP handshake tests
test_transfer_protocol.cpp         # Protocol serialization
test_transfer_security.cpp         # Encryption/key exchange
test_network_transfer_integration.cpp  # End-to-end transfer
```

---

## 🔧 Implementation Phases

### Phase 1.1: Foundation (Week 1-2)

**Goals**:
- Create UI skeleton
- Implement discovery protocol
- Basic TCP connection

**Tasks**:
1. Create `network_transfer_panel.h/cpp` with mode selection UI
2. Create `peer_discovery_service.h/cpp` with UDP broadcast
3. Create `network_connection_manager.h/cpp` with TCP server/client
4. Create `transfer_protocol.h/cpp` with message definitions
5. Implement handshake and capability exchange
6. Add network error codes to `error_codes.h`
7. Write unit tests for discovery and connection

**Acceptance Criteria**:
- ✅ Source PC can discover destination PC on LAN
- ✅ TCP connection established with handshake
- ✅ UI shows discovered peers
- ✅ Manual IP entry works

---

### Phase 1.2: Data Transfer (Week 3-4)

**Goals**:
- Implement file streaming
- Add encryption
- Progress tracking

**Tasks**:
1. Create `network_transfer_worker.h/cpp` with threaded transfer
2. Create `transfer_security_manager.h/cpp` for AES-256-GCM
3. Implement manifest generation (reuse UserProfileBackupWizard logic)
4. Implement file chunking (64 KB chunks)
5. Implement encryption per chunk
6. Add progress signals (bytes/files/ETA)
7. Implement ACK/retry logic
8. Write integration tests for file transfer

**Acceptance Criteria**:
- ✅ Files transferred successfully over network
- ✅ Encryption enabled with pre-shared secret
- ✅ Progress updates in real-time on both PCs
- ✅ Resume works after network interruption
- ✅ SHA-256 verification on completion

---

### Phase 1.3: User Profile Integration (Week 5-6)

**Goals**:
- Integrate with existing backup/restore logic
- Handle ACLs over network
- Smart filtering

**Tasks**:
1. Create `network_transfer_controller.h/cpp` state machine
2. Integrate with `UserProfileBackupWizard` for source data selection
3. Integrate with `UserProfileRestoreWorker` for destination restore
4. Implement ACL transfer (serialize DACL to JSON)
5. Integrate `SmartFileFilter` for exclusion rules
6. Add bandwidth throttling (QoS)
7. Write end-to-end user profile transfer test

**Acceptance Criteria**:
- ✅ User profiles transferred with folder structure intact
- ✅ ACLs preserved on destination
- ✅ Smart filters applied (temp files excluded)
- ✅ Bandwidth limiting works (configurable in settings)

---

### Phase 1.4: App Migration Integration (Week 7-8)

**Goals**:
- Transfer app lists
- Remote Chocolatey installation
- Version locking

**Tasks**:
1. Integrate with `AppScanner` for source app detection
2. Integrate with `PackageMatcher` for Chocolatey matching
3. Serialize app list to JSON (name, version, choco_package)
4. Implement remote installation trigger on destination
5. Stream installation logs back to source PC
6. Handle failures and retry logic
7. Write app migration integration test

**Acceptance Criteria**:
- ✅ Apps transferred to destination as Chocolatey install list
- ✅ Destination installs apps via ChocolateyManager
- ✅ Source PC sees real-time installation progress
- ✅ Version locking respected

---

### Phase 1.5: Polish & Testing (Week 9-10)

**Goals**:
- Error handling
- Edge case testing
- Documentation

**Tasks**:
1. Implement robust error handling (network timeouts, disk full, etc.)
2. Add logging for all network operations
3. Test on various network conditions (WiFi, Ethernet, slow networks)
4. Test with large transfers (50+ GB)
5. Test with many small files (100k+ files)
6. Write user documentation for network transfer
7. Update README.md with network transfer section
8. Create troubleshooting guide (firewall, ports, etc.)

**Acceptance Criteria**:
- ✅ No crashes on network errors
- ✅ Clear error messages to user
- ✅ Comprehensive logging
- ✅ User documentation complete

---

### Phase 2.1: Internet Transfer - NAT Traversal (Week 11-13)

**Goals**:
- STUN/TURN support
- Connection code system
- Relay server

**Tasks**:
1. Research and select WebRTC library (QtWebChannel or native WebRTC)
2. Implement STUN client for external IP discovery
3. Implement TURN client for relay fallback
4. Create relay server (Node.js + Express + WebSocket)
5. Implement connection code generation (6-digit codes)
6. Implement hole punching for direct P2P
7. Add connection code UI to NetworkTransferPanel
8. Deploy relay server to cloud (AWS/Azure/DigitalOcean)

**Acceptance Criteria**:
- ✅ Connection codes work for pairing
- ✅ Direct P2P connection established when possible
- ✅ Relay fallback works when NAT is too strict
- ✅ Connection time < 10 seconds

---

### Phase 2.2: Internet Transfer - Security (Week 14-15)

**Goals**:
- ECDH key exchange
- TLS 1.3 for control channel
- End-to-end encryption

**Tasks**:
1. Implement ECDH key exchange (use Qt's QSslKey or OpenSSL)
2. Replace TCP with TLS 1.3 sockets (QSslSocket)
3. Implement certificate pinning or connection code verification
4. Add HMAC per chunk for integrity
5. Implement replay attack prevention (nonce sequence)
6. Security audit of protocol
7. Penetration testing

**Acceptance Criteria**:
- ✅ ECDH key exchange successful
- ✅ TLS 1.3 established for control channel
- ✅ No plaintext data on network
- ✅ Security audit passes

---

### Phase 2.3: Internet Transfer - Optimization (Week 16-17)

**Goals**:
- Compression
- Adaptive bitrate
- Performance tuning

**Tasks**:
1. Add gzip/zstd compression option (reuse existing decompressors)
2. Implement adaptive chunk size based on network speed
3. Implement congestion control (monitor packet loss, RTT)
4. Add multi-stream support (parallel file transfers)
5. Optimize for high-latency networks (100ms+ RTT)
6. Benchmark against FTP/SFTP/rsync
7. Performance tuning

**Acceptance Criteria**:
- ✅ Compression reduces transfer time by 30%+ for text files
- ✅ Adaptive bitrate maintains stable transfer
- ✅ Performance competitive with rsync

---

## 📋 Configuration & Settings

### ConfigManager Extensions

Add to `config_manager.h/cpp`:

```cpp
// Network Transfer Settings
bool getNetworkTransferEnabled() const;
void setNetworkTransferEnabled(bool enabled);

int getNetworkTransferPort() const;
void setNetworkTransferPort(int port);

bool getNetworkTransferEncryptionEnabled() const;
void setNetworkTransferEncryptionEnabled(bool enabled);

int getNetworkTransferMaxBandwidth() const; // KB/s, 0 = unlimited
void setNetworkTransferMaxBandwidth(int bandwidth);

bool getNetworkTransferAutoDiscoveryEnabled() const;
void setNetworkTransferAutoDiscoveryEnabled(bool enabled);

int getNetworkTransferChunkSize() const; // Default: 65536 (64 KB)
void setNetworkTransferChunkSize(int size);

bool getNetworkTransferCompressionEnabled() const;
void setNetworkTransferCompressionEnabled(bool enabled);

QString getNetworkTransferRelayServer() const; // Phase 2
void setNetworkTransferRelayServer(const QString& server);
```

**Default Values**:
```cpp
network_transfer/enabled = true
network_transfer/port = 54322
network_transfer/encryption_enabled = true
network_transfer/max_bandwidth = 0 (unlimited)
network_transfer/auto_discovery = true
network_transfer/chunk_size = 65536
network_transfer/compression_enabled = false (Phase 2)
network_transfer/relay_server = "relay.sak-utility.io:8443" (Phase 2)
```

---

## 🧪 Testing Strategy

### Unit Tests

**test_peer_discovery.cpp**:
- UDP broadcast sending
- UDP broadcast receiving
- Peer list population
- Manual IP entry

**test_network_connection.cpp**:
- TCP server listening
- TCP client connecting
- Handshake protocol
- Capability exchange

**test_transfer_protocol.cpp**:
- JSON serialization
- Binary chunk serialization
- Message validation
- Protocol versioning

**test_transfer_security.cpp**:
- AES-256-GCM encryption/decryption
- ECDH key exchange (Phase 2)
- SHA-256 checksums
- Nonce sequence validation

### Integration Tests

**test_network_transfer_integration.cpp**:
- Full transfer flow (source → destination)
- Large file transfer (1 GB+)
- Many small files (10k+ files)
- Resume after disconnect
- Bandwidth throttling
- ACL preservation

### Manual Testing Scenarios

1. **Local Network Transfer**:
   - Connect two PCs on same LAN
   - Transfer user profile (5 GB)
   - Verify files and ACLs
   - Measure transfer speed

2. **WiFi Transfer**:
   - Source on WiFi, destination on Ethernet
   - Transfer with weak signal
   - Test resume on signal drop

3. **Firewall Testing**:
   - Enable Windows Firewall on destination
   - Verify automatic firewall prompt
   - Test manual port forwarding

4. **Large Transfer**:
   - Transfer 50+ GB user profile
   - Monitor memory usage
   - Verify no memory leaks

5. **Stress Test**:
   - Transfer 100k+ small files
   - Monitor CPU/network utilization
   - Verify no timeouts

---

## 🔒 Security Considerations

### Threat Model

**Threats on Local Network** (Phase 1):
- ❌ **Eavesdropping**: Attacker sniffs network traffic
  - **Mitigation**: AES-256-GCM encryption per chunk
  
- ❌ **Man-in-the-Middle**: Attacker impersonates peer
  - **Mitigation**: Pre-shared secret, optional certificate pinning
  
- ❌ **Replay Attacks**: Attacker replays captured packets
  - **Mitigation**: Nonce sequence numbers, timestamp validation

**Threats on Internet** (Phase 2):
- ❌ **NAT Traversal Attacks**: Attacker hijacks STUN/TURN session
  - **Mitigation**: ECDH key exchange, connection code verification
  
- ❌ **Relay Server Compromise**: Relay server logs/modifies data
  - **Mitigation**: End-to-end encryption (independent of relay)
  
- ❌ **Connection Code Guessing**: Attacker brute-forces codes
  - **Mitigation**: 6-digit codes (1M combinations), 10-minute expiry, rate limiting

### Best Practices

1. **Default to Encryption**: Always enable AES-256-GCM by default
2. **Warn on Plaintext**: Show warning if user disables encryption
3. **Firewall Guidance**: Detect firewall blocks, offer guidance
4. **Log Security Events**: Log handshake failures, auth failures
5. **Rate Limiting**: Prevent connection spam, DoS attempts
6. **Secure Defaults**: Use TLS 1.3, disable older protocols

---

## 🎯 Success Metrics

### Performance Targets

| Metric | Phase 1 (LAN) | Phase 2 (Internet) |
|--------|---------------|---------------------|
| Connection Time | < 5 seconds | < 10 seconds |
| Transfer Speed (Gigabit LAN) | > 100 MB/s | N/A |
| Transfer Speed (100 Mbps Internet) | N/A | > 10 MB/s |
| Memory Usage | < 200 MB | < 300 MB |
| CPU Usage | < 30% (1 core) | < 50% (1 core) |
| Resume Time After Disconnect | < 3 seconds | < 10 seconds |

### Reliability Targets

- ✅ **99% Success Rate** on local network
- ✅ **95% Success Rate** on internet (NAT traversal)
- ✅ **Zero Data Loss** with checksums
- ✅ **Zero Crashes** on network errors

---

## 📚 Dependencies

### Qt Modules

Already in project:
- ✅ `Qt6::Core` - QObject, QString, QByteArray
- ✅ `Qt6::Network` - QTcpServer, QTcpSocket, QUdpSocket
- ✅ `Qt6::Widgets` - QWidget, QTableView, etc.

New additions needed:
- ⚠️ `Qt6::WebSockets` (Phase 2) - For relay server communication
- ⚠️ `OpenSSL` (Phase 2) - For ECDH, TLS 1.3 (Qt provides QSslSocket)

### External Libraries (Phase 2)

Consider:
- **libnice** - ICE/STUN/TURN implementation (LGPL)
- **libsrtp** - Secure RTP for encryption (BSD-3-Clause)
- **WebRTC Native** - Full WebRTC stack (BSD-3-Clause)

**Recommendation**: Start with Qt's built-in networking, add WebRTC in Phase 2 if needed.

---

## 📝 Documentation Requirements

### User Documentation

1. **Network Transfer Quick Start Guide**:
   - How to enable discovery
   - How to connect two PCs
   - Firewall configuration
   - Port forwarding (if needed)

2. **Troubleshooting Guide**:
   - "Connection timed out" → Check firewall
   - "Peer not found" → Check same network/subnet
   - "Transfer failed" → Check disk space
   - "Slow transfer" → Check network speed, disable WiFi power saving

3. **Security Best Practices**:
   - Always use encryption
   - Use strong pre-shared secrets (12+ chars)
   - Verify peer hostname before connecting

### Developer Documentation

1. **Protocol Specification**:
   - Complete message format documentation
   - State machine diagrams
   - Sequence diagrams

2. **Architecture Guide**:
   - Component interaction diagrams
   - Threading model
   - Error handling patterns

3. **API Reference**:
   - Public API for each class
   - Signal/slot documentation
   - Example usage code

---

## 🚧 Risks & Mitigation

### Technical Risks

| Risk | Impact | Probability | Mitigation |
|------|--------|-------------|------------|
| **NAT traversal failure** | High | Medium | Relay server fallback, clear error messages |
| **Performance on slow networks** | Medium | High | Compression, adaptive bitrate, resume |
| **Memory exhaustion (large files)** | High | Low | Streaming with fixed buffer size (64 KB chunks) |
| **Firewall blocking** | High | Medium | Auto-detect, guide user to configure firewall |
| **Security vulnerabilities** | Critical | Low | Security audit, penetration testing, code review |

### Project Risks

| Risk | Impact | Probability | Mitigation |
|------|--------|-------------|------------|
| **Scope creep** | Medium | High | Strict phase boundaries, defer Phase 2 features |
| **Compatibility issues (Qt versions)** | Low | Medium | Target Qt 6.5.3+, test on multiple versions |
| **Third-party library issues** | Medium | Low | Minimize external dependencies, use Qt where possible |

---

## 📅 Timeline Estimate

### Phase 1: Local Network (10 weeks)

| Week | Phase | Tasks | Deliverables |
|------|-------|-------|--------------|
| 1-2  | 1.1 Foundation | Discovery, TCP connection | Working handshake |
| 3-4  | 1.2 Data Transfer | File streaming, encryption | File transfer works |
| 5-6  | 1.3 User Profile Integration | ACLs, smart filters | Profile transfer works |
| 7-8  | 1.4 App Migration Integration | Chocolatey remote install | App migration works |
| 9-10 | 1.5 Polish & Testing | Error handling, docs | Ready for release |

**Target Release**: v0.7.0 (Q2 2026)

### Phase 2: Internet Transfer (7 weeks)

| Week | Phase | Tasks | Deliverables |
|------|-------|-------|--------------|
| 11-13 | 2.1 NAT Traversal | STUN/TURN, relay server | Internet connection works |
| 14-15 | 2.2 Security | ECDH, TLS 1.3 | Secure internet transfer |
| 16-17 | 2.3 Optimization | Compression, adaptive bitrate | Performance optimized |

**Target Release**: v0.8.0 (Q3 2026)

### Phase 3: Multi-PC Deployment (8 weeks)

| Week | Phase | Tasks | Deliverables |
|------|-------|-------|--------------|
| 18-19 | 3.1 Orchestration | DeploymentManager, DestinationRegistry | Multi-PC discovery |
| 20-21 | 3.2 Mapping Engine | 1:N, N:N, custom mapping | Deployment config works |
| 22-23 | 3.3 Parallel Transfers | Multi-threaded transfers, load balancing | 10+ simultaneous transfers |
| 24-25 | 3.4 Monitoring & UI | Progress aggregation, deployment dashboard | Complete UI |

**Target Release**: v0.9.0 (Q4 2026)

---

## 🎬 Getting Started Checklist

### Prerequisites

Before starting implementation:

- [ ] Review existing `UserProfileBackupWizard` and `AppMigrationPanel` code
- [ ] Study Qt Network module documentation (QTcpServer, QTcpSocket, QUdpSocket)
- [ ] Design protocol state machine diagram
- [ ] Create UML sequence diagrams for connection/transfer flows
- [ ] Set up test environment (2 Windows PCs on same network)
- [ ] Configure firewall rules for ports 54321-54323

### Phase 1.1 Setup

- [ ] Create branch: `feature/network-transfer-phase1`
- [ ] Create header files in `include/sak/`
- [ ] Create implementation files in `src/core/` and `src/gui/`
- [ ] Update `CMakeLists.txt` with new source files
- [ ] Add Qt6::Network to target_link_libraries
- [ ] Create test files in `tests/`
- [ ] Update VERSION to 0.6.0 (pre-release for network features)

---

## 📖 References

### Qt Documentation

- [Qt Network Module](https://doc.qt.io/qt-6/qtnetwork-index.html)
- [QTcpServer](https://doc.qt.io/qt-6/qtcpserver.html)
- [QTcpSocket](https://doc.qt.io/qt-6/qtcpsocket.html)
- [QUdpSocket](https://doc.qt.io/qt-6/qudpsocket.html)
- [QSslSocket](https://doc.qt.io/qt-6/qsslsocket.html) (Phase 2)

### Protocols

- [JSON-RPC 2.0](https://www.jsonrpc.org/specification) - Control message format
- [STUN RFC 5389](https://tools.ietf.org/html/rfc5389) - NAT traversal (Phase 2)
- [TURN RFC 5766](https://tools.ietf.org/html/rfc5766) - Relay protocol (Phase 2)
- [ICE RFC 5245](https://tools.ietf.org/html/rfc5245) - Connection establishment (Phase 2)

### Security

- [AES-GCM](https://en.wikipedia.org/wiki/Galois/Counter_Mode) - Authenticated encryption
- [ECDH](https://en.wikipedia.org/wiki/Elliptic-curve_Diffie%E2%80%93Hellman) - Key exchange (Phase 2)
- [TLS 1.3](https://tools.ietf.org/html/rfc8446) - Transport security (Phase 2)

---

## 💡 Future Enhancements (Post-v0.8)

### v0.9 - Advanced Features
- Multi-stream transfers (parallel files)
- Delta sync (only transfer changed files)
- Folder watching (real-time sync)
- Mobile app support (Android/iOS as destination)

### v1.0 - Enterprise Features
- Central management server (deploy to 100+ PCs)
- Active Directory integration
- Group Policy support
- Automated scheduling
- Email notifications on completion

---

## 📞 Support & Contributions

**Questions?** Open a GitHub Discussion  
**Found a Bug?** Open a GitHub Issue  
**Want to Contribute?** See [CONTRIBUTING.md](CONTRIBUTING.md)

---

**Document Version**: 1.0  
**Last Updated**: December 13, 2025  
**Author**: Randy Northrup  
**Status**: ✅ Ready for Implementation

---
