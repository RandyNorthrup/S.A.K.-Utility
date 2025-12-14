# PXE Boot Panel - Comprehensive Implementation Plan

**Version**: 1.0  
**Date**: December 13, 2025  
**Status**: Planning Phase  
**Target Release**: v1.0.0

---

## 🎯 Executive Summary

The PXE Boot Panel transforms a PC running S.A.K. Utility into a network boot server, enabling technicians to deploy Windows installations or boot the SAK WinPE environment across multiple client PCs simultaneously without physical USB drives. This is essential for mass deployments, lab environments, and rapid troubleshooting scenarios.

### Key Objectives
- ✅ **PXE Server** - Host DHCP + TFTP + HTTP services for network booting
- ✅ **Windows ISO Deployment** - Network install Windows from ISO files
- ✅ **SAK WinPE Boot** - Boot SAK Utility via network (no USB required)
- ✅ **Multi-Client Support** - Service 10+ simultaneous client boots
- ✅ **Boot Menu** - iPXE menu for client selection (Windows vs SAK)
- ✅ **Zero Touch** - Clients boot automatically via PXE

---

## 📊 Project Scope

### What is PXE Boot?

**Preboot Execution Environment (PXE)** is a protocol that allows computers to boot from the network instead of local storage.

**Boot Sequence**:
1. Client PC powers on, no bootable OS found (or PXE set as first boot)
2. Network card's PXE firmware requests IP via DHCP
3. DHCP server responds with IP + TFTP server address + boot filename
4. Client downloads boot loader (e.g., `pxelinux.0`, `ipxe.efi`) via TFTP
5. Boot loader downloads kernel/WIM file via TFTP or HTTP
6. Client boots into downloaded OS (Windows PE, SAK PE, etc.)

**Required Services**:
- **DHCP Server** - Assigns IP addresses + PXE options
- **TFTP Server** - Transfers boot files (slow, UDP-based)
- **HTTP Server** - Transfers large files (fast, TCP-based)
- **SMB/NFS Server** (Optional) - For Windows installation source files

---

## 🎯 Use Cases

### 1. **Mass Windows Deployment**
**Scenario**: IT admin needs to install Windows on 20 new workstations

**Workflow**:
1. Launch SAK Utility on technician PC
2. Open PXE Boot Panel
3. Load Windows 11 ISO
4. Start PXE server
5. Boot all 20 client PCs via network (F12 → Network Boot)
6. Clients download Windows installer via PXE
7. Automated or manual Windows installation

**Benefits**:
- No USB drive needed
- Simultaneous deployments
- Consistent image across all PCs

---

### 2. **Remote Troubleshooting**
**Scenario**: User's PC won't boot, technician needs to diagnose remotely

**Workflow**:
1. Technician starts PXE server with SAK WinPE
2. User powers on PC, enters boot menu (F12)
3. Selects "Network Boot"
4. PC downloads SAK WinPE over network
5. Technician connects via remote desktop to SAK PE
6. Performs diagnostics, data backup, repairs

**Benefits**:
- No physical access needed
- No USB drive shipping
- Quick turnaround

---

### 3. **Lab Environment**
**Scenario**: Training lab with 30 PCs needs frequent OS reimaging

**Workflow**:
1. Dedicated PXE server runs 24/7
2. Students boot PCs from network
3. iPXE boot menu offers:
   - Windows 10 Installation
   - Windows 11 Installation
   - SAK Utility WinPE (for diagnostics)
   - Linux Live USB (optional)
4. Student selects option, OS deploys

**Benefits**:
- Zero maintenance (no USB wear)
- Instant OS switching
- Centralized management

---

### 4. **Emergency Data Recovery Network**
**Scenario**: Multiple PCs in office affected by ransomware

**Workflow**:
1. IT admin sets up PXE server with SAK WinPE
2. All affected PCs boot to SAK PE via network
3. SAK PE scans for encrypted files
4. Backups saved to network share
5. PCs wiped and reimaged via same PXE server

**Benefits**:
- Rapid response (no USB hunting)
- Consistent recovery environment
- Parallel operations

---

## 🏗️ Architecture Overview

### Component Hierarchy

```
PXEBootPanel (QWidget)
├─ PXEServerController (QObject)
│  ├─ State: Stopped / Starting / Running / Stopping
│  ├─ Manages: DHCP, TFTP, HTTP servers
│  └─ Monitors: Client connections, transfer progress
│
├─ DHCPServer (QObject)
│  ├─ QUdpSocket - Port 67 (DHCP server)
│  ├─ IP Pool Management (192.168.50.100-192.168.50.200)
│  ├─ PXE Option 66 (TFTP server address)
│  ├─ PXE Option 67 (Boot filename)
│  └─ Lease tracking
│
├─ TFTPServer (QObject)
│  ├─ QUdpSocket - Port 69 (TFTP)
│  ├─ File transfer (slow, reliable)
│  ├─ Serves: Boot loaders (pxelinux.0, ipxe.efi)
│  └─ RFC 1350 compliant
│
├─ HTTPServer (QTcpServer)
│  ├─ Port 80 or 8080
│  ├─ File transfer (fast)
│  ├─ Serves: WIM files, ISO contents, SAK PE image
│  └─ Range request support (resume downloads)
│
├─ iPXEMenuGenerator (Static Utility)
│  ├─ Generates iPXE boot menu script
│  ├─ Menu options: Windows ISOs, SAK PE, custom entries
│  └─ Syntax: iPXE scripting language
│
├─ WindowsISOExtractor (QObject)
│  ├─ Extracts ISO to temp directory
│  ├─ Identifies boot.wim location
│  └─ Prepares for network boot
│
└─ PXEClientMonitor (QObject)
   ├─ Tracks connected clients (MAC, IP, hostname)
   ├─ Current boot stage (DHCP, TFTP, HTTP)
   └─ Transfer statistics (speed, bytes, ETA)
```

---

## 🛠️ Technical Specifications

### DHCP Server Implementation

**Purpose**: Assign IP addresses and provide PXE boot options

**DHCP Packet Structure**:
```cpp
struct DHCPPacket {
    uint8_t op;           // 1 = BOOTREQUEST, 2 = BOOTREPLY
    uint8_t htype;        // 1 = Ethernet
    uint8_t hlen;         // 6 = MAC address length
    uint8_t hops;         // 0
    uint32_t xid;         // Transaction ID (random)
    uint16_t secs;        // Seconds elapsed
    uint16_t flags;       // 0x8000 = Broadcast
    uint32_t ciaddr;      // Client IP (0.0.0.0 initially)
    uint32_t yiaddr;      // Your IP (assigned by server)
    uint32_t siaddr;      // Server IP (TFTP server)
    uint32_t giaddr;      // Gateway IP
    uint8_t chaddr[16];   // Client MAC address
    char sname[64];       // Server hostname
    char file[128];       // Boot filename (e.g., "ipxe.efi")
    uint8_t options[];    // DHCP options (variable length)
};
```

**Key DHCP Options for PXE**:
```cpp
// Option 53: DHCP Message Type
// 1 = DHCPDISCOVER, 2 = DHCPOFFER, 3 = DHCPREQUEST, 5 = DHCPACK

// Option 54: Server Identifier (PXE server IP)
// Option 51: IP Address Lease Time (seconds)

// Option 66: TFTP Server Name (IP address of TFTP server)
// Example: "192.168.50.1"

// Option 67: Bootfile Name (filename to download via TFTP)
// BIOS: "pxelinux.0" or "undionly.kpxe"
// UEFI: "ipxe.efi" or "bootx64.efi"

// Option 60: Vendor Class Identifier
// "PXEClient" - Indicates PXE client

// Option 93: Client System Architecture
// 0x0000 = BIOS x86
// 0x0007 = UEFI x64
// 0x0009 = UEFI x64 HTTP
```

**DHCP Server Implementation**:
```cpp
class DHCPServer : public QObject {
    Q_OBJECT
public:
    struct DHCPLease {
        QString mac;
        QString ip;
        QString hostname;
        QDateTime expiresAt;
        bool isPXEClient;
    };
    
    explicit DHCPServer(QObject* parent = nullptr);
    
    void start(const QString& interfaceIP, 
               const QString& subnetMask,
               const QString& ipPoolStart,
               const QString& ipPoolEnd,
               const QString& tftpServerIP,
               const QString& bootFilename);
    
    void stop();
    
    QVector<DHCPLease> getActiveLeases() const;
    
Q_SIGNALS:
    void started();
    void stopped();
    void clientConnected(QString mac, QString ip, QString hostname);
    void errorOccurred(QString error);
    
private:
    void handleDHCPPacket(const QByteArray& data, const QHostAddress& sender);
    void sendDHCPOffer(const DHCPPacket& request);
    void sendDHCPAck(const DHCPPacket& request);
    QString allocateIP();
    
    QUdpSocket* m_socket;
    QString m_interfaceIP;
    QString m_subnetMask;
    QString m_gateway;
    QString m_dnsServer;
    QString m_tftpServerIP;
    QString m_bootFilename;
    QVector<DHCPLease> m_leases;
};
```

**DHCP Workflow**:
```
Client                          DHCP Server
  |                                  |
  |  DHCPDISCOVER (broadcast)        |
  |--------------------------------->|
  |                                  | (Find available IP)
  |  DHCPOFFER (broadcast/unicast)   |
  |<---------------------------------|
  |  IP: 192.168.50.100              |
  |  TFTP: 192.168.50.1              |
  |  Boot file: ipxe.efi             |
  |                                  |
  |  DHCPREQUEST (broadcast)         |
  |--------------------------------->|
  |  "I accept this IP"              |
  |                                  |
  |  DHCPACK (broadcast/unicast)     |
  |<---------------------------------|
  |  "Lease confirmed"               |
  |                                  |
```

---

### TFTP Server Implementation

**Purpose**: Transfer boot files (small files, < 10 MB)

**TFTP Protocol** (RFC 1350):
- UDP-based (port 69)
- Simple, no authentication
- Block size: 512 bytes (default), up to 65535 with extensions
- Packet types: RRQ (read request), DATA, ACK, ERROR

**TFTP Packet Types**:
```cpp
enum TFTPOpcode {
    RRQ = 1,    // Read request
    WRQ = 2,    // Write request (not used for PXE)
    DATA = 3,   // Data block
    ACK = 4,    // Acknowledgment
    ERROR = 5   // Error
};

struct TFTPReadRequest {
    uint16_t opcode;     // 1 (RRQ)
    char filename[];     // "ipxe.efi\0"
    char mode[];         // "octet\0" (binary) or "netascii"
};

struct TFTPDataPacket {
    uint16_t opcode;     // 3 (DATA)
    uint16_t blockNum;   // Block number (starts at 1)
    uint8_t data[512];   // Data (last block may be < 512)
};

struct TFTPAckPacket {
    uint16_t opcode;     // 4 (ACK)
    uint16_t blockNum;   // Block number being acknowledged
};
```

**TFTP Server Implementation**:
```cpp
class TFTPServer : public QObject {
    Q_OBJECT
public:
    explicit TFTPServer(QObject* parent = nullptr);
    
    void start(quint16 port = 69);
    void stop();
    void setRootDirectory(const QString& path);
    
Q_SIGNALS:
    void started();
    void stopped();
    void fileRequested(QString filename, QString clientIP);
    void transferProgress(QString filename, QString clientIP, qint64 bytes, qint64 total);
    void transferComplete(QString filename, QString clientIP);
    void errorOccurred(QString error);
    
private:
    void handleReadRequest(const QByteArray& data, const QHostAddress& sender, quint16 port);
    void sendDataBlock(const QString& filename, const QHostAddress& client, quint16 port, quint16 blockNum);
    void sendError(const QHostAddress& client, quint16 port, quint16 errorCode, const QString& message);
    
    struct TFTPTransfer {
        QFile file;
        quint16 currentBlock;
        qint64 totalSize;
        QHostAddress clientIP;
        quint16 clientPort;
    };
    
    QUdpSocket* m_socket;
    QString m_rootDirectory;
    QHash<QString, TFTPTransfer> m_activeTransfers;  // Key: clientIP:port
};
```

**TFTP Workflow**:
```
Client                          TFTP Server
  |                                  |
  |  RRQ: "ipxe.efi", mode="octet"   |
  |--------------------------------->|
  |                                  | (Open file, start transfer)
  |  DATA: Block 1 (512 bytes)       |
  |<---------------------------------|
  |                                  |
  |  ACK: Block 1                    |
  |--------------------------------->|
  |                                  |
  |  DATA: Block 2 (512 bytes)       |
  |<---------------------------------|
  |                                  |
  |  ACK: Block 2                    |
  |--------------------------------->|
  |                                  |
  |  ... (repeat until EOF)          |
  |                                  |
  |  DATA: Block N (< 512 bytes)     |
  |<---------------------------------| (Last block)
  |                                  |
  |  ACK: Block N                    |
  |--------------------------------->|
  |                                  |
```

---

### HTTP Server Implementation

**Purpose**: Transfer large files (fast, TCP-based)

**Features**:
- HTTP/1.1 support
- Range requests (resume downloads)
- Chunked transfer encoding
- MIME type detection
- Directory listing (optional)

**HTTP Server Implementation**:
```cpp
class HTTPServer : public QTcpServer {
    Q_OBJECT
public:
    explicit HTTPServer(QObject* parent = nullptr);
    
    void start(quint16 port = 8080);
    void stop();
    void setRootDirectory(const QString& path);
    void setVirtualPaths(const QMap<QString, QString>& paths);
    
Q_SIGNALS:
    void started();
    void stopped();
    void fileRequested(QString path, QString clientIP);
    void transferProgress(QString path, QString clientIP, qint64 bytes, qint64 total);
    void transferComplete(QString path, QString clientIP);
    
protected:
    void incomingConnection(qintptr socketDescriptor) override;
    
private:
    class HTTPConnection : public QObject {
    public:
        explicit HTTPConnection(qintptr socketDescriptor, HTTPServer* server);
        void handleRequest();
        void sendResponse(int statusCode, const QString& statusText, 
                         const QByteArray& body, const QString& contentType = "text/html");
        void sendFile(const QString& filePath, qint64 rangeStart = 0, qint64 rangeEnd = -1);
        
    private:
        QTcpSocket* m_socket;
        HTTPServer* m_server;
        QString m_requestMethod;
        QString m_requestPath;
        QMap<QString, QString> m_requestHeaders;
    };
    
    QString m_rootDirectory;
    QMap<QString, QString> m_virtualPaths;  // URL path -> filesystem path
    QVector<HTTPConnection*> m_activeConnections;
};
```

**HTTP Request Example**:
```http
GET /windows11/sources/boot.wim HTTP/1.1
Host: 192.168.50.1:8080
Range: bytes=0-1048575
Connection: keep-alive
```

**HTTP Response Example**:
```http
HTTP/1.1 206 Partial Content
Content-Type: application/octet-stream
Content-Length: 1048576
Content-Range: bytes 0-1048575/3221225472
Connection: keep-alive

[Binary data...]
```

---

### iPXE Boot Menu

**iPXE** is a modern, feature-rich boot loader that supports:
- HTTP downloads (much faster than TFTP)
- Boot menus with user selection
- Scripting (conditionals, loops, variables)
- Chainloading other boot loaders

**iPXE Menu Script** (`menu.ipxe`):
```bash
#!ipxe

# Set variables
set server_ip 192.168.50.1
set http_port 8080

# Display menu
:start
menu S.A.K. Utility - Network Boot Menu
item --gap -- ---------------- Windows Installation ----------------
item win11 Windows 11 Pro Installation
item win10 Windows 10 Pro Installation
item --gap -- ---------------- Diagnostics & Recovery ----------------
item sak_pe S.A.K. Utility WinPE (Diagnostics & Data Recovery)
item --gap -- ---------------- Other Options ----------------
item local Boot from local disk
item reboot Reboot
item shell iPXE shell (advanced)
choose --default sak_pe --timeout 30000 target && goto ${target}

:win11
echo Booting Windows 11 Installation...
kernel http://${server_ip}:${http_port}/windows11/sources/boot.wim
imgargs kernel /MININT
boot || goto failed

:win10
echo Booting Windows 10 Installation...
kernel http://${server_ip}:${http_port}/windows10/sources/boot.wim
imgargs kernel /MININT
boot || goto failed

:sak_pe
echo Booting S.A.K. Utility WinPE...
kernel http://${server_ip}:${http_port}/sak_winpe/sources/boot.wim
imgargs kernel /MININT
boot || goto failed

:local
echo Booting from local disk...
exit

:reboot
echo Rebooting...
reboot

:shell
echo Entering iPXE shell...
shell

:failed
echo Boot failed. Press any key to return to menu...
prompt
goto start
```

**iPXE Menu Generator**:
```cpp
class iPXEMenuGenerator {
public:
    struct MenuEntry {
        QString id;
        QString label;
        QString type;  // "windows_iso", "sak_pe", "custom"
        QString bootFile;  // Path to WIM or kernel
        QString args;
    };
    
    static QString generateMenu(const QVector<MenuEntry>& entries,
                                const QString& serverIP,
                                quint16 httpPort,
                                int defaultTimeout = 30000);
};
```

---

### Windows ISO Extraction

**Challenge**: Windows ISOs contain `boot.wim` and `install.wim` files that must be extracted

**Solution**: Extract ISO contents to HTTP server directory

```cpp
class WindowsISOExtractor : public QObject {
    Q_OBJECT
public:
    /**
     * @brief Extract Windows ISO for network boot
     * @param isoPath Path to Windows ISO file
     * @param extractPath Destination directory (will be served via HTTP)
     * @return true if successful
     */
    void extractISO(const QString& isoPath, const QString& extractPath);
    
Q_SIGNALS:
    void progressUpdated(int percent, QString currentFile);
    void extractionComplete(bool success, QString message);
    void errorOccurred(QString error);
    
private:
    void mountISO(const QString& isoPath);
    void copyFiles(const QString& source, const QString& destination);
    void unmountISO();
    void createBootConfig(const QString& extractPath);
};
```

**Extraction Steps**:
```cpp
void WindowsISOExtractor::extractISO(const QString& isoPath, const QString& extractPath) {
    // 1. Mount ISO (Windows 10+)
    QProcess mount;
    mount.start("powershell.exe", QStringList() 
        << "-Command"
        << QString("Mount-DiskImage -ImagePath '%1'").arg(isoPath));
    mount.waitForFinished();
    
    // 2. Get mounted drive letter
    QProcess getMount;
    getMount.start("powershell.exe", QStringList()
        << "-Command"
        << QString("(Get-DiskImage -ImagePath '%1' | Get-Volume).DriveLetter").arg(isoPath));
    getMount.waitForFinished();
    QString driveLetter = QString::fromUtf8(getMount.readAllStandardOutput()).trimmed();
    
    // 3. Copy all files
    QString sourcePath = QString("%1:\\").arg(driveLetter);
    copyFilesRecursive(sourcePath, extractPath);
    
    // 4. Unmount ISO
    QProcess unmount;
    unmount.start("powershell.exe", QStringList()
        << "-Command"
        << QString("Dismount-DiskImage -ImagePath '%1'").arg(isoPath));
    unmount.waitForFinished();
    
    emit extractionComplete(true, "ISO extracted successfully");
}
```

**Extracted Structure**:
```
http_root/
├─ windows11/
│  ├─ boot/
│  │  ├─ bcd
│  │  └─ boot.sdi
│  ├─ sources/
│  │  ├─ boot.wim         # Windows PE boot image
│  │  └─ install.wim      # Windows installation image (4+ GB)
│  ├─ bootmgr
│  └─ bootmgr.efi
│
└─ sak_winpe/
   └─ sources/
      └─ boot.wim          # SAK Utility WinPE image
```

---

## 🎨 User Interface Design

### PXE Boot Panel Layout

```
┌─────────────────────────────────────────────────────────────────────┐
│ PXE Boot Panel                                                      │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────────── ⚙️ SERVER CONFIGURATION ──────────────────┐  │
│  │                                                               │  │
│  │  Network Interface:  [Ethernet - 192.168.50.1] ▼             │  │
│  │  DHCP Range:         [192.168.50.100] - [192.168.50.200]     │  │
│  │  Subnet Mask:        [255.255.255.0]                         │  │
│  │  Gateway:            [192.168.50.1]                          │  │
│  │                                                               │  │
│  │  HTTP Port:          [8080]                                  │  │
│  │  TFTP Port:          [69]   (requires admin)                 │  │
│  │  DHCP Port:          [67]   (requires admin)                 │  │
│  │                                                               │  │
│  │  ☑ Enable DHCP Server   ☑ Enable TFTP Server                │  │
│  │  ☑ Enable HTTP Server   ☐ Enable NFS/SMB (advanced)         │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌───────────────────── 📁 BOOT SOURCES ───────────────────────┐  │
│  │                                                               │  │
│  │  Windows ISOs:                                               │  │
│  │  ┌─────────────────────────────────────────────────────────┐│  │
│  │  │ Name                   | Path                | Status   ││  │
│  │  │────────────────────────────────────────────────────────││  │
│  │  │ Windows 11 Pro 23H2   | D:\ISOs\win11.iso   | ✅ Ready ││  │
│  │  │ Windows 10 Pro 22H2   | D:\ISOs\win10.iso   | ✅ Ready ││  │
│  │  └─────────────────────────────────────────────────────────┘│  │
│  │  [➕ Add Windows ISO]  [➖ Remove]  [🔄 Refresh]             │  │
│  │                                                               │  │
│  │  S.A.K. WinPE Boot:                                          │  │
│  │  ┌─────────────────────────────────────────────────────────┐│  │
│  │  │ ☑ S.A.K. Utility WinPE (v0.9.0)                         ││  │
│  │  │   Source: C:\SAK\WinPE\boot.wim                         ││  │
│  │  │   Size: 892 MB                                          ││  │
│  │  └─────────────────────────────────────────────────────────┘│  │
│  │  [📦 Build WinPE Image]  [📁 Browse...]                     │  │
│  │                                                               │  │
│  │  Custom Boot Entries: (Advanced)                             │  │
│  │  [➕ Add Custom Entry]                                       │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌────────────────────── 🎛️ BOOT MENU ─────────────────────────┐  │
│  │                                                               │  │
│  │  Menu Title: [S.A.K. Utility - Network Boot Menu]           │  │
│  │  Default Option: [S.A.K. Utility WinPE] ▼                   │  │
│  │  Timeout: [30] seconds                                       │  │
│  │                                                               │  │
│  │  Menu Preview:                                               │  │
│  │  ┌─────────────────────────────────────────────────────────┐│  │
│  │  │ S.A.K. Utility - Network Boot Menu                      ││  │
│  │  │ ─────────────────────────────────────────────           ││  │
│  │  │ ─── Windows Installation ───                            ││  │
│  │  │ [1] Windows 11 Pro 23H2                                 ││  │
│  │  │ [2] Windows 10 Pro 22H2                                 ││  │
│  │  │ ─── Diagnostics & Recovery ───                          ││  │
│  │  │ [3] S.A.K. Utility WinPE (Default)                      ││  │
│  │  │ ─── Other Options ───                                   ││  │
│  │  │ [4] Boot from local disk                                ││  │
│  │  │ [5] Reboot                                              ││  │
│  │  └─────────────────────────────────────────────────────────┘│  │
│  │                                                               │  │
│  │  [📝 Edit Menu Script]  [💾 Save Menu]                       │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌──────────────────── 🚀 SERVER CONTROL ──────────────────────┐  │
│  │                                                               │  │
│  │  Status: ⚫ Stopped                                          │  │
│  │                                                               │  │
│  │  [▶️ START PXE SERVER]                                       │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌────────────────── 📊 ACTIVE CONNECTIONS ────────────────────┐  │
│  │                                                               │  │
│  │  Connected Clients: 0                                        │  │
│  │                                                               │  │
│  │  ┌─────────────────────────────────────────────────────────┐│  │
│  │  │ Hostname  | MAC Address      | IP Address   | Status   ││  │
│  │  │──────────────────────────────────────────────────────────││  │
│  │  │ (No clients connected)                                  ││  │
│  │  └─────────────────────────────────────────────────────────┘│  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Server Running State

```
┌─────────────────────────────────────────────────────────────────────┐
│ PXE Boot Panel                                        [? Help]      │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌──────────────────── 🚀 SERVER CONTROL ──────────────────────┐  │
│  │                                                               │  │
│  │  Status: 🟢 Running                                          │  │
│  │                                                               │  │
│  │  Services:                                                   │  │
│  │  ✅ DHCP Server   - Port 67  - 3 leases active              │  │
│  │  ✅ TFTP Server   - Port 69  - 2 active transfers           │  │
│  │  ✅ HTTP Server   - Port 8080 - 1 active download           │  │
│  │                                                               │  │
│  │  Server URLs:                                                │  │
│  │  • TFTP: tftp://192.168.50.1                                │  │
│  │  • HTTP: http://192.168.50.1:8080                           │  │
│  │  • Boot Menu: http://192.168.50.1:8080/menu.ipxe            │  │
│  │                                                               │  │
│  │  [⏸️ PAUSE]  [⏹️ STOP PXE SERVER]  [📋 View Logs]           │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌────────────────── 📊 ACTIVE CONNECTIONS ────────────────────┐  │
│  │                                                               │  │
│  │  Connected Clients: 3                                        │  │
│  │                                                               │  │
│  │  ┌─────────────────────────────────────────────────────────┐│  │
│  │  │ Hostname    | MAC Address       | IP           | Stage ││  │
│  │  │─────────────────────────────────────────────────────────││  │
│  │  │ DESKTOP-01 | 00:1A:2B:3C:4D:5E | 192.168.50.100 | DHCP ││  │
│  │  │ LAPTOP-02  | AA:BB:CC:DD:EE:FF | 192.168.50.101 | HTTP ││◀── Downloading
│  │  │ WORKST-03  | 11:22:33:44:55:66 | 192.168.50.102 | TFTP ││  │
│  │  └─────────────────────────────────────────────────────────┘│  │
│  │                                                               │  │
│  │  Transfer Details: LAPTOP-02                                 │  │
│  │  ┌─────────────────────────────────────────────────────────┐│  │
│  │  │ File: sak_winpe/sources/boot.wim                        ││  │
│  │  │ Progress: ████████████░░░░░░░  60% (535 MB / 892 MB)   ││  │
│  │  │ Speed: 45.2 MB/s  |  ETA: 8 seconds                     ││  │
│  │  └─────────────────────────────────────────────────────────┘│  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌───────────────────── 📋 SERVER LOG ─────────────────────────┐  │
│  │                                                               │  │
│  │  [14:32:15] PXE server started                               │  │
│  │  [14:32:20] DHCP: Lease assigned to DESKTOP-01 (100)        │  │
│  │  [14:32:22] TFTP: DESKTOP-01 requested ipxe.efi             │  │
│  │  [14:32:25] HTTP: DESKTOP-01 downloading menu.ipxe          │  │
│  │  [14:33:10] DHCP: Lease assigned to LAPTOP-02 (101)         │  │
│  │  [14:33:12] TFTP: LAPTOP-02 requested ipxe.efi              │  │
│  │  [14:33:15] HTTP: LAPTOP-02 downloading boot.wim (892 MB)   │  │
│  │  [14:33:45] HTTP: LAPTOP-02 download 60% complete           │  │
│  │                                                               │  │
│  │  [📋 Copy Log]  [💾 Save Log]  [🗑️ Clear]                   │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 🎛️ Client Boot Experience

### BIOS/UEFI Boot Menu (Client PC)

```
Boot Menu (F12)
──────────────────────────────────
1. Hard Drive (Windows)
2. CD/DVD Drive
3. USB Drive
4. Network Boot (PXE)  ◀── User selects this
5. Enter BIOS Setup

Booting from: Network Boot
──────────────────────────────────
```

### PXE Boot Sequence (Client PC)

```
┌──────────────────────────────────────────────┐
│                                              │
│  PXE Boot - Initializing...                 │
│                                              │
│  [1/5] Requesting IP address via DHCP...    │
│  ✅ IP assigned: 192.168.50.100              │
│                                              │
│  [2/5] Downloading boot loader (TFTP)...    │
│  ✅ Downloaded ipxe.efi (256 KB)             │
│                                              │
│  [3/5] Loading iPXE boot loader...          │
│  ✅ iPXE v1.21.1 loaded                      │
│                                              │
│  [4/5] Downloading boot menu (HTTP)...      │
│  ✅ Downloaded menu.ipxe                     │
│                                              │
│  [5/5] Displaying boot menu...              │
│                                              │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│ S.A.K. Utility - Network Boot Menu          │
├──────────────────────────────────────────────┤
│                                              │
│ ─── Windows Installation ───                │
│ 1. Windows 11 Pro 23H2                      │
│ 2. Windows 10 Pro 22H2                      │
│                                              │
│ ─── Diagnostics & Recovery ───              │
│ 3. S.A.K. Utility WinPE ◀── Default         │
│                                              │
│ ─── Other Options ───                       │
│ 4. Boot from local disk                     │
│ 5. Reboot                                   │
│                                              │
│ Auto-boot in 30 seconds...                  │
│ Press any key to stop countdown.            │
│                                              │
└──────────────────────────────────────────────┘

User presses 3 or waits 30 seconds...

┌──────────────────────────────────────────────┐
│                                              │
│  Booting S.A.K. Utility WinPE...            │
│                                              │
│  Downloading boot.wim (892 MB)...           │
│  ████████████░░░░░░░░░░  60%                │
│  45.2 MB/s  |  ETA: 8 seconds               │
│                                              │
└──────────────────────────────────────────────┘

Download completes...

┌──────────────────────────────────────────────┐
│                                              │
│  Windows PE 11                              │
│  Loading...                                 │
│                                              │
│  • Initializing hardware...                 │
│  • Loading drivers...                       │
│  • Starting S.A.K. Utility...               │
│                                              │
└──────────────────────────────────────────────┘

SAK Utility launches in WinPE mode!
```

---

## 📂 File Structure

### New Files to Create

#### Headers (`include/sak/`)

```
pxe_boot_panel.h                   # Main UI panel
pxe_server_controller.h            # Orchestrates DHCP, TFTP, HTTP
dhcp_server.h                      # DHCP server implementation
tftp_server.h                      # TFTP server implementation
http_file_server.h                 # HTTP file server
ipxe_menu_generator.h              # iPXE menu script generator
windows_iso_extractor.h            # Extract Windows ISOs
pxe_client_monitor.h               # Track connected clients
pxe_error_codes.h                  # PXE-specific error codes
```

#### Implementation (`src/`)

```
gui/pxe_boot_panel.cpp
core/pxe_server_controller.cpp
core/dhcp_server.cpp
core/tftp_server.cpp
core/http_file_server.cpp
core/ipxe_menu_generator.cpp
core/windows_iso_extractor.cpp
core/pxe_client_monitor.cpp
```

#### Resources

```
resources/pxe/
├─ ipxe_boot/
│  ├─ ipxe.efi              # UEFI boot loader (download from ipxe.org)
│  ├─ undionly.kpxe         # BIOS boot loader
│  └─ menu_template.ipxe    # Default menu template
│
└─ icons/
   ├─ pxe_server.svg
   ├─ network_boot.svg
   └─ client_connected.svg
```

---

## 🔧 Implementation Phases

### Phase 1: HTTP File Server (Week 1-2)

**Goals**:
- Simple HTTP server for file serving
- Range request support
- Progress tracking

**Tasks**:
1. Implement `HTTPFileServer` with QTcpServer
2. Handle GET requests
3. Serve files from directory
4. Add range request support (for resume)
5. Add MIME type detection
6. Write unit tests

**Acceptance Criteria**:
- ✅ HTTP server serves files
- ✅ Range requests work
- ✅ Progress tracking works
- ✅ Clients can download large files (1+ GB)

---

### Phase 2: TFTP Server (Week 3-4)

**Goals**:
- RFC 1350 compliant TFTP server
- Handle multiple simultaneous transfers

**Tasks**:
1. Implement `TFTPServer` with QUdpSocket
2. Handle RRQ (read request) packets
3. Send DATA packets (512-byte blocks)
4. Handle ACK packets
5. Implement timeout and retry logic
6. Write unit tests

**Acceptance Criteria**:
- ✅ TFTP server transfers files
- ✅ Multiple clients supported
- ✅ Retry logic works
- ✅ Compatible with standard TFTP clients

---

### Phase 3: DHCP Server (Week 5-7)

**Goals**:
- Basic DHCP server with PXE options
- IP address allocation and tracking

**Tasks**:
1. Implement `DHCPServer` with QUdpSocket
2. Parse DHCP packets (DISCOVER, REQUEST)
3. Send DHCP packets (OFFER, ACK)
4. Implement IP address pool management
5. Add PXE-specific options (66, 67)
6. Detect client architecture (BIOS vs UEFI)
7. Write unit tests

**Acceptance Criteria**:
- ✅ DHCP assigns IP addresses
- ✅ PXE clients receive boot options
- ✅ Both BIOS and UEFI clients supported
- ✅ IP lease tracking works

---

### Phase 4: iPXE Integration (Week 8-9)

**Goals**:
- Bundle iPXE boot loaders
- Generate boot menus dynamically

**Tasks**:
1. Download iPXE binaries (ipxe.efi, undionly.kpxe)
2. Implement `iPXEMenuGenerator`
3. Create menu template with variables
4. Add menu customization UI
5. Test boot menu on physical hardware
6. Write documentation

**Acceptance Criteria**:
- ✅ iPXE boot loaders served via TFTP
- ✅ Boot menu displays correctly
- ✅ Menu options work (Windows, SAK PE)
- ✅ Timeout and default selection work

---

### Phase 5: Windows ISO Support (Week 10-11)

**Goals**:
- Extract Windows ISOs
- Serve boot.wim via HTTP
- Boot Windows installer via PXE

**Tasks**:
1. Implement `WindowsISOExtractor`
2. Mount ISO via PowerShell
3. Copy files to HTTP directory
4. Create iPXE boot entry for Windows
5. Test Windows 10 and 11 installation
6. Write documentation

**Acceptance Criteria**:
- ✅ Windows ISO extraction works
- ✅ Windows installer boots via PXE
- ✅ Installation completes successfully
- ✅ Multiple Windows versions supported

---

### Phase 6: SAK WinPE Integration (Week 12-13)

**Goals**:
- Boot SAK WinPE via PXE
- Full SAK functionality over network

**Tasks**:
1. Copy SAK WinPE boot.wim to HTTP directory
2. Create iPXE boot entry for SAK PE
3. Test SAK PE boot via PXE
4. Verify all SAK features work
5. Test network backups from SAK PE
6. Write documentation

**Acceptance Criteria**:
- ✅ SAK WinPE boots via PXE
- ✅ All SAK features work (backup, scanner, etc.)
- ✅ Network connectivity in SAK PE
- ✅ Performance acceptable (< 3 min boot)

---

### Phase 7: UI Implementation (Week 14-15)

**Goals**:
- Complete PXE Boot Panel UI
- Client monitoring
- Configuration management

**Tasks**:
1. Implement `PXEBootPanel` GUI
2. Add server configuration controls
3. Add boot sources management (ISOs, SAK PE)
4. Implement client monitoring table
5. Add real-time transfer progress
6. Add log viewer
7. Write user documentation

**Acceptance Criteria**:
- ✅ UI complete and intuitive
- ✅ All controls functional
- ✅ Real-time updates work
- ✅ User documentation complete

---

### Phase 8: Testing & Polish (Week 16-18)

**Goals**:
- Test on diverse hardware
- Performance optimization
- Stability improvements

**Tasks**:
1. Test on 10+ different PCs (UEFI and BIOS)
2. Test with multiple simultaneous clients (10+)
3. Test large file transfers (4+ GB)
4. Performance tuning (HTTP buffer sizes, etc.)
5. Error handling improvements
6. Security audit
7. Update README.md

**Acceptance Criteria**:
- ✅ Tested on diverse hardware
- ✅ Supports 10+ simultaneous clients
- ✅ No memory leaks
- ✅ Comprehensive documentation

---

**Total Timeline**: 18 weeks (4.5 months)  
**Target Release**: v1.0.0 (Q4 2026)

---

## 📋 Configuration & Settings

### ConfigManager Extensions

Add to `config_manager.h/cpp`:

```cpp
// PXE Server Settings
bool getPXEServerEnabled() const;
void setPXEServerEnabled(bool enabled);

QString getPXEServerInterface() const;  // Network interface (IP)
void setPXEServerInterface(const QString& interfaceIP);

QString getPXEDHCPRangeStart() const;
void setPXEDHCPRangeStart(const QString& ip);

QString getPXEDHCPRangeEnd() const;
void setPXEDHCPRangeEnd(const QString& ip);

QString getPXESubnetMask() const;
void setPXESubnetMask(const QString& mask);

QString getPXEGateway() const;
void setPXEGateway(const QString& gateway);

quint16 getPXEHTTPPort() const;
void setPXEHTTPPort(quint16 port);

quint16 getPXETFTPPort() const;
void setPXETFTPPort(quint16 port);

quint16 getPXEDHCPPort() const;
void setPXEDHCPPort(quint16 port);

bool getPXEDHCPEnabled() const;
void setPXEDHCPEnabled(bool enabled);

bool getPXETFTPEnabled() const;
void setPXETFTPEnabled(bool enabled);

bool getPXEHTTPEnabled() const;
void setPXEHTTPEnabled(bool enabled);

QString getPXEBootMenuTitle() const;
void setPXEBootMenuTitle(const QString& title);

QString getPXEBootMenuDefaultOption() const;
void setPXEBootMenuDefaultOption(const QString& option);

int getPXEBootMenuTimeout() const;
void setPXEBootMenuTimeout(int seconds);

QStringList getPXEWindowsISOs() const;
void setPXEWindowsISOs(const QStringList& isoPaths);

QString getPXESAKWinPEPath() const;
void setPXESAKWinPEPath(const QString& wimPath);

QString getPXEHTTPRootDirectory() const;
void setPXEHTTPRootDirectory(const QString& path);
```

**Default Values**:
```cpp
pxe_server/enabled = false
pxe_server/interface = "192.168.50.1"
pxe_server/dhcp_range_start = "192.168.50.100"
pxe_server/dhcp_range_end = "192.168.50.200"
pxe_server/subnet_mask = "255.255.255.0"
pxe_server/gateway = "192.168.50.1"
pxe_server/http_port = 8080
pxe_server/tftp_port = 69
pxe_server/dhcp_port = 67
pxe_server/dhcp_enabled = true
pxe_server/tftp_enabled = true
pxe_server/http_enabled = true
pxe_server/boot_menu_title = "S.A.K. Utility - Network Boot Menu"
pxe_server/boot_menu_default_option = "sak_pe"
pxe_server/boot_menu_timeout = 30
pxe_server/http_root_directory = "%TEMP%\\SAK_PXE"
```

---

## 🧪 Testing Strategy

### Unit Tests

**test_dhcp_server.cpp**:
- DHCP packet parsing
- IP allocation
- Lease tracking
- PXE options

**test_tftp_server.cpp**:
- File transfer
- Block ACKs
- Timeout handling
- Multiple clients

**test_http_file_server.cpp**:
- GET requests
- Range requests
- Large file transfers
- MIME types

**test_ipxe_menu_generator.cpp**:
- Menu generation
- Variable substitution
- Boot entry formatting

### Integration Tests

**test_pxe_integration.cpp**:
- Full PXE boot sequence (simulated)
- DHCP → TFTP → HTTP flow
- Multiple clients
- Windows ISO boot
- SAK PE boot

### Manual Testing

1. **BIOS PC Boot**:
   - Boot legacy BIOS PC via PXE
   - Verify iPXE boot loader loads
   - Verify menu displays
   - Boot SAK PE

2. **UEFI PC Boot**:
   - Boot UEFI PC via PXE
   - Verify ipxe.efi loads
   - Verify menu displays
   - Boot Windows installer

3. **Multi-Client Test**:
   - Boot 10 PCs simultaneously
   - Monitor server performance
   - Verify all clients boot successfully

4. **Large File Transfer**:
   - Boot Windows 11 (install.wim ~4.5 GB)
   - Monitor transfer speed
   - Verify no corruption

---

## 🚧 Limitations & Challenges

### Technical Limitations

**DHCP Conflicts**:
- ⚠️ Cannot run if existing DHCP server on network
- **Mitigation**: Run on isolated network segment, or use DHCP proxy mode

**Port Requirements**:
- ❌ Ports 67 (DHCP) and 69 (TFTP) require administrator privileges
- ❌ Port 67 conflicts with existing DHCP servers
- **Mitigation**: Require admin elevation, detect conflicts, offer alternatives

**Firewall Blocking**:
- ⚠️ Windows Firewall may block DHCP/TFTP/HTTP
- **Mitigation**: Auto-create firewall rules, guide user to configure manually

**Network Performance**:
- ⚠️ TFTP is slow (~5-10 MB/s max)
- ✅ HTTP is fast (~100+ MB/s on Gigabit LAN)
- **Mitigation**: Use HTTP for large files (boot.wim), TFTP only for small boot loaders

**UEFI Secure Boot**:
- ❌ Custom iPXE boot loaders won't work with Secure Boot enabled
- **Mitigation**: Prompt user to disable Secure Boot in BIOS

### Workarounds

**DHCP Proxy Mode**:
```cpp
// Instead of full DHCP server, respond only to PXE requests
// Let existing DHCP server assign IPs, we only provide PXE options
class DHCPProxyServer : public DHCPServer {
    void handleDHCPDiscover(const DHCPPacket& request) override {
        // Don't offer IP, only provide PXE boot options
        sendProxyDHCPOffer(request);
    }
};
```

**Isolated Network**:
- Use dedicated network adapter on server PC
- Clients connect via switch (no gateway, no internet)
- PXE server has full control

---

## 🎯 Success Metrics

| Metric | Target | Importance |
|--------|--------|------------|
| Boot time (SAK PE) | < 3 minutes | High |
| Boot time (Windows) | < 5 minutes | Medium |
| HTTP transfer speed | > 50 MB/s (Gigabit LAN) | High |
| Simultaneous clients | 10+ | High |
| BIOS compatibility | 95%+ | Critical |
| UEFI compatibility | 95%+ | Critical |
| Setup time | < 5 minutes | Medium |

---

## 🔒 Security Considerations

### Threat Model

**Rogue DHCP Server**:
- ❌ Attacker runs PXE server, hijacks client boots
- **Mitigation**: Use isolated network, MAC address filtering

**Boot Image Tampering**:
- ❌ Attacker replaces boot.wim with malicious image
- **Mitigation**: SHA-256 verification, HTTPS (future), code signing

**Unauthorized Access**:
- ❌ Anyone on network can boot from PXE server
- **Mitigation**: MAC address whitelist, password-protected boot menu

**DHCP Starvation**:
- ❌ Attacker requests all IPs from pool
- **Mitigation**: Rate limiting, IP reservation by MAC

### Best Practices

1. **Isolated Network**: Run PXE server on dedicated network segment
2. **MAC Filtering**: Whitelist known client MAC addresses
3. **Firewall Rules**: Only allow specific IPs to connect
4. **File Integrity**: Verify SHA-256 of boot images before serving
5. **Logging**: Log all DHCP/TFTP/HTTP requests for audit

---

## 💡 Future Enhancements (Post-v1.0)

### v1.1 - Advanced Features

**HTTPS Support**:
- Replace HTTP with HTTPS for encrypted transfers
- Self-signed certificates for internal networks

**Authentication**:
- Password-protected boot menu
- Per-client authentication

**Remote Management**:
- Web UI for remote PXE server management
- REST API for automation

**iPXE Advanced**:
- Custom iPXE builds with embedded scripts
- Branded boot menus with logos

### v1.2 - Enterprise Features

**DHCP Relay**:
- Work with existing DHCP servers via DHCP relay

**PXE Boot from Cloud**:
- Store boot images in cloud (Azure, AWS S3)
- Stream images on-demand

**Multi-Site Management**:
- Manage multiple PXE servers from central console
- Sync boot images across sites

**Unattended Installation**:
- Fully automated Windows installation with answer files
- Post-install script execution

---

## 📚 Resources

### Official Documentation

- [PXE Specification](https://www.intel.com/content/dam/doc/product-specification/preboot-execution-environment-pxe-specification.pdf)
- [DHCP RFC 2131](https://tools.ietf.org/html/rfc2131)
- [TFTP RFC 1350](https://tools.ietf.org/html/rfc1350)
- [iPXE Documentation](https://ipxe.org/docs)
- [WinPE Network Boot](https://learn.microsoft.com/windows-hardware/manufacture/desktop/winpe-network-drivers-initializing-and-adding-drivers)

### Community Resources

- [iPXE Boot Menu Examples](https://gist.github.com/robinsmidsrod/2234639)
- [Windows Deployment Services](https://learn.microsoft.com/windows/deployment/deploy-windows-with-wds)
- [FOG Project](https://fogproject.org/) - Open-source PXE deployment
- [Serva PXE Server](https://www.vercot.com/~serva/) - Windows PXE server

---

## 📞 Support

**Questions?** Open a GitHub Discussion  
**Found a Bug?** Open a GitHub Issue  
**Want to Contribute?** See [CONTRIBUTING.md](CONTRIBUTING.md)

---

**Document Version**: 1.0  
**Last Updated**: December 13, 2025  
**Author**: Randy Northrup  
**Status**: ✅ Ready for Implementation

---
