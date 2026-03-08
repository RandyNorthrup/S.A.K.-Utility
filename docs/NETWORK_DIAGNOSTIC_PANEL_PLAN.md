# Network Diagnostics & Troubleshooting Panel - Comprehensive Implementation Plan

**Version**: 1.0  
**Date**: February 25, 2026  
**Status**: ✅ Complete  
**Completed in**: v0.8.5

---

## 🎯 Executive Summary

The Network Diagnostics & Troubleshooting Panel provides enterprise-grade network analysis and troubleshooting capabilities directly within S.A.K. Utility. It enables technicians to rapidly diagnose connectivity issues, measure LAN/WAN performance, map network topology, inspect DNS/DHCP configuration, scan for open ports, analyze WiFi environments, and audit firewall rules — all without installing separate tools. This panel eliminates the need to carry a separate toolkit of command-line utilities by integrating them into a unified, visual interface.

### Key Objectives
- ✅ **Network Adapter Inspector** - Full adapter enumeration with IP config, MAC, link speed, driver info
- ✅ **Connectivity Testing** - Visual ping, traceroute, and MTR-style combined diagnostics
- ✅ **DNS Diagnostics** - Forward/reverse lookup, DNS server testing, record type queries, cache inspection
- ✅ **Port Scanner** - TCP connect scanning with service fingerprinting for LAN/WAN targets
- ✅ **LAN Bandwidth Testing** - iPerf3-based throughput measurement between SAK instances via bundled `iperf3.exe`
- ✅ **Internet Speed Test** - HTTP-based download/upload speed measurement using public endpoints
- ✅ **WiFi Analyzer** - SSID discovery, signal strength, channel utilization, security assessment
- ✅ **Active Connections Monitor** - Real-time view of TCP/UDP connections with process mapping
- ✅ **Firewall Rule Auditor** - Windows Firewall rule enumeration with conflict/gap analysis
- ✅ **Network Share Browser** - Discover and test access to SMB shares on the local network
- ✅ **Report Generation** - Professional HTML/JSON diagnostic reports

---

## 📊 Project Scope

### What is Network Diagnostics & Troubleshooting?

**Network Diagnostics** is the systematic process of testing and analyzing network components (adapters, protocols, services, routes) to identify the root cause of connectivity failures, performance degradation, or security misconfigurations.

**Diagnostic Workflow**:
1. Technician opens Network Diagnostics Panel
2. Panel auto-discovers all network adapters and current configuration
3. Technician selects specific diagnostic tools based on the reported issue
4. Tools provide visual, real-time results with pass/fail indicators
5. Results are aggregated into a diagnostic report

**Key Data Sources (Windows)**:
- **Windows IP Helper API** (`iphlpapi.dll`) - Adapter info, routing tables, ARP cache, TCP/UDP tables
- **Native WiFi API** (`wlanapi.dll`) - WiFi scanning, signal strength, channel info
- **ICMP API** (`icmp.dll`) - Ping/traceroute via `IcmpSendEcho2`
- **DNS API** (`dnsapi.dll`) - DNS resolution, record queries
- **Windows Firewall API** (`netfw`) - Firewall rule enumeration
- **Qt Network Module** - DNS lookup, TCP/UDP sockets, SSL info
- **PowerShell** - `Get-NetAdapter`, `Get-NetIPConfiguration`, `Get-NetFirewallRule`, `netsh wlan`
- **iPerf3** (bundled) - LAN/WAN bandwidth measurement

---

## 🎯 Use Cases

### 1. **"Internet Not Working" Troubleshooting**
**Scenario**: Customer reports "I can't access the internet." Technician needs to pinpoint the failure.

**Workflow**:
1. Open Network Diagnostics Panel
2. **Adapter Inspector**: Ethernet shows "Connected" with IP 169.254.x.x (APIPA) → DHCP failure
3. **Connectivity Test**: Ping gateway fails → no L3 connectivity
4. **DNS Test**: DNS resolution fails → expected since no IP
5. **Diagnosis**: DHCP server unreachable. Check cable → check DHCP server → check switch port
6. After fixing: Re-run tests → all pass → generate report for ticket

**Benefits**:
- Systematic layer-by-layer diagnosis (L1 → L2 → L3 → DNS → Application)
- Visual indicators immediately show where the chain breaks
- No command-line knowledge required

---

### 2. **Slow Network Performance**
**Scenario**: Users report file transfers to NAS are "really slow" (should be Gigabit).

**Workflow**:
1. Open Network Diagnostics Panel
2. **Adapter Inspector**: Shows link speed = 100 Mbps (should be 1000) → bad cable or autoneg issue
3. After fixing cable: Link speed = 1000 Mbps
4. **LAN Bandwidth Test**: Run iPerf3 between workstation and NAS (SAK on both) → measures 920 Mbps throughput
5. **Confirmed**: Problem was cable/link negotiation, now resolved
6. Generate performance report

**Benefits**:
- Immediately identifies link speed negotiation issues
- iPerf3 provides definitive bandwidth measurement
- Before/after proof for the customer

---

### 3. **WiFi Coverage Assessment**
**Scenario**: Office is expanding, need to assess WiFi coverage before adding access points.

**Workflow**:
1. Walk through office with laptop running SAK Utility
2. Open Network Diagnostics → WiFi Analyzer
3. At each location, record: SSID, signal strength (dBm), channel, noise
4. WiFi Analyzer shows channel utilization — channels 1, 6, 11 heat map
5. Identifies: 3 neighboring networks on channel 6 causing interference
6. Recommendation: New AP on channel 1 in east wing

**Benefits**:
- No need for dedicated WiFi survey tool ($500+)
- Visual channel utilization shows interference
- Signal strength history helps map coverage

---

### 4. **Firewall Blocking Application**
**Scenario**: New business application can't connect to its cloud API (HTTPS port 443).

**Workflow**:
1. Open Network Diagnostics Panel
2. **Port Scanner**: Scan api.example.com port 443 → "Filtered" (firewall blocking)
3. **Firewall Rule Auditor**: Search rules for port 443 → find explicit Block rule for the application
4. **Active Connections**: App shows "SYN_SENT" (connection attempt timing out)
5. **Fix**: Create allow rule for the app
6. **Verify**: Port scan now shows "Open", app connects successfully

**Benefits**:
- Identifies firewall as root cause quickly
- Auditor pinpoints the exact blocking rule
- Active connections confirm the app's connection state

---

### 5. **DNS Resolution Issues**
**Scenario**: Some websites load, others don't. Intermittent "DNS_PROBE_FINISHED_NXDOMAIN" errors.

**Workflow**:
1. Open Network Diagnostics Panel
2. **DNS Diagnostics**: Query failing domain against configured DNS (ISP's DNS) → NXDOMAIN
3. **DNS Diagnostics**: Query same domain against 8.8.8.8 (Google DNS) → resolves correctly
4. **DNS Diagnostics**: Query against 1.1.1.1 (Cloudflare DNS) → resolves correctly
5. **Diagnosis**: ISP DNS server is failing for some domains
6. **Fix**: Change DNS to 8.8.8.8 / 1.1.1.1
7. **Verify**: All domains now resolve. Generate report.

**Benefits**:
- Compare multiple DNS servers side-by-side
- Query specific record types (A, AAAA, MX, CNAME, TXT, SOA)
- Immediate root cause identification

---

### 6. **Pre-Deployment Network Readiness Check**
**Scenario**: Before deploying 20 new workstations, verify network infrastructure is ready.

**Workflow**:
1. From a test workstation, open Network Diagnostics Panel
2. **Adapter Inspector**: Verify Gigabit link on each switch port
3. **Connectivity Test**: Ping domain controller, file server, internet gateway, DNS
4. **DNS Diagnostics**: Verify AD DNS resolution (SRV records for _ldap._tcp)
5. **Port Scanner**: Verify ports 445 (SMB), 389 (LDAP), 3389 (RDP), 80/443 (web) accessible
6. **LAN Bandwidth Test**: Measure throughput to file server → confirm Gigabit performance
7. **Network Share Browser**: Verify access to deployment and user shares
8. Generate "Network Readiness Report" → attach to deployment plan

**Benefits**:
- Comprehensive pre-deployment checklist
- Catches infrastructure issues before deployment day
- Documentation for project records

---

## 🏗️ Architecture Overview

### Component Hierarchy

```
NetworkDiagnosticPanel (QWidget)
├─ NetworkDiagnosticController (QObject)
│  ├─ State: Idle / Scanning / Testing / Monitoring
│  ├─ Manages: All diagnostic workers
│  └─ Aggregates: Results for report generation
│
├─ NetworkAdapterInspector (QObject) [Worker Thread]
│  ├─ GetAdaptersAddresses API → Full adapter enumeration
│  ├─ Adapter: Name, Description, MAC, Type (Ethernet/WiFi/VPN)
│  ├─ IPv4/IPv6 addresses, subnet, gateway, DNS servers
│  ├─ Link speed, media state
│  ├─ Driver: name, version, date
│  └─ Output: QVector<NetworkAdapterInfo>
│
├─ ConnectivityTester (QObject) [Worker Thread]
│  ├─ Ping (ICMP Echo) via IcmpSendEcho2
│  ├─ Traceroute via ICMP TTL expiration
│  ├─ MTR mode (continuous traceroute with statistics)
│  ├─ Configurable: target, count, interval, timeout, packet size
│  └─ Output: PingResult, TracerouteResult
│
├─ DnsDiagnosticTool (QObject) [Worker Thread]
│  ├─ Forward lookup (hostname → IP)
│  ├─ Reverse lookup (IP → hostname)
│  ├─ Record type queries (A, AAAA, MX, CNAME, TXT, SOA, SRV, NS, PTR)
│  ├─ Multi-server comparison (test same query against multiple DNS servers)
│  ├─ DNS cache inspection (ipconfig /displaydns parsing)
│  ├─ Response time measurement per server
│  └─ Output: DnsQueryResult
│
├─ PortScanner (QObject) [Worker Thread]
│  ├─ TCP Connect scan (reliable, no raw sockets needed)
│  ├─ Common port presets (Web, Email, Database, Infrastructure, Custom)
│  ├─ Port range scanning (1-65535)
│  ├─ Service fingerprinting (banner grabbing)
│  ├─ Configurable: timeout, concurrent connections, retry
│  └─ Output: QVector<PortScanResult>
│
├─ BandwidthTester (QObject) [Worker Thread]
│  ├─ LAN: iPerf3 client/server via bundled iperf3.exe
│  ├─ WAN: HTTP download speed test (configurable endpoint)
│  ├─ Bidirectional throughput (upload + download)
│  ├─ Duration-based testing (default 10 seconds)
│  ├─ Real-time throughput graph
│  └─ Output: BandwidthTestResult
│
├─ WiFiAnalyzer (QObject) [Worker Thread]
│  ├─ Native WiFi API (wlanapi.dll) → SSID scan
│  ├─ Signal strength (dBm + quality %)
│  ├─ Channel and frequency (2.4 GHz / 5 GHz / 6 GHz)
│  ├─ Security: Open / WPA2 / WPA3 / WEP
│  ├─ Channel utilization analysis
│  ├─ BSS type (Infrastructure / Ad-Hoc)
│  └─ Output: QVector<WiFiNetworkInfo>
│
├─ ActiveConnectionsMonitor (QObject) [Worker Thread, Polling]
│  ├─ GetExtendedTcpTable → TCP connections with process ID
│  ├─ GetExtendedUdpTable → UDP listeners with process ID
│  ├─ Process name resolution via OpenProcess + GetModuleFileName
│  ├─ State tracking: LISTEN, ESTABLISHED, TIME_WAIT, SYN_SENT, etc.
│  ├─ Real-time refresh (configurable interval)
│  └─ Output: QVector<ConnectionInfo>
│
├─ FirewallRuleAuditor (QObject) [Worker Thread]
│  ├─ INetFwPolicy2 COM interface → enumerate all rules
│  ├─ Rule: Name, Direction, Action, Protocol, Port, Program, Profile
│  ├─ Conflict detection (overlapping allow/block rules)
│  ├─ Gap analysis (commonly needed ports not explicitly allowed)
│  ├─ Search/filter by port, program, direction
│  └─ Output: QVector<FirewallRule>, QVector<FirewallConflict>
│
├─ NetworkShareBrowser (QObject) [Worker Thread]
│  ├─ NetShareEnum API → enumerate shares on target host
│  ├─ WNetEnumResource → discover network resources
│  ├─ Access testing (can current user read/write)
│  ├─ Permission display
│  └─ Output: QVector<NetworkShareInfo>
│
└─ NetworkDiagnosticReportGenerator (QObject)
   ├─ Aggregates all diagnostic results
   ├─ Pass/Fail indicators per test
   ├─ Generates: HTML (printable), JSON (machine-readable)
   └─ Recommendations engine
```

---

## 🛠️ Technical Specifications

### Network Adapter Inspector

**Purpose**: Enumerate all network adapters with complete configuration details.

**Data Structures**:
```cpp
struct NetworkAdapterInfo {
    // Identity
    QString name;              // "Ethernet 2"
    QString description;       // "Intel(R) I225-V"
    QString adapterType;       // "Ethernet" / "WiFi" / "VPN" / "Loopback"
    QString macAddress;        // "00:1A:2B:3C:4D:5E"
    uint32_t interfaceIndex;   // Windows interface index
    
    // Status
    bool isConnected;          // Media connected
    uint64_t linkSpeedBps;     // e.g., 1000000000 (1 Gbps)
    QString mediaState;        // "Connected" / "Disconnected"
    
    // IPv4 Configuration
    QVector<QString> ipv4Addresses;
    QVector<QString> ipv4SubnetMasks;
    QString ipv4Gateway;
    QVector<QString> ipv4DnsServers;
    bool dhcpEnabled;
    QString dhcpServer;
    QDateTime dhcpLeaseObtained;
    QDateTime dhcpLeaseExpires;
    
    // IPv6 Configuration
    QVector<QString> ipv6Addresses;
    QString ipv6Gateway;
    QVector<QString> ipv6DnsServers;
    
    // Driver Info
    QString driverName;
    QString driverVersion;
    QString driverDate;
    
    // Statistics
    uint64_t bytesReceived;
    uint64_t bytesSent;
    uint64_t packetsReceived;
    uint64_t packetsSent;
    uint64_t errorsReceived;
    uint64_t errorsSent;
};
```

**Implementation**:
```cpp
class NetworkAdapterInspector : public QObject {
    Q_OBJECT
public:
    explicit NetworkAdapterInspector(QObject* parent = nullptr);
    
    void scan();
    void refresh();
    
Q_SIGNALS:
    void scanComplete(QVector<NetworkAdapterInfo> adapters);
    void errorOccurred(QString error);
    
private:
    QVector<NetworkAdapterInfo> m_adapters;
    
    QVector<NetworkAdapterInfo> enumerateAdapters();
    
    // Uses GetAdaptersAddresses (IP_ADAPTER_ADDRESSES)
    // with GAA_FLAG_INCLUDE_ALL_INTERFACES for complete enumeration
};
```

**Windows API Usage**:
```cpp
QVector<NetworkAdapterInfo> NetworkAdapterInspector::enumerateAdapters() {
    ULONG bufferSize = 15000;
    auto buffer = std::make_unique<uint8_t[]>(bufferSize);
    auto addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.get());
    
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &bufferSize);
    
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer = std::make_unique<uint8_t[]>(bufferSize);
        addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.get());
        result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &bufferSize);
    }
    
    QVector<NetworkAdapterInfo> adapters;
    if (result == NO_ERROR) {
        for (auto addr = addresses; addr != nullptr; addr = addr->Next) {
            NetworkAdapterInfo info;
            info.name = QString::fromWCharArray(addr->FriendlyName);
            info.description = QString::fromWCharArray(addr->Description);
            info.macAddress = formatMacAddress(addr->PhysicalAddress, addr->PhysicalAddressLength);
            info.isConnected = (addr->OperStatus == IfOperStatusUp);
            info.linkSpeedBps = addr->TransmitLinkSpeed;
            info.dhcpEnabled = (addr->Flags & IP_ADAPTER_DHCP_ENABLED);
            // ... parse unicast addresses, DNS servers, gateway, etc.
            adapters.append(info);
        }
    }
    return adapters;
}
```

---

### Connectivity Tester (Ping / Traceroute / MTR)

**Purpose**: Test network connectivity with visual ping, traceroute, and combined MTR-style analysis.

**Data Structures**:
```cpp
struct PingResult {
    QString target;            // Hostname or IP
    QString resolvedIP;        // Resolved IP address
    
    struct PingReply {
        int sequenceNumber;
        bool success;
        double rttMs;          // Round-trip time (ms)
        int ttl;
        QString replyFrom;     // IP of responder
        QString errorMessage;  // If !success
    };
    
    QVector<PingReply> replies;
    
    // Statistics
    int sent;
    int received;
    int lost;
    double lossPercent;
    double minRtt;
    double maxRtt;
    double avgRtt;
    double jitter;             // Standard deviation of RTT
};

struct TracerouteHop {
    int hopNumber;
    QString ipAddress;
    QString hostname;          // Reverse DNS (if available)
    double rtt1Ms;             // First probe
    double rtt2Ms;             // Second probe
    double rtt3Ms;             // Third probe
    double avgRttMs;
    bool timedOut;             // All probes timed out ("* * *")
    QString asn;               // Autonomous System Number (optional)
};

struct TracerouteResult {
    QString target;
    QString resolvedIP;
    QVector<TracerouteHop> hops;
    bool reachedTarget;
    int totalHops;
};

// MTR (My Traceroute) - combined continuous ping + traceroute
struct MtrHopStats {
    int hopNumber;
    QString ipAddress;
    QString hostname;
    int sent;
    int received;
    double lossPercent;
    double lastRttMs;
    double avgRttMs;
    double bestRttMs;
    double worstRttMs;
    double jitterMs;
};

struct MtrResult {
    QString target;
    QVector<MtrHopStats> hops;
    QDateTime startTime;
    int totalCycles;
};
```

**Implementation**:
```cpp
class ConnectivityTester : public QObject {
    Q_OBJECT
public:
    struct PingConfig {
        QString target;
        int count = 10;              // Number of pings
        int intervalMs = 1000;       // Interval between pings
        int timeoutMs = 4000;        // Per-ping timeout
        int packetSizeBytes = 32;    // ICMP payload size
        int ttl = 128;               // Time to Live
        bool resolveHostnames = true;
    };
    
    struct TracerouteConfig {
        QString target;
        int maxHops = 30;
        int timeoutMs = 5000;
        int probesPerHop = 3;
        bool resolveHostnames = true;
    };
    
    struct MtrConfig {
        QString target;
        int cycles = 100;            // Number of ping cycles
        int intervalMs = 1000;
        int maxHops = 30;
        int timeoutMs = 5000;
    };
    
    explicit ConnectivityTester(QObject* parent = nullptr);
    
    void ping(const PingConfig& config);
    void traceroute(const TracerouteConfig& config);
    void mtr(const MtrConfig& config);
    void cancel();
    
Q_SIGNALS:
    // Ping signals (real-time per-reply)
    void pingReply(int sequence, bool success, double rttMs, int ttl, QString from);
    void pingComplete(PingResult result);
    
    // Traceroute signals (real-time per-hop)
    void tracerouteHop(TracerouteHop hop);
    void tracerouteComplete(TracerouteResult result);
    
    // MTR signals (real-time updates)
    void mtrUpdate(QVector<MtrHopStats> hops, int cycle);
    void mtrComplete(MtrResult result);
    
    void errorOccurred(QString error);
    
private:
    std::atomic<bool> m_cancelled{false};
    
    // ICMP via Windows API (IcmpSendEcho2)
    PingResult::PingReply sendIcmpEcho(const QString& target, int timeoutMs, 
                                       int packetSize, int ttl);
    
    // Traceroute via incremental TTL
    TracerouteHop probeHop(const QString& target, int ttl, int timeoutMs, int probes);
};
```

**ICMP Ping Implementation (Windows API)**:
```cpp
PingResult::PingReply ConnectivityTester::sendIcmpEcho(
    const QString& target, int timeoutMs, int packetSize, int ttl) 
{
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        return {0, false, 0, 0, "", "Failed to create ICMP handle"};
    }
    
    // Resolve hostname
    IN_ADDR destAddr;
    // ... DNS resolution via getaddrinfo
    
    // Set TTL via IP_OPTION_INFORMATION
    IP_OPTION_INFORMATION options{};
    options.Ttl = static_cast<UCHAR>(ttl);
    
    // Send data buffer
    auto sendData = std::make_unique<char[]>(packetSize);
    std::fill_n(sendData.get(), packetSize, 'A');
    
    // Reply buffer
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + packetSize;
    auto replyBuffer = std::make_unique<char[]>(replySize);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    DWORD numReplies = IcmpSendEcho(
        hIcmp, destAddr.S_un.S_addr,
        sendData.get(), static_cast<WORD>(packetSize),
        &options, replyBuffer.get(), replySize,
        static_cast<DWORD>(timeoutMs));
    
    auto end = std::chrono::high_resolution_clock::now();
    double rtt = std::chrono::duration<double, std::milli>(end - start).count();
    
    IcmpCloseHandle(hIcmp);
    
    if (numReplies > 0) {
        auto reply = reinterpret_cast<PICMP_ECHO_REPLY>(replyBuffer.get());
        IN_ADDR replyAddr;
        replyAddr.S_un.S_addr = reply->Address;
        
        return {0, true, static_cast<double>(reply->RoundTripTime), 
                reply->Options.Ttl, 
                QString::fromLatin1(inet_ntoa(replyAddr)), ""};
    }
    
    return {0, false, 0, 0, "", "Request timed out"};
}
```

---

### DNS Diagnostic Tool

**Purpose**: Comprehensive DNS testing with multi-server comparison and record type queries.

**Data Structures**:
```cpp
struct DnsQueryResult {
    QString queryName;           // "example.com"
    QString recordType;          // "A" / "AAAA" / "MX" / "CNAME" / "TXT" / "SOA" / "NS" / "SRV" / "PTR"
    QString dnsServer;           // Server used for query
    
    bool success;
    double responseTimeMs;
    
    QVector<QString> answers;    // Resolved addresses/values
    QString errorMessage;
    
    // Raw details
    int ttlSeconds;
    QString authoritySection;
    
    QDateTime queryTimestamp;
};

struct DnsServerComparison {
    QString queryName;
    QString recordType;
    QVector<DnsQueryResult> results;  // One per DNS server tested
    
    // Analysis
    bool allAgree;               // All servers return same answers
    QString fastestServer;       // Lowest response time
    double fastestTimeMs;
};
```

**Implementation**:
```cpp
class DnsDiagnosticTool : public QObject {
    Q_OBJECT
public:
    explicit DnsDiagnosticTool(QObject* parent = nullptr);
    
    // Single query
    void query(const QString& hostname, const QString& recordType = "A",
               const QString& dnsServer = ""); // Empty = system default
    
    // Reverse lookup
    void reverseLookup(const QString& ipAddress, const QString& dnsServer = "");
    
    // Multi-server comparison
    void compareServers(const QString& hostname, const QString& recordType,
                        const QStringList& dnsServers);
    
    // DNS cache inspection
    void inspectDnsCache();
    
    // Flush DNS cache
    void flushDnsCache();
    
    void cancel();
    
    // Well-known DNS servers
    static QVector<QPair<QString, QString>> wellKnownDnsServers() {
        return {
            {"System Default", ""},
            {"Google DNS", "8.8.8.8"},
            {"Google DNS (Secondary)", "8.8.4.4"},
            {"Cloudflare", "1.1.1.1"},
            {"Cloudflare (Secondary)", "1.0.0.1"},
            {"Quad9", "9.9.9.9"},
            {"Quad9 (Secondary)", "149.112.112.112"},
            {"OpenDNS", "208.67.222.222"},
            {"OpenDNS (Secondary)", "208.67.220.220"},
        };
    }
    
Q_SIGNALS:
    void queryComplete(DnsQueryResult result);
    void comparisonComplete(DnsServerComparison comparison);
    void dnsCacheResults(QVector<QPair<QString, QString>> entries);
    void dnsCacheFlushed();
    void errorOccurred(QString error);
    
private:
    // Use Qt's QDnsLookup for record-type queries
    // Use DnsQuery_W API for specific server targeting
    DnsQueryResult performQuery(const QString& hostname, const QString& recordType,
                                 const QString& dnsServer);
};
```

**DNS Query with Specific Server (Windows API)**:
```cpp
DnsQueryResult DnsDiagnosticTool::performQuery(
    const QString& hostname, const QString& recordType, const QString& dnsServer) 
{
    DnsQueryResult result;
    result.queryName = hostname;
    result.recordType = recordType;
    result.dnsServer = dnsServer.isEmpty() ? "System Default" : dnsServer;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    WORD type = DNS_TYPE_A;
    if (recordType == "AAAA") type = DNS_TYPE_AAAA;
    else if (recordType == "MX") type = DNS_TYPE_MX;
    else if (recordType == "CNAME") type = DNS_TYPE_CNAME;
    else if (recordType == "TXT") type = DNS_TYPE_TEXT;
    else if (recordType == "SOA") type = DNS_TYPE_SOA;
    else if (recordType == "NS") type = DNS_TYPE_NS;
    else if (recordType == "SRV") type = DNS_TYPE_SRV;
    else if (recordType == "PTR") type = DNS_TYPE_PTR;
    
    DNS_QUERY_REQUEST request{};
    request.Version = DNS_QUERY_REQUEST_VERSION1;
    request.QueryName = hostname.toStdWString().c_str();
    request.QueryType = type;
    request.QueryOptions = DNS_QUERY_BYPASS_CACHE; // Always query fresh
    
    // Set custom DNS server if specified
    IP4_ARRAY serverList{};
    if (!dnsServer.isEmpty()) {
        serverList.AddrCount = 1;
        inet_pton(AF_INET, dnsServer.toLatin1().constData(), &serverList.AddrArray[0]);
        // Use legacy DnsQuery_W with pExtra parameter for server override
    }
    
    PDNS_RECORD dnsRecord = nullptr;
    DNS_STATUS status = DnsQuery_W(
        hostname.toStdWString().c_str(),
        type,
        DNS_QUERY_BYPASS_CACHE,
        dnsServer.isEmpty() ? nullptr : &serverList,
        &dnsRecord,
        nullptr);
    
    auto end = std::chrono::high_resolution_clock::now();
    result.responseTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    if (status == 0 && dnsRecord) {
        result.success = true;
        // Parse DNS_RECORD linked list for answers
        for (auto rec = dnsRecord; rec != nullptr; rec = rec->pNext) {
            if (rec->wType == DNS_TYPE_A) {
                IN_ADDR addr;
                addr.S_un.S_addr = rec->Data.A.IpAddress;
                result.answers.append(QString::fromLatin1(inet_ntoa(addr)));
            }
            // ... handle other record types
            result.ttlSeconds = rec->dwTtl;
        }
        DnsRecordListFree(dnsRecord, DnsFreeRecordListDeep);
    } else {
        result.success = false;
        result.errorMessage = QString("DNS query failed with status %1").arg(status);
    }
    
    result.queryTimestamp = QDateTime::currentDateTime();
    return result;
}
```

---

### Port Scanner

**Purpose**: TCP connect scanning for service discovery and firewall verification.

**Data Structures**:
```cpp
struct PortScanResult {
    QString target;
    uint16_t port;
    
    enum State { Open, Closed, Filtered, Error };
    State state;
    
    double responseTimeMs;
    QString serviceName;         // "HTTP" / "HTTPS" / "SSH" / "RDP"
    QString banner;              // Service banner (if retrieved)
    QString errorMessage;
    
    QDateTime scanTimestamp;
};

// Common port presets
struct PortPreset {
    QString name;
    QVector<uint16_t> ports;
};
```

**Implementation**:
```cpp
class PortScanner : public QObject {
    Q_OBJECT
public:
    struct ScanConfig {
        QString target;                 // Hostname or IP
        QVector<uint16_t> ports;        // Specific ports
        uint16_t portRangeStart = 0;    // Range scan (if ports empty)
        uint16_t portRangeEnd = 0;
        int timeoutMs = 3000;           // Per-port timeout
        int maxConcurrent = 50;         // Concurrent connections
        bool grabBanners = true;        // Attempt banner grabbing
    };
    
    explicit PortScanner(QObject* parent = nullptr);
    
    void scan(const ScanConfig& config);
    void cancel();
    
    // Common presets
    static QVector<PortPreset> getPresets() {
        return {
            {"Web Servers", {80, 443, 8080, 8443}},
            {"Email", {25, 110, 143, 465, 587, 993, 995}},
            {"File Sharing", {21, 22, 445, 2049, 3260}},
            {"Remote Access", {22, 23, 3389, 5900, 5985, 5986}},
            {"Database", {1433, 1521, 3306, 5432, 6379, 27017}},
            {"Infrastructure", {53, 67, 68, 69, 161, 162, 389, 636}},
            {"Common Services", {80, 443, 22, 21, 25, 53, 110, 143, 3389, 445}},
            {"Top 100", {/* top 100 most common ports */}},
        };
    }
    
Q_SIGNALS:
    void scanStarted(QString target, int totalPorts);
    void portScanned(PortScanResult result);
    void scanProgress(int scanned, int total);
    void scanComplete(QVector<PortScanResult> results);
    void errorOccurred(QString error);
    
private:
    std::atomic<bool> m_cancelled{false};
    
    PortScanResult scanPort(const QString& target, uint16_t port, 
                            int timeoutMs, bool grabBanner);
    QString grabBanner(QTcpSocket& socket, uint16_t port);
    QString getServiceName(uint16_t port);
    
    // Service name database (well-known ports)
    static const QHash<uint16_t, QString>& serviceDatabase();
};
```

**TCP Connect Scan Implementation**:
```cpp
PortScanResult PortScanner::scanPort(const QString& target, uint16_t port, 
                                      int timeoutMs, bool grabBannerFlag) {
    PortScanResult result;
    result.target = target;
    result.port = port;
    result.serviceName = getServiceName(port);
    
    QTcpSocket socket;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    socket.connectToHost(target, port);
    bool connected = socket.waitForConnected(timeoutMs);
    
    auto end = std::chrono::high_resolution_clock::now();
    result.responseTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    if (connected) {
        result.state = PortScanResult::Open;
        
        // Attempt banner grab
        if (grabBannerFlag) {
            result.banner = grabBanner(socket, port);
        }
        
        socket.disconnectFromHost();
    } else {
        QAbstractSocket::SocketError error = socket.error();
        if (error == QAbstractSocket::ConnectionRefusedError) {
            result.state = PortScanResult::Closed;
        } else if (error == QAbstractSocket::SocketTimeoutError) {
            result.state = PortScanResult::Filtered;
        } else {
            result.state = PortScanResult::Error;
            result.errorMessage = socket.errorString();
        }
    }
    
    result.scanTimestamp = QDateTime::currentDateTime();
    return result;
}
```

---

### Bandwidth Tester (iPerf3)

**Purpose**: Measure LAN/WAN throughput using iPerf3 — the industry-standard bandwidth testing tool.

**Why iPerf3?**
- **Open source** (BSD 3-Clause) — freely redistributable
- **Industry standard** — used by network engineers worldwide
- **JSON output** — `--json` flag provides structured data
- **Bidirectional** — test both directions simultaneously
- **TCP and UDP modes** — measure throughput and jitter
- **Portable** — single executable, no installation required
- **Cross-platform** — works on Windows, Linux, macOS (SAK instances can be on any platform)

**Bundle Strategy**:
```
tools/
└─ iperf3/
   ├─ iperf3.exe             # iPerf3 client/server (~2 MB)
   ├─ cygwin1.dll            # Cygwin runtime (required on Windows)
   ├─ LICENSE                # BSD 3-Clause
   └─ README.txt             # Version and attribution
```

**Data Structures**:
```cpp
struct BandwidthTestResult {
    enum TestMode { LAN_IPERF3, WAN_HTTP };
    TestMode mode;
    
    QString target;            // iPerf3 server or URL
    
    // Throughput
    double downloadMbps;       // Download speed (Mbps)
    double uploadMbps;         // Upload speed (Mbps)
    
    // iPerf3 specific
    double tcpWindowSize;      // Negotiated window size
    double retransmissions;    // TCP retransmits count
    double jitterMs;           // UDP jitter (if UDP mode)
    double packetLossPercent;  // UDP packet loss (if UDP mode)
    
    // Per-interval data (for graphing)
    struct IntervalData {
        double startSec;
        double endSec;
        double bitsPerSecond;
        int retransmits;
    };
    QVector<IntervalData> intervals;
    
    // Test parameters
    int durationSec;
    int parallelStreams;
    bool reverseMode;          // Server sends to client
    
    QDateTime timestamp;
};
```

**Implementation**:
```cpp
class BandwidthTester : public QObject {
    Q_OBJECT
public:
    struct IperfConfig {
        QString serverAddress;       // iPerf3 server IP/hostname
        uint16_t port = 5201;        // Default iPerf3 port
        int durationSec = 10;
        int parallelStreams = 1;
        bool bidirectional = true;   // Test both directions
        bool udpMode = false;
        int udpBandwidthMbps = 100;  // Target UDP bandwidth
    };
    
    explicit BandwidthTester(QObject* parent = nullptr);
    
    // Start iPerf3 server (so another SAK instance can test against this one)
    void startIperfServer(uint16_t port = 5201);
    void stopIperfServer();
    bool isServerRunning() const;
    
    // Run iPerf3 client test against a server
    void runIperfTest(const IperfConfig& config);
    
    // Simple HTTP-based internet speed test
    void runHttpSpeedTest(const QString& downloadUrl = "");
    
    void cancel();
    
Q_SIGNALS:
    // Server signals
    void serverStarted(uint16_t port);
    void serverStopped();
    void serverClientConnected(QString clientIP);
    
    // Client test signals
    void testStarted(QString target);
    void testProgress(double currentMbps, double elapsedSec, double totalSec);
    void testComplete(BandwidthTestResult result);
    
    // HTTP speed test signals
    void httpSpeedTestProgress(double downloadMbps, double uploadMbps);
    void httpSpeedTestComplete(double downloadMbps, double uploadMbps, double latencyMs);
    
    void errorOccurred(QString error);
    
private:
    QString m_iperf3Path;      // Path to bundled iperf3.exe
    QProcess* m_serverProcess = nullptr;
    QProcess* m_clientProcess = nullptr;
    
    BandwidthTestResult parseIperfJson(const QByteArray& json);
};
```

**iPerf3 Invocation**:
```cpp
void BandwidthTester::runIperfTest(const IperfConfig& config) {
    QStringList args;
    args << "-c" << config.serverAddress;
    args << "-p" << QString::number(config.port);
    args << "-t" << QString::number(config.durationSec);
    args << "-P" << QString::number(config.parallelStreams);
    args << "--json";  // JSON output for parsing
    
    if (config.udpMode) {
        args << "-u" << "-b" << QString("%1M").arg(config.udpBandwidthMbps);
    }
    
    if (config.bidirectional) {
        args << "--bidir";
    }
    
    m_clientProcess = new QProcess(this);
    m_clientProcess->start(m_iperf3Path, args);
    
    connect(m_clientProcess, &QProcess::finished, this, [this](int exitCode) {
        QByteArray output = m_clientProcess->readAllStandardOutput();
        BandwidthTestResult result = parseIperfJson(output);
        emit testComplete(result);
    });
}

void BandwidthTester::startIperfServer(uint16_t port) {
    m_serverProcess = new QProcess(this);
    m_serverProcess->start(m_iperf3Path, {"-s", "-p", QString::number(port), "--json"});
    
    if (m_serverProcess->waitForStarted(5000)) {
        emit serverStarted(port);
    } else {
        emit errorOccurred("Failed to start iPerf3 server");
    }
}
```

---

### WiFi Analyzer

**Purpose**: Discover and analyze WiFi networks, signal strength, channel utilization, and security.

**Data Structures**:
```cpp
struct WiFiNetworkInfo {
    // Identity
    QString ssid;              // Network name (empty for hidden)
    QString bssid;             // MAC address of AP
    
    // Signal
    int signalQuality;         // 0-100 (Windows quality %)
    int rssiDbm;               // Signal strength in dBm (-30 to -90)
    
    // Channel / Frequency
    uint32_t channelFrequencyKHz;  // e.g., 2437000 (channel 6)
    int channelNumber;             // e.g., 6
    QString band;                  // "2.4 GHz" / "5 GHz" / "6 GHz"
    int channelWidthMHz;           // 20 / 40 / 80 / 160
    
    // Security
    QString authentication;    // "WPA2-Personal" / "WPA3-Personal" / "Open" / "WEP"
    QString encryption;        // "AES" / "TKIP" / "None"
    bool isSecure;             // true if WPA2+ with AES
    
    // Network
    QString bssType;           // "Infrastructure" / "Ad-Hoc"
    bool isConnected;          // Currently connected to this network
    
    // Vendor
    QString apVendor;          // OUI lookup from BSSID (e.g., "Cisco", "Ubiquiti")
};

struct WiFiChannelUtilization {
    int channelNumber;
    QString band;
    int networkCount;               // Number of networks on this channel
    QVector<QString> ssids;         // SSIDs on this channel
    double averageSignalDbm;        // Average signal of networks on this channel
    double interferenceScore;       // 0-100 (higher = more interference)
};
```

**Implementation**:
```cpp
class WiFiAnalyzer : public QObject {
    Q_OBJECT
public:
    explicit WiFiAnalyzer(QObject* parent = nullptr);
    
    void scan();                     // Trigger new WiFi scan
    void startContinuousScan(int intervalMs = 5000);
    void stopContinuousScan();
    
    QVector<WiFiNetworkInfo> getLastScanResults() const;
    QVector<WiFiChannelUtilization> getChannelUtilization() const;
    
    WiFiNetworkInfo getCurrentConnection() const;
    
Q_SIGNALS:
    void scanComplete(QVector<WiFiNetworkInfo> networks);
    void channelUtilizationUpdated(QVector<WiFiChannelUtilization> channels);
    void errorOccurred(QString error);
    
private:
    QVector<WiFiNetworkInfo> m_lastScan;
    HANDLE m_wlanHandle = nullptr;
    GUID m_interfaceGuid{};
    
    bool initializeWlan();
    void cleanupWlan();
    QVector<WiFiNetworkInfo> performWlanScan();
    QVector<WiFiChannelUtilization> calculateChannelUtilization(
        const QVector<WiFiNetworkInfo>& networks);
    QString lookupVendor(const QString& bssid);
    int frequencyToChannel(uint32_t freqKHz);
    QString frequencyToBand(uint32_t freqKHz);
};
```

**Windows Native WiFi API**:
```cpp
QVector<WiFiNetworkInfo> WiFiAnalyzer::performWlanScan() {
    // Trigger scan
    WlanScan(m_wlanHandle, &m_interfaceGuid, nullptr, nullptr, nullptr);
    
    // Get available networks
    PWLAN_AVAILABLE_NETWORK_LIST networkList = nullptr;
    DWORD result = WlanGetAvailableNetworkList(m_wlanHandle, &m_interfaceGuid,
                                                0, nullptr, &networkList);
    
    // Also get BSS list for detailed RF info
    PWLAN_BSS_LIST bssList = nullptr;
    WlanGetNetworkBssList(m_wlanHandle, &m_interfaceGuid, nullptr,
                          dot11_BSS_type_any, FALSE, nullptr, &bssList);
    
    QVector<WiFiNetworkInfo> networks;
    
    if (bssList) {
        for (DWORD i = 0; i < bssList->dwNumberOfItems; ++i) {
            const WLAN_BSS_ENTRY& bss = bssList->wlanBssEntries[i];
            
            WiFiNetworkInfo info;
            info.ssid = QString::fromUtf8(
                reinterpret_cast<const char*>(bss.dot11Ssid.ucSSID),
                bss.dot11Ssid.uSSIDLength);
            info.bssid = formatMacAddress(bss.dot11Bssid, 6);
            info.rssiDbm = bss.lRssi;
            info.signalQuality = bss.uLinkQuality;
            info.channelFrequencyKHz = bss.ulChCenterFrequency;
            info.channelNumber = frequencyToChannel(bss.ulChCenterFrequency);
            info.band = frequencyToBand(bss.ulChCenterFrequency);
            info.apVendor = lookupVendor(info.bssid);
            
            // Parse security from IE (Information Elements)
            // ... parse RSN/WPA IE for auth/encryption details
            
            networks.append(info);
        }
        WlanFreeMemory(bssList);
    }
    
    if (networkList) WlanFreeMemory(networkList);
    
    return networks;
}
```

---

### Active Connections Monitor

**Purpose**: Real-time TCP/UDP connection monitoring with process identification.

**Data Structures**:
```cpp
struct ConnectionInfo {
    enum Protocol { TCP, UDP };
    Protocol protocol;
    
    QString localAddress;
    uint16_t localPort;
    QString remoteAddress;
    uint16_t remotePort;
    
    // TCP state
    QString state;             // "ESTABLISHED", "LISTEN", "TIME_WAIT", "SYN_SENT", etc.
    
    // Process info
    DWORD processId;
    QString processName;       // "chrome.exe"
    QString processPath;       // "C:\Program Files\Google\Chrome..."
    
    // Resolved names (optional)
    QString remoteHostname;    // Reverse DNS of remote address
    QString serviceName;       // Service name for local/remote port
};
```

**Implementation**:
```cpp
class ActiveConnectionsMonitor : public QObject {
    Q_OBJECT
public:
    struct MonitorConfig {
        int refreshIntervalMs = 2000;
        bool resolveHostnames = false;     // Slow, disabled by default
        bool resolveProcessNames = true;
        Protocol filterProtocol = Protocol::TCP; // TCP, UDP, or both
        QString filterProcessName;          // Filter by process name
        uint16_t filterPort = 0;           // Filter by port (0 = all)
    };
    
    explicit ActiveConnectionsMonitor(QObject* parent = nullptr);
    
    void startMonitoring(const MonitorConfig& config = {});
    void stopMonitoring();
    
    void refreshNow();
    
    QVector<ConnectionInfo> getCurrentConnections() const;
    
Q_SIGNALS:
    void connectionsUpdated(QVector<ConnectionInfo> connections);
    void newConnectionDetected(ConnectionInfo connection);
    void connectionClosed(ConnectionInfo connection);
    void errorOccurred(QString error);
    
private:
    QTimer* m_refreshTimer;
    QVector<ConnectionInfo> m_lastConnections;
    
    QVector<ConnectionInfo> enumerateTcpConnections();
    QVector<ConnectionInfo> enumerateUdpListeners();
    QString getProcessName(DWORD pid);
    QString resolveHostname(const QString& ip);
};
```

**TCP Connection Enumeration (Windows API)**:
```cpp
QVector<ConnectionInfo> ActiveConnectionsMonitor::enumerateTcpConnections() {
    ULONG bufferSize = 0;
    GetExtendedTcpTable(nullptr, &bufferSize, FALSE, AF_INET, 
                        TCP_TABLE_OWNER_PID_ALL, 0);
    
    auto buffer = std::make_unique<uint8_t[]>(bufferSize);
    auto table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.get());
    
    if (GetExtendedTcpTable(table, &bufferSize, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
        return {};
    }
    
    QVector<ConnectionInfo> connections;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        
        ConnectionInfo info;
        info.protocol = ConnectionInfo::TCP;
        
        IN_ADDR localAddr, remoteAddr;
        localAddr.S_un.S_addr = row.dwLocalAddr;
        remoteAddr.S_un.S_addr = row.dwRemoteAddr;
        
        info.localAddress = QString::fromLatin1(inet_ntoa(localAddr));
        info.localPort = ntohs(static_cast<uint16_t>(row.dwLocalPort));
        info.remoteAddress = QString::fromLatin1(inet_ntoa(remoteAddr));
        info.remotePort = ntohs(static_cast<uint16_t>(row.dwRemotePort));
        info.processId = row.dwOwningPid;
        info.processName = getProcessName(row.dwOwningPid);
        
        // Map TCP state
        switch (row.dwState) {
            case MIB_TCP_STATE_ESTAB: info.state = "ESTABLISHED"; break;
            case MIB_TCP_STATE_LISTEN: info.state = "LISTEN"; break;
            case MIB_TCP_STATE_TIME_WAIT: info.state = "TIME_WAIT"; break;
            case MIB_TCP_STATE_SYN_SENT: info.state = "SYN_SENT"; break;
            case MIB_TCP_STATE_SYN_RCVD: info.state = "SYN_RCVD"; break;
            case MIB_TCP_STATE_CLOSE_WAIT: info.state = "CLOSE_WAIT"; break;
            case MIB_TCP_STATE_FIN_WAIT1: info.state = "FIN_WAIT_1"; break;
            case MIB_TCP_STATE_FIN_WAIT2: info.state = "FIN_WAIT_2"; break;
            case MIB_TCP_STATE_CLOSING: info.state = "CLOSING"; break;
            case MIB_TCP_STATE_LAST_ACK: info.state = "LAST_ACK"; break;
            case MIB_TCP_STATE_CLOSED: info.state = "CLOSED"; break;
            default: info.state = "UNKNOWN"; break;
        }
        
        connections.append(info);
    }
    
    return connections;
}
```

---

### Firewall Rule Auditor

**Purpose**: Enumerate Windows Firewall rules, detect conflicts, and identify gaps.

**Data Structures**:
```cpp
struct FirewallRule {
    QString name;
    QString description;
    bool enabled;
    
    enum Direction { Inbound, Outbound };
    Direction direction;
    
    enum Action { Allow, Block };
    Action action;
    
    enum Protocol { TCP, UDP, ICMPv4, ICMPv6, Any };
    Protocol protocol;
    
    QString localPorts;        // "80,443" or "1024-65535" or "*"
    QString remotePorts;       // Same format
    QString localAddresses;    // "10.0.0.0/8" or "*" or "LocalSubnet"
    QString remoteAddresses;   // Same format
    
    QString applicationPath;   // "C:\Program Files\App\app.exe" or "*"
    QString serviceName;       // Windows service name or "*"
    
    enum Profile { Domain = 1, Private = 2, Public = 4 };
    int profiles;              // Bitmask of profiles
    
    QString grouping;          // Rule group
};

struct FirewallConflict {
    FirewallRule ruleA;
    FirewallRule ruleB;
    QString conflictDescription; // "Rule A allows port 80 but Rule B blocks all inbound"
    enum Severity { Info, Warning, Critical };
    Severity severity;
};

struct FirewallGap {
    QString description;       // "No inbound rule for port 3389 (RDP)"
    QString recommendation;    // "If RDP is needed, create allow rule for specific IPs"
    enum Severity { Info, Warning };
    Severity severity;
};
```

**Implementation**:
```cpp
class FirewallRuleAuditor : public QObject {
    Q_OBJECT
public:
    explicit FirewallRuleAuditor(QObject* parent = nullptr);
    
    void enumerateRules();
    void detectConflicts();
    void analyzeGaps();
    void fullAudit();          // Enumerate + conflicts + gaps
    
    // Search/filter
    QVector<FirewallRule> findRulesByPort(uint16_t port, 
                                          FirewallRule::Direction direction);
    QVector<FirewallRule> findRulesByApplication(const QString& appPath);
    QVector<FirewallRule> findRulesByName(const QString& nameFilter);
    
Q_SIGNALS:
    void rulesEnumerated(QVector<FirewallRule> rules);
    void conflictsDetected(QVector<FirewallConflict> conflicts);
    void gapsAnalyzed(QVector<FirewallGap> gaps);
    void auditComplete(QVector<FirewallRule> rules, 
                       QVector<FirewallConflict> conflicts,
                       QVector<FirewallGap> gaps);
    void errorOccurred(QString error);
    
private:
    QVector<FirewallRule> m_rules;
    
    QVector<FirewallRule> enumerateViaNetsh();
    QVector<FirewallRule> enumerateViaCOM();
    QVector<FirewallConflict> findConflicts(const QVector<FirewallRule>& rules);
    QVector<FirewallGap> findGaps(const QVector<FirewallRule>& rules);
};
```

**Firewall Rule Enumeration via COM**:
```cpp
QVector<FirewallRule> FirewallRuleAuditor::enumerateViaCOM() {
    // Initialize COM
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    
    // Create firewall policy instance
    INetFwPolicy2* fwPolicy = nullptr;
    CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                     __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&fwPolicy));
    
    // Get rules collection
    INetFwRules* fwRules = nullptr;
    fwPolicy->get_Rules(&fwRules);
    
    // Enumerate
    IEnumVARIANT* enumVar = nullptr;
    IUnknown* unknown = nullptr;
    fwRules->get__NewEnum(&unknown);
    unknown->QueryInterface(__uuidof(IEnumVARIANT), reinterpret_cast<void**>(&enumVar));
    
    QVector<FirewallRule> rules;
    VARIANT var;
    VariantInit(&var);
    
    while (enumVar->Next(1, &var, nullptr) == S_OK) {
        INetFwRule* fwRule = nullptr;
        var.punkVal->QueryInterface(__uuidof(INetFwRule), reinterpret_cast<void**>(&fwRule));
        
        FirewallRule rule;
        BSTR name, desc, appPath, ports;
        
        fwRule->get_Name(&name);
        rule.name = QString::fromWCharArray(name);
        SysFreeString(name);
        
        // ... extract all properties
        
        rules.append(rule);
        fwRule->Release();
        VariantClear(&var);
    }
    
    // Cleanup COM objects
    enumVar->Release();
    unknown->Release();
    fwRules->Release();
    fwPolicy->Release();
    CoUninitialize();
    
    return rules;
}
```

---

### Network Share Browser

**Purpose**: Discover and test access to SMB shares on the local network.

**Data Structures**:
```cpp
struct NetworkShareInfo {
    QString hostName;          // Server name or IP
    QString shareName;         // "Documents$", "Public"
    QString uncPath;           // "\\\\server\\share"
    
    enum ShareType { Disk, Printer, Device, IPC, Special };
    ShareType type;
    
    QString remark;            // Share description
    
    // Access testing
    bool canRead = false;
    bool canWrite = false;
    bool requiresAuth = false;
    QString accessError;
    
    QDateTime discovered;
};
```

**Implementation**:
```cpp
class NetworkShareBrowser : public QObject {
    Q_OBJECT
public:
    explicit NetworkShareBrowser(QObject* parent = nullptr);
    
    void discoverShares(const QString& hostname);
    void discoverSharesOnSubnet(const QString& subnet); // "192.168.1.0/24"
    void testAccess(const QString& uncPath);
    
    void cancel();
    
Q_SIGNALS:
    void shareDiscovered(NetworkShareInfo share);
    void discoveryComplete(QVector<NetworkShareInfo> shares);
    void accessTestComplete(QString uncPath, bool canRead, bool canWrite);
    void errorOccurred(QString error);
    
private:
    std::atomic<bool> m_cancelled{false};
    
    QVector<NetworkShareInfo> enumerateShares(const QString& hostname);
    QPair<bool, bool> testReadWriteAccess(const QString& uncPath);
};
```

---

### Network Diagnostic Controller

**Purpose**: Orchestrate all network diagnostic components.

```cpp
class NetworkDiagnosticController : public QObject {
    Q_OBJECT
public:
    enum class State {
        Idle,
        ScanningAdapters,
        RunningPing,
        RunningTraceroute,
        RunningMtr,
        RunningDnsQuery,
        ScanningPorts,
        RunningBandwidthTest,
        ScanningWiFi,
        MonitoringConnections,
        AuditingFirewall,
        BrowsingShares,
        GeneratingReport
    };
    
    explicit NetworkDiagnosticController(QObject* parent = nullptr);
    ~NetworkDiagnosticController();
    
    // Adapter inspection
    void scanAdapters();
    
    // Connectivity
    void ping(const ConnectivityTester::PingConfig& config);
    void traceroute(const ConnectivityTester::TracerouteConfig& config);
    void mtr(const ConnectivityTester::MtrConfig& config);
    
    // DNS
    void dnsQuery(const QString& hostname, const QString& recordType, 
                  const QString& dnsServer);
    void dnsCompare(const QString& hostname, const QString& recordType,
                    const QStringList& servers);
    
    // Port scanning
    void scanPorts(const PortScanner::ScanConfig& config);
    
    // Bandwidth
    void startIperfServer(uint16_t port = 5201);
    void stopIperfServer();
    void runBandwidthTest(const BandwidthTester::IperfConfig& config);
    void runHttpSpeedTest();
    
    // WiFi
    void scanWiFi();
    void startContinuousWiFiScan();
    void stopContinuousWiFiScan();
    
    // Connections
    void startConnectionMonitor();
    void stopConnectionMonitor();
    
    // Firewall
    void auditFirewall();
    
    // Shares
    void discoverShares(const QString& hostname);
    
    // Report
    void generateReport(const QString& outputPath, const QString& format);
    
    // Cancel
    void cancel();
    
    State currentState() const;
    
Q_SIGNALS:
    void stateChanged(State newState);
    void statusMessage(QString message, int timeout);
    void progressUpdated(int percent, QString status);
    
    // Component-specific signals forwarded from workers
    void adaptersScanComplete(QVector<NetworkAdapterInfo> adapters);
    void pingReply(int seq, bool success, double rttMs, int ttl, QString from);
    void pingComplete(PingResult result);
    void tracerouteHop(TracerouteHop hop);
    void tracerouteComplete(TracerouteResult result);
    void mtrUpdate(QVector<MtrHopStats> hops, int cycle);
    void dnsQueryComplete(DnsQueryResult result);
    void dnsComparisonComplete(DnsServerComparison comparison);
    void portScanned(PortScanResult result);
    void portScanComplete(QVector<PortScanResult> results);
    void bandwidthProgress(double currentMbps, double elapsedSec, double totalSec);
    void bandwidthComplete(BandwidthTestResult result);
    void iperfServerStarted(uint16_t port);
    void iperfServerStopped();
    void wifiScanComplete(QVector<WiFiNetworkInfo> networks);
    void connectionsUpdated(QVector<ConnectionInfo> connections);
    void firewallAuditComplete(QVector<FirewallRule> rules,
                                QVector<FirewallConflict> conflicts,
                                QVector<FirewallGap> gaps);
    void sharesDiscovered(QVector<NetworkShareInfo> shares);
    void reportGenerated(QString path);
    void errorOccurred(QString error);
    
private:
    State m_state = State::Idle;
    QThread* m_workerThread;
    
    std::unique_ptr<NetworkAdapterInspector> m_adapterInspector;
    std::unique_ptr<ConnectivityTester> m_connectivityTester;
    std::unique_ptr<DnsDiagnosticTool> m_dnsTool;
    std::unique_ptr<PortScanner> m_portScanner;
    std::unique_ptr<BandwidthTester> m_bandwidthTester;
    std::unique_ptr<WiFiAnalyzer> m_wifiAnalyzer;
    std::unique_ptr<ActiveConnectionsMonitor> m_connectionMonitor;
    std::unique_ptr<FirewallRuleAuditor> m_firewallAuditor;
    std::unique_ptr<NetworkShareBrowser> m_shareBrowser;
    std::unique_ptr<NetworkDiagnosticReportGenerator> m_reportGenerator;
};
```

---

## 🎨 User Interface Design

### Network Diagnostics Panel Layout (Sub-Tab Design)

The panel uses an internal tab bar for each diagnostic tool, keeping the interface clean.

```
┌─────────────────────────────────────────────────────────────────────┐
│ Network Diagnostics & Troubleshooting Panel                        │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌──────────────── 🔌 NETWORK ADAPTERS ────────────────────────┐  │
│  │                                                               │  │
│  │  ┌─────────────────────────────────────────────────────────┐ │  │
│  │  │ Adapter        | Type     | Status    | IP          | Speed│ │
│  │  │────────────────────────────────────────────────────────│ │  │
│  │  │ Ethernet       | Wired    | 🟢 Up    | 10.0.1.42   | 1 Gbps│ │
│  │  │ Wi-Fi          | Wireless | 🟢 Up    | 10.0.1.105  | 866 Mbps│ │
│  │  │ VPN Client     | VPN      | ⚫ Down  | —           | —     │ │
│  │  └─────────────────────────────────────────────────────────┘ │  │
│  │                                                               │  │
│  │  Selected: Ethernet                                          │  │
│  │  MAC: 00:1A:2B:3C:4D:5E  |  DHCP: Yes  |  Gateway: 10.0.1.1│  │
│  │  DNS: 10.0.1.1, 8.8.8.8  |  Subnet: 255.255.255.0           │  │
│  │  Driver: Intel I225-V (v12.19.1.37)                          │  │
│  │                                                               │  │
│  │  [🔄 Refresh]  [📋 Copy Config]                               │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌─ 📡 DIAGNOSTIC TOOLS ─────────────────────────────────────────┐ │
│  │ [Ping] [Traceroute] [MTR] [DNS] [Port Scan] [Bandwidth]      │ │
│  │ [WiFi] [Connections] [Firewall] [Shares]                      │ │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  (Tool-specific content appears below based on selected tab)        │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Ping Tool

```
┌─────────────────────────────────────────────────────────────────────┐
│  ┌──────────────── 📡 PING ────────────────────────────────────┐  │
│  │                                                               │  │
│  │  Target: [google.com_______________]  Count: [10]  ▼         │  │
│  │  Interval: [1000] ms   Timeout: [4000] ms                   │  │
│  │  Packet Size: [32] bytes   TTL: [128]                        │  │
│  │                                                               │  │
│  │  [▶️ Start Ping]  [⏹️ Stop]                                   │  │
│  │                                                               │  │
│  │  Results:                                                    │  │
│  │  ┌─────────────────────────────────────────────────────────┐ │  │
│  │  │ #   | Reply From      | Time    | TTL | Status          │ │  │
│  │  │──────────────────────────────────────────────────────── │ │  │
│  │  │ 1   | 142.250.80.46   | 12.4 ms | 118 | 🟢 Reply       │ │  │
│  │  │ 2   | 142.250.80.46   | 11.8 ms | 118 | 🟢 Reply       │ │  │
│  │  │ 3   | 142.250.80.46   | 13.1 ms | 118 | 🟢 Reply       │ │  │
│  │  │ 4   | —               | —       | —   | 🔴 Timeout      │ │  │
│  │  │ 5   | 142.250.80.46   | 12.0 ms | 118 | 🟢 Reply       │ │  │
│  │  └─────────────────────────────────────────────────────────┘ │  │
│  │                                                               │  │
│  │  Statistics:                                                 │  │
│  │  Sent: 5  |  Received: 4  |  Lost: 1 (20%)                 │  │
│  │  RTT: min=11.8ms  avg=12.3ms  max=13.1ms  jitter=0.5ms     │  │
│  │                                                               │  │
│  │  [RTT graph/chart showing latency over time]                 │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
```

### Traceroute Tool

```
┌─────────────────────────────────────────────────────────────────────┐
│  ┌──────────────── 🗺️ TRACEROUTE ──────────────────────────────┐  │
│  │                                                               │  │
│  │  Target: [google.com_______________]  Max Hops: [30]         │  │
│  │                                                               │  │
│  │  [▶️ Trace Route]  [⏹️ Stop]                                  │  │
│  │                                                               │  │
│  │  ┌─────────────────────────────────────────────────────────┐ │  │
│  │  │ Hop | IP Address      | Hostname           | RTT 1-3   │ │  │
│  │  │──────────────────────────────────────────────────────── │ │  │
│  │  │  1  | 10.0.1.1        | gateway.local      | 1ms 1ms 1ms│ │  │
│  │  │  2  | 203.0.113.1     | isp-gw.example.com | 5ms 4ms 5ms│ │  │
│  │  │  3  | 198.51.100.12   | core-rtr.isp.net   | 8ms 7ms 9ms│ │  │
│  │  │  4  | * * *           | (timed out)        | — — —      │ │  │
│  │  │  5  | 72.14.238.168   | google-peer.net    | 11ms 10ms 12ms│ │
│  │  │  6  | 142.250.80.46   | lax17s55-in-f14    | 12ms 12ms 11ms│ │
│  │  └─────────────────────────────────────────────────────────┘ │  │
│  │                                                               │  │
│  │  ✅ Target reached in 6 hops                                  │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
```

### DNS Diagnostic Tool

```
┌─────────────────────────────────────────────────────────────────────┐
│  ┌──────────────── 🌐 DNS DIAGNOSTICS ─────────────────────────┐  │
│  │                                                               │  │
│  │  Hostname: [example.com____________]                         │  │
│  │  Record Type: [A] ▼  (A, AAAA, MX, CNAME, TXT, SOA, NS, SRV)│  │
│  │  DNS Server:  [System Default] ▼                             │  │
│  │                                                               │  │
│  │  [🔍 Query]  [🔄 Compare All Servers]  [🗑️ Flush DNS Cache]  │  │
│  │                                                               │  │
│  │  Query Result:                                               │  │
│  │  ┌─────────────────────────────────────────────────────────┐ │  │
│  │  │ Server           | Response   | Time    | Status        │ │  │
│  │  │──────────────────────────────────────────────────────── │ │  │
│  │  │ System Default   | 93.184.216.34 | 15ms | 🟢 OK        │ │  │
│  │  │ Google (8.8.8.8) | 93.184.216.34 | 22ms | 🟢 OK        │ │  │
│  │  │ Cloudflare (1.1.1.1)| 93.184.216.34 | 8ms | 🟢 OK     │ │  │
│  │  │ Quad9 (9.9.9.9)  | 93.184.216.34 | 18ms | 🟢 OK        │ │  │
│  │  │ OpenDNS           | 93.184.216.34 | 25ms | 🟢 OK        │ │  │
│  │  └─────────────────────────────────────────────────────────┘ │  │
│  │                                                               │  │
│  │  ✅ All servers agree: 93.184.216.34                          │  │
│  │  ⚡ Fastest: Cloudflare (1.1.1.1) at 8ms                     │  │
│  │  TTL: 300 seconds                                            │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
```

### Port Scanner

```
┌─────────────────────────────────────────────────────────────────────┐
│  ┌──────────────── 🔓 PORT SCANNER ────────────────────────────┐  │
│  │                                                               │  │
│  │  Target: [192.168.1.100____________]                         │  │
│  │  Preset: [Common Services] ▼  or Custom: [1-1024___]        │  │
│  │  Timeout: [3000] ms  Concurrent: [50]  ☑ Banner Grab        │  │
│  │                                                               │  │
│  │  [🔍 Scan Ports]  [⏹️ Stop]                                   │  │
│  │                                                               │  │
│  │  Progress: ████████████████░░░░  80% (800/1000 ports)       │  │
│  │                                                               │  │
│  │  ┌─────────────────────────────────────────────────────────┐ │  │
│  │  │ Port  | State    | Service | Response | Banner          │ │  │
│  │  │──────────────────────────────────────────────────────── │ │  │
│  │  │ 22    | 🟢 Open  | SSH     | 2.1 ms   | OpenSSH_8.9    │ │  │
│  │  │ 80    | 🟢 Open  | HTTP    | 1.5 ms   | nginx/1.24     │ │  │
│  │  │ 443   | 🟢 Open  | HTTPS   | 1.8 ms   | —              │ │  │
│  │  │ 445   | 🔴 Closed| SMB     | 0.5 ms   | —              │ │  │
│  │  │ 3389  | 🟡 Filtered| RDP   | timeout  | —              │ │  │
│  │  └─────────────────────────────────────────────────────────┘ │  │
│  │                                                               │  │
│  │  Summary: 3 open, 997 closed, 0 filtered                    │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
```

### Bandwidth Test (iPerf3)

```
┌─────────────────────────────────────────────────────────────────────┐
│  ┌──────────────── ⚡ BANDWIDTH TEST ──────────────────────────┐  │
│  │                                                               │  │
│  │  ┌─ LAN Bandwidth (iPerf3) ─────────────────────────────┐   │  │
│  │  │                                                       │   │  │
│  │  │  Mode: [● Client  ○ Server]                          │   │  │
│  │  │                                                       │   │  │
│  │  │  Server IP: [192.168.1.50________]  Port: [5201]     │   │  │
│  │  │  Duration: [10] sec  Streams: [1]  ☑ Bidirectional   │   │  │
│  │  │                                                       │   │  │
│  │  │  [▶️ Run Test]  [⏹️ Stop]                              │   │  │
│  │  │                                                       │   │  │
│  │  │  Results:                                             │   │  │
│  │  │  Download: 941.2 Mbps  ████████████████████ 94%       │   │  │
│  │  │  Upload:   938.7 Mbps  ████████████████████ 94%       │   │  │
│  │  │  Retransmits: 3  |  Jitter: 0.12ms                   │   │  │
│  │  │                                                       │   │  │
│  │  │  [Throughput graph over test duration]                 │   │  │
│  │  │                                                       │   │  │
│  │  └───────────────────────────────────────────────────────┘   │  │
│  │                                                               │  │
│  │  ┌─ Internet Speed ──────────────────────────────────────┐   │  │
│  │  │                                                       │   │  │
│  │  │  [▶️ Run Internet Speed Test]                          │   │  │
│  │  │                                                       │   │  │
│  │  │  Download: 245.3 Mbps  |  Upload: 35.1 Mbps          │   │  │
│  │  │  Latency: 12ms                                        │   │  │
│  │  │                                                       │   │  │
│  │  └───────────────────────────────────────────────────────┘   │  │
│  │                                                               │  │
│  │  Server Mode:                                                │  │
│  │  Status: ⚫ Not Running                                      │  │
│  │  [▶️ Start iPerf3 Server]  (for other SAK instances to test)  │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
```

### WiFi Analyzer

```
┌─────────────────────────────────────────────────────────────────────┐
│  ┌──────────────── 📶 WIFI ANALYZER ───────────────────────────┐  │
│  │                                                               │  │
│  │  [🔄 Scan Now]  [▶️ Continuous Scan (5s)]  [⏹️ Stop]          │  │
│  │                                                               │  │
│  │  Discovered Networks: 12                                     │  │
│  │  ┌─────────────────────────────────────────────────────────┐ │  │
│  │  │ SSID           | Signal | Ch | Band   | Security | Vendor│ │  │
│  │  │──────────────────────────────────────────────────────── │ │  │
│  │  │ CorpWiFi ★     | -42dBm | 36 | 5 GHz  | WPA3     | Ubiq.│ │  │
│  │  │ CorpWiFi-Guest | -48dBm | 6  | 2.4GHz | WPA2     | Ubiq.│ │  │
│  │  │ Neighbor-5G    | -65dBm | 40 | 5 GHz  | WPA2     | TP-L.│ │  │
│  │  │ HomeNet        | -72dBm | 1  | 2.4GHz | WPA2     | Netg.│ │  │
│  │  │ (hidden)       | -78dBm | 11 | 2.4GHz | WPA2     | Cisco│ │  │
│  │  └─────────────────────────────────────────────────────────┘ │  │
│  │                                                               │  │
│  │  ★ = Currently connected                                    │  │
│  │                                                               │  │
│  │  Channel Utilization (2.4 GHz):                              │  │
│  │  Ch 1:  ██░░░░░░░░  1 network  (-72 dBm avg)               │  │
│  │  Ch 6:  ████░░░░░░  2 networks (-55 dBm avg) ⚠️ Busy        │  │
│  │  Ch 11: ██░░░░░░░░  1 network  (-78 dBm avg)               │  │
│  │                                                               │  │
│  │  Channel Utilization (5 GHz):                                │  │
│  │  Ch 36: ██░░░░░░░░  1 network  (-42 dBm avg) ✅ Clean       │  │
│  │  Ch 40: ██░░░░░░░░  1 network  (-65 dBm avg) ✅ Clean       │  │
│  │                                                               │  │
│  │  ⚠️ Security Alert: 0 networks using WEP (insecure)         │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
```

### Active Connections Monitor

```
┌─────────────────────────────────────────────────────────────────────┐
│  ┌──────────────── 🔗 ACTIVE CONNECTIONS ──────────────────────┐  │
│  │                                                               │  │
│  │  Filter: ☑ TCP ☑ UDP  Process: [__________]  Port: [____]   │  │
│  │  Refresh: [2s] ▼  ☑ Auto-refresh                            │  │
│  │                                                               │  │
│  │  Connections: 47 (35 ESTABLISHED, 8 LISTEN, 4 TIME_WAIT)    │  │
│  │  ┌─────────────────────────────────────────────────────────┐ │  │
│  │  │ Process     | Proto | Local          | Remote         | State│ │
│  │  │──────────────────────────────────────────────────────── │ │  │
│  │  │ chrome.exe  | TCP   | 10.0.1.42:52341| 142.250.80.46:443| ESTAB│ │
│  │  │ chrome.exe  | TCP   | 10.0.1.42:52342| 185.70.41.35:443| ESTAB│ │
│  │  │ svchost.exe | TCP   | 0.0.0.0:135    | 0.0.0.0:0      | LISTEN│ │
│  │  │ Teams.exe   | UDP   | 10.0.1.42:50000| —              | —     │ │
│  │  │ sak_utility | TCP   | 10.0.1.42:5201 | 0.0.0.0:0      | LISTEN│ │
│  │  └─────────────────────────────────────────────────────────┘ │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
```

### Firewall Rule Auditor

```
┌─────────────────────────────────────────────────────────────────────┐
│  ┌──────────────── 🛡️ FIREWALL AUDITOR ────────────────────────┐  │
│  │                                                               │  │
│  │  [🔍 Audit Firewall]                                         │  │
│  │                                                               │  │
│  │  Search: [_______________]  Filter: [Inbound ▼] [All ▼]     │  │
│  │                                                               │  │
│  │  Rules: 312 (198 Allow, 114 Block)  |  Conflicts: 2  |  Gaps: 3│  │
│  │  ┌─────────────────────────────────────────────────────────┐ │  │
│  │  │ Name                | Dir  | Action | Proto | Ports      │ │  │
│  │  │──────────────────────────────────────────────────────── │ │  │
│  │  │ Allow HTTP          | In   | Allow  | TCP   | 80         │ │  │
│  │  │ Allow HTTPS         | In   | Allow  | TCP   | 443        │ │  │
│  │  │ Block All Inbound   | In   | Block  | Any   | *          │ │  │
│  │  │ Allow RDP           | In   | Allow  | TCP   | 3389       │ │  │
│  │  └─────────────────────────────────────────────────────────┘ │  │
│  │                                                               │  │
│  │  ⚠️ Conflicts Detected:                                      │  │
│  │  • "Allow HTTP" (port 80) conflicts with "Block All Inbound" │  │
│  │  • "Allow RDP" is enabled on Public profile (security risk)  │  │
│  │                                                               │  │
│  │  📋 Gaps Detected:                                            │  │
│  │  • No explicit rule for DNS (port 53 outbound)               │  │
│  │  • ICMP (ping) blocked on all profiles                       │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
```

### Report Generation

```
┌─────────────────────────────────────────────────────────────────────┐
│  ┌──────────────── 📄 NETWORK DIAGNOSTIC REPORT ───────────────┐  │
│  │                                                               │  │
│  │  Technician: [________________]  Ticket #: [___________]    │  │
│  │  Notes:      [________________________________]             │  │
│  │                                                               │  │
│  │  Include in Report:                                          │  │
│  │  ☑ Adapter Configuration    ☑ Ping Results                  │  │
│  │  ☑ Traceroute Results       ☑ DNS Query Results             │  │
│  │  ☑ Port Scan Results        ☑ Bandwidth Test Results        │  │
│  │  ☑ WiFi Analysis            ☑ Firewall Audit                │  │
│  │  ☐ Active Connections       ☐ Network Shares                │  │
│  │                                                               │  │
│  │  [📄 Generate HTML Report]  [📊 Export JSON]                  │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
```

---

## 📂 File Structure

### New Files to Create

#### Headers (`include/sak/`)

```
network_diagnostic_panel.h              # Main UI panel (sub-tabbed)
network_diagnostic_controller.h         # Orchestrates all diagnostic workers
network_adapter_inspector.h             # Adapter enumeration via IP Helper API
connectivity_tester.h                   # Ping, Traceroute, MTR
dns_diagnostic_tool.h                   # DNS queries, multi-server comparison
port_scanner.h                          # TCP connect scanning
bandwidth_tester.h                      # iPerf3 integration + HTTP speed test
wifi_analyzer.h                         # WiFi scanning via Native WiFi API
active_connections_monitor.h            # TCP/UDP connection monitoring
firewall_rule_auditor.h                 # Windows Firewall rule enumeration
network_share_browser.h                 # SMB share discovery
network_diagnostic_report_generator.h   # HTML/JSON report generation
network_diagnostic_types.h             # Shared types (all structs above)
```

#### Implementation (`src/`)

```
gui/network_diagnostic_panel.cpp
core/network_diagnostic_controller.cpp
core/network_adapter_inspector.cpp
core/connectivity_tester.cpp
core/dns_diagnostic_tool.cpp
core/port_scanner.cpp
core/bandwidth_tester.cpp
core/wifi_analyzer.cpp
core/active_connections_monitor.cpp
core/firewall_rule_auditor.cpp
core/network_share_browser.cpp
core/network_diagnostic_report_generator.cpp
```

#### Bundle Script

```
scripts/bundle_iperf3.ps1              # Download + verify iperf3.exe
```

#### Resources

```
tools/iperf3/
├─ iperf3.exe                          # iPerf3 client/server (~2 MB)
├─ cygwin1.dll                         # Cygwin runtime (required, ~3.4 MB)
├─ LICENSE                             # BSD 3-Clause
└─ README.txt                          # Version and attribution

resources/network/
├─ oui_database.txt                    # MAC address vendor lookup (IEEE OUI)
├─ report_template.html               # HTML report template
└─ icons/
   ├─ network_diagnostic.svg
   ├─ ping.svg
   ├─ traceroute.svg
   ├─ dns.svg
   ├─ port_scan.svg
   ├─ bandwidth.svg
   ├─ wifi.svg
   └─ firewall.svg
```

---

## 🔧 Third-Party Dependencies

### iPerf3 (iperf3.exe)

| Property | Value |
|----------|-------|
| **Tool** | iPerf3 |
| **Version** | 3.17.1 (latest stable) |
| **License** | BSD 3-Clause |
| **Source** | https://github.com/esnet/iperf |
| **Download** | https://github.com/ar51an/iperf3-win-builds/releases (Windows builds) |
| **Size** | ~2 MB (iperf3.exe) + ~3.4 MB (cygwin1.dll) |
| **Redistributable** | ✅ Yes (BSD 3-Clause — very permissive) |
| **Purpose** | LAN/WAN bandwidth measurement (TCP/UDP throughput, jitter, packet loss) |
| **Why this tool** | Industry standard, JSON output, client/server bidirectional, portable |
| **Alternative considered** | Custom TCP speed test — rejected due to complexity and lack of standards compliance |

**Bundle Script** (`scripts/bundle_iperf3.ps1`):
```powershell
<#
.SYNOPSIS
    Downloads and bundles iPerf3 for S.A.K. Utility.
.DESCRIPTION
    Downloads the official iPerf3 Windows build from GitHub releases,
    verifies SHA-256 hash, and extracts to tools/iperf3/.
#>

param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$ToolName = "iperf3"
$Version = "3.17.1"
$DestDir = Join-Path $PSScriptRoot "..\tools\iperf3"
$TempDir = Join-Path $env:TEMP "sak_bundle_iperf3"

# Download URL (Windows 64-bit build)
$DownloadUrl = "https://github.com/ar51an/iperf3-win-builds/releases/download/3.17.1/iperf3.17.1-win64.zip"
$ExpectedHash = "<SHA256_HASH_OF_ZIP>"  # Verify from release page

# Check if already present
if ((Test-Path "$DestDir\iperf3.exe") -and -not $Force) {
    $existingVersion = & "$DestDir\iperf3.exe" --version 2>&1 | Select-Object -First 1
    Write-Host "iPerf3 already bundled: $existingVersion"
    Write-Host "Use -Force to re-download."
    return
}

# Create directories
New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
New-Item -ItemType Directory -Force -Path $TempDir | Out-Null

try {
    # Download zip
    $zipPath = Join-Path $TempDir "iperf3.zip"
    Write-Host "Downloading iPerf3 v$Version..."
    Invoke-WebRequest -Uri $DownloadUrl -OutFile $zipPath -UseBasicParsing

    # Verify hash
    $actualHash = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash
    if ($actualHash -ne $ExpectedHash) {
        throw "SHA-256 mismatch! Expected: $ExpectedHash, Got: $actualHash"
    }
    Write-Host "SHA-256 verified: $actualHash"

    # Extract
    $extractDir = Join-Path $TempDir "extracted"
    Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force

    # Copy required files
    $files = @("iperf3.exe", "cygwin1.dll")
    foreach ($file in $files) {
        $src = Get-ChildItem -Path $extractDir -Recurse -Filter $file | Select-Object -First 1
        if ($src) {
            Copy-Item $src.FullName "$DestDir\$file" -Force
            Write-Host "Copied: $file"
        } else {
            throw "$file not found after extraction"
        }
    }

    # Copy license
    $licenseSrc = Get-ChildItem -Path $extractDir -Recurse -Filter "LICENSE" | Select-Object -First 1
    if ($licenseSrc) {
        Copy-Item $licenseSrc.FullName "$DestDir\LICENSE" -Force
    }

    # Create README
    @"
iPerf3 v$Version
=================
Bundled for S.A.K. Utility network diagnostics panel.
License: BSD 3-Clause (see LICENSE)
Source: https://github.com/esnet/iperf
Windows Build: https://github.com/ar51an/iperf3-win-builds
"@ | Set-Content "$DestDir\README.txt"

    # Verify
    $version = & "$DestDir\iperf3.exe" --version 2>&1 | Select-Object -First 1
    Write-Host "Successfully bundled: $version"

} finally {
    # Cleanup
    if (Test-Path $TempDir) {
        Remove-Item -Recurse -Force $TempDir -ErrorAction SilentlyContinue
    }
}
```

### OUI Database (MAC Vendor Lookup)

| Property | Value |
|----------|-------|
| **Data** | IEEE OUI Database |
| **License** | Public Domain (IEEE publishes openly) |
| **Source** | https://standards-oui.ieee.org/oui/oui.txt |
| **Size** | ~4 MB (text file) |
| **Purpose** | Map first 3 bytes of MAC address to manufacturer name |
| **Update** | Can be refreshed periodically |

### Native Dependencies (No Third-Party)

All other network diagnostic capabilities use Windows built-in APIs:

| Capability | API / Library / Method |
|------------|------------------------|
| Adapter enumeration | `GetAdaptersAddresses` (iphlpapi.dll) |
| Ping (ICMP) | `IcmpSendEcho2` (icmp.dll) |
| Traceroute | ICMP with incremental TTL (icmp.dll) |
| DNS queries | `DnsQuery_W` (dnsapi.dll) + Qt `QDnsLookup` |
| DNS cache | `ipconfig /displaydns` parsing |
| TCP connect scan | Qt `QTcpSocket` (already linked via Qt6::Network) |
| WiFi scanning | `WlanScan` + `WlanGetNetworkBssList` (wlanapi.dll) |
| TCP connections | `GetExtendedTcpTable` (iphlpapi.dll) |
| UDP listeners | `GetExtendedUdpTable` (iphlpapi.dll) |
| Process name | `OpenProcess` + `QueryFullProcessImageName` (kernel32.dll) |
| Firewall rules | `INetFwPolicy2` COM interface (netfw) |
| Network shares | `NetShareEnum` (netapi32.dll) |
| ARP cache | `GetIpNetTable` (iphlpapi.dll) |
| Routing table | `GetIpForwardTable` (iphlpapi.dll) |

---

## 🔧 Implementation Phases

### Phase 1: Network Adapter Inspector (Week 1-2)

**Goals**:
- Complete adapter enumeration with IP config, MAC, link speed, driver info
- Visual adapter list with status indicators

**Tasks**:
1. Implement `NetworkAdapterInspector` using `GetAdaptersAddresses`
2. Parse IPv4/IPv6 addresses, subnets, gateways, DNS servers
3. Parse DHCP info (server, lease times)
4. Parse link speed and media state
5. Driver info via WMI `Win32_NetworkAdapterConfiguration`
6. Statistics (bytes/packets sent/received, errors)
7. "Copy Config" button (formatted text for pasting into tickets)
8. Write unit tests with mock adapter data

**Acceptance Criteria**:
- ✅ All adapter types enumerated (Ethernet, WiFi, VPN, Loopback)
- ✅ Complete IP configuration displayed
- ✅ Link speed and media state accurate
- ✅ Handles multiple adapters, multiple IPs per adapter, IPv6

---

### Phase 2: Connectivity Testing - Ping & Traceroute (Week 3-5)

**Goals**:
- Visual ping with real-time RTT graph
- Traceroute with per-hop timing
- MTR mode for continuous monitoring

**Tasks**:
1. Implement `ConnectivityTester` with ICMP API (`IcmpSendEcho2`)
2. Ping: configurable count, interval, timeout, packet size, TTL
3. Real-time per-reply updates with RTT graph
4. Statistics: min/max/avg RTT, jitter, packet loss
5. Traceroute: incremental TTL, 3 probes per hop, reverse DNS
6. MTR mode: continuous traceroute with running statistics
7. Cancellation support
8. Write unit tests

**Acceptance Criteria**:
- ✅ Ping works for hostnames and IP addresses
- ✅ Real-time RTT graph updates per reply
- ✅ Traceroute reaches target with correct hop count
- ✅ MTR mode runs continuously with updating statistics
- ✅ Cancellation responsive

---

### Phase 3: DNS Diagnostics (Week 6-7)

**Goals**:
- DNS queries with record type selection
- Multi-server comparison
- DNS cache inspection and flush

**Tasks**:
1. Implement `DnsDiagnosticTool` with `DnsQuery_W` API
2. Support all common record types (A, AAAA, MX, CNAME, TXT, SOA, NS, SRV, PTR)
3. Query against specific DNS server (custom server override)
4. Multi-server comparison (query same name against 5+ servers)
5. Response time measurement per server
6. DNS cache display (`ipconfig /displaydns` parsing)
7. DNS cache flush (`ipconfig /flushdns`)
8. Reverse lookup
9. Write unit tests

**Acceptance Criteria**:
- ✅ All record types query correctly
- ✅ Custom DNS server override works
- ✅ Multi-server comparison shows agreement/disagreement
- ✅ Response times measured accurately
- ✅ Cache display and flush work

---

### Phase 4: Port Scanner (Week 8-9)

**Goals**:
- TCP connect scanning with presets and custom ranges
- Service fingerprinting via banner grabbing
- Concurrent scanning for speed

**Tasks**:
1. Implement `PortScanner` with Qt `QTcpSocket`
2. TCP connect scan with configurable timeout
3. Concurrent connections (throttled to max 50-100)
4. Service name database (well-known ports)
5. Banner grabbing (read initial response bytes)
6. Port presets (Web, Email, Database, Remote Access, etc.)
7. Custom port range support (1-65535)
8. Progress reporting and cancellation
9. Write unit tests

**Acceptance Criteria**:
- ✅ Accurately identifies Open/Closed/Filtered states
- ✅ Concurrent scanning completes 1000 ports in < 30 seconds
- ✅ Banner grabbing retrieves service versions
- ✅ Presets cover common use cases
- ✅ No false positives/negatives

---

### Phase 5: Bandwidth Testing (iPerf3) (Week 10-12)

**Goals**:
- Bundle iPerf3 with SHA-256 verification
- Client/server modes for LAN testing
- HTTP-based internet speed test

**Tasks**:
1. Create `scripts/bundle_iperf3.ps1`
2. Download, verify, and extract iPerf3
3. Implement `BandwidthTester` class
4. iPerf3 server mode (other SAK instances test against this)
5. iPerf3 client mode (test against remote server)
6. Parse iPerf3 JSON output for throughput, jitter, retransmits
7. Real-time throughput graph (per-second updates)
8. Bidirectional testing support
9. HTTP-based internet speed test (download large file, measure throughput)
10. Add to `THIRD_PARTY_LICENSES.md`
11. Write unit tests

**Acceptance Criteria**:
- ✅ iPerf3 bundled and verified
- ✅ Server mode starts/stops cleanly
- ✅ Client mode measures throughput accurately
- ✅ JSON output parsed correctly
- ✅ Real-time graph updates smoothly
- ✅ HTTP speed test provides reasonable internet speed estimate

---

### Phase 6: WiFi Analyzer (Week 13-14)

**Goals**:
- WiFi network discovery with signal strength
- Channel utilization analysis
- Security assessment

**Tasks**:
1. Implement `WiFiAnalyzer` with Windows Native WiFi API (`wlanapi.dll`)
2. SSID scanning with signal strength (dBm + quality %)
3. Channel and frequency detection (2.4/5/6 GHz)
4. Security type identification (WPA2/WPA3/WEP/Open)
5. Channel utilization calculation and visualization
6. Hidden network detection
7. Continuous scan mode with configurable interval
8. MAC vendor lookup via OUI database
9. Current connection info display
10. Write unit tests

**Acceptance Criteria**:
- ✅ All visible WiFi networks discovered
- ✅ Signal strength accurate (matches other WiFi tools)
- ✅ Channel utilization correctly calculated
- ✅ Security types correctly identified
- ✅ Vendor lookup works for common manufacturers
- ✅ Handles systems with no WiFi adapter gracefully

---

### Phase 7: Active Connections & Firewall (Week 15-17)

**Goals**:
- Real-time TCP/UDP connection monitoring
- Windows Firewall rule auditing with conflict detection

**Tasks**:
1. Implement `ActiveConnectionsMonitor` with `GetExtendedTcpTable`/`GetExtendedUdpTable`
2. Process name resolution for each connection
3. Real-time refresh with configurable interval
4. Filter by protocol, process, port
5. New connection / closed connection detection (diff between refreshes)
6. Implement `FirewallRuleAuditor` with COM `INetFwPolicy2`
7. Enumerate all inbound/outbound rules
8. Conflict detection (overlapping allow/block rules)
9. Gap analysis (common ports without explicit rules)
10. Search/filter rules by port, app, name
11. Write unit tests

**Acceptance Criteria**:
- ✅ All TCP/UDP connections displayed with correct process names
- ✅ Real-time updates work smoothly
- ✅ Firewall rules fully enumerated (matches `netsh advfirewall` output)
- ✅ Conflicts correctly identified
- ✅ Gap analysis provides useful recommendations

---

### Phase 8: Network Shares & Report Generation (Week 18-19)

**Goals**:
- SMB share discovery and access testing
- Comprehensive diagnostic report generation

**Tasks**:
1. Implement `NetworkShareBrowser` with `NetShareEnum`/`WNetEnumResource`
2. Share discovery on specific host
3. Access testing (read/write permissions)
4. Implement `NetworkDiagnosticReportGenerator`
5. HTML report with all diagnostic results, styled tables, status indicators
6. JSON report with structured data
7. Selectable report sections (choose which tests to include)
8. Technician name and ticket number fields
9. Write unit tests

**Acceptance Criteria**:
- ✅ Shares discovered on target host
- ✅ Access permissions correctly tested
- ✅ HTML report is print-ready and professional
- ✅ JSON report is valid and complete
- ✅ Section selection works

---

### Phase 9: UI Implementation (Week 20-23)

**Goals**:
- Complete Network Diagnostics Panel with sub-tabbed interface
- All tools integrated with visual feedback

**Tasks**:
1. Implement `NetworkDiagnosticPanel` GUI with internal sub-tab bar
2. Adapter inspector section (always visible at top)
3. Sub-tab: Ping (with RTT graph)
4. Sub-tab: Traceroute (hop table)
5. Sub-tab: MTR (continuous traceroute statistics)
6. Sub-tab: DNS (query form, multi-server comparison table)
7. Sub-tab: Port Scanner (presets, results table)
8. Sub-tab: Bandwidth (iPerf3 client/server, throughput graph)
9. Sub-tab: WiFi (network list, channel utilization chart)
10. Sub-tab: Connections (live connection table)
11. Sub-tab: Firewall (rule list, conflicts, gaps)
12. Sub-tab: Shares (share browser)
13. Report generation section (bottom)
14. Connect all signals/slots to `NetworkDiagnosticController`
15. Settings persistence via `ConfigManager`
16. Add panel to `MainWindow` tab widget

**Acceptance Criteria**:
- ✅ All sub-tabs functional and responsive
- ✅ Real-time updates for ping, connections, WiFi
- ✅ Graphs render correctly
- ✅ Consistent with existing SAK UI style
- ✅ Tab switching is instant

---

### Phase 10: Testing & Polish (Week 24-26)

**Goals**:
- Test on diverse network configurations
- Edge case handling
- Performance optimization

**Tasks**:
1. Test on wired, WiFi, VPN, cellular connections
2. Test with multiple adapters active
3. Test port scanner against various targets
4. Test iPerf3 between two SAK instances on LAN
5. Test WiFi analyzer in dense WiFi environments
6. Test firewall auditor with complex rule sets
7. Test on networks with restrictive firewalls
8. Performance profiling (connection monitor overhead)
9. Error handling for all edge cases
10. Update README.md and THIRD_PARTY_LICENSES.md
11. Write user documentation

**Acceptance Criteria**:
- ✅ Works on diverse network configurations
- ✅ Graceful handling of missing adapters, no WiFi, restricted networks
- ✅ Port scanner doesn't crash or hang on unresponsive targets
- ✅ Connection monitor has minimal CPU overhead (< 1%)
- ✅ Documentation complete

---

**Total Timeline**: 26 weeks (6.5 months)

---

## 📋 CMakeLists.txt Changes

### New Source Files
```cmake
# Add to CORE_SOURCES:
src/core/network_diagnostic_controller.cpp
src/core/network_adapter_inspector.cpp
src/core/connectivity_tester.cpp
src/core/dns_diagnostic_tool.cpp
src/core/port_scanner.cpp
src/core/bandwidth_tester.cpp
src/core/wifi_analyzer.cpp
src/core/active_connections_monitor.cpp
src/core/firewall_rule_auditor.cpp
src/core/network_share_browser.cpp
src/core/network_diagnostic_report_generator.cpp
include/sak/network_diagnostic_controller.h
include/sak/network_adapter_inspector.h
include/sak/connectivity_tester.h
include/sak/dns_diagnostic_tool.h
include/sak/port_scanner.h
include/sak/bandwidth_tester.h
include/sak/wifi_analyzer.h
include/sak/active_connections_monitor.h
include/sak/firewall_rule_auditor.h
include/sak/network_share_browser.h
include/sak/network_diagnostic_report_generator.h
include/sak/network_diagnostic_types.h

# Add to GUI_SOURCES:
src/gui/network_diagnostic_panel.cpp
include/sak/network_diagnostic_panel.h

# Add to PLATFORM_LIBS (Windows):
# iphlpapi.lib  - IP Helper API (adapters, TCP table, ARP, routing)
# icmp.lib      - ICMP API (ping, traceroute)
# dnsapi.lib    - DNS query API
# wlanapi.lib   - Native WiFi API
# netapi32.lib  - Network share enumeration
# ole32.lib     - COM (for INetFwPolicy2 firewall)
# oleaut32.lib  - COM automation (for firewall rule enumeration)
```

### Bundle iPerf3 During Build
```cmake
# Copy iPerf3 to output
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/iperf3")
    add_custom_command(TARGET sak_utility POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/iperf3
            $<TARGET_FILE_DIR:sak_utility>/tools/iperf3
        COMMENT "Copying bundled iPerf3 to output directory"
    )
endif()

# Copy OUI database to output
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/network/oui_database.txt")
    add_custom_command(TARGET sak_utility POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/network/oui_database.txt"
            "$<TARGET_FILE_DIR:sak_utility>/resources/network/oui_database.txt"
        COMMENT "Copying OUI vendor database to output directory"
    )
endif()
```

---

## 📋 Configuration & Settings

### ConfigManager Extensions

```cpp
// Network Diagnostics Settings

// Ping defaults
int getNetDiagPingCount() const;
void setNetDiagPingCount(int count);
int getNetDiagPingTimeout() const;
void setNetDiagPingTimeout(int ms);
int getNetDiagPingInterval() const;
void setNetDiagPingInterval(int ms);
int getNetDiagPingPacketSize() const;
void setNetDiagPingPacketSize(int bytes);

// Traceroute defaults
int getNetDiagTracerouteMaxHops() const;
void setNetDiagTracerouteMaxHops(int hops);

// Port scanner defaults
int getNetDiagPortScanTimeout() const;
void setNetDiagPortScanTimeout(int ms);
int getNetDiagPortScanConcurrent() const;
void setNetDiagPortScanConcurrent(int count);
bool getNetDiagPortScanBannerGrab() const;
void setNetDiagPortScanBannerGrab(bool enabled);

// Bandwidth test
uint16_t getNetDiagIperfPort() const;
void setNetDiagIperfPort(uint16_t port);
int getNetDiagIperfDuration() const;
void setNetDiagIperfDuration(int seconds);

// Connection monitor
int getNetDiagConnectionRefreshMs() const;
void setNetDiagConnectionRefreshMs(int ms);
bool getNetDiagConnectionResolveHostnames() const;
void setNetDiagConnectionResolveHostnames(bool enabled);

// WiFi analyzer
int getNetDiagWifiScanIntervalMs() const;
void setNetDiagWifiScanIntervalMs(int ms);

// Report
QString getNetDiagReportOutputDir() const;
void setNetDiagReportOutputDir(const QString& path);
QString getNetDiagTechnicianName() const;
void setNetDiagTechnicianName(const QString& name);

// Favorite DNS servers (user-customizable)
QStringList getNetDiagFavoriteDnsServers() const;
void setNetDiagFavoriteDnsServers(const QStringList& servers);

// Favorite ping/scan targets
QStringList getNetDiagFavoriteTargets() const;
void setNetDiagFavoriteTargets(const QStringList& targets);
```

**Default Values**:
```cpp
netdiag/ping_count = 10
netdiag/ping_timeout = 4000              // ms
netdiag/ping_interval = 1000             // ms
netdiag/ping_packet_size = 32            // bytes
netdiag/traceroute_max_hops = 30
netdiag/port_scan_timeout = 3000         // ms
netdiag/port_scan_concurrent = 50
netdiag/port_scan_banner_grab = true
netdiag/iperf_port = 5201
netdiag/iperf_duration = 10              // seconds
netdiag/connection_refresh_ms = 2000
netdiag/connection_resolve_hostnames = false   // slow, off by default
netdiag/wifi_scan_interval_ms = 5000
netdiag/report_output_dir = "%USERPROFILE%\\Documents\\SAK_Reports"
netdiag/technician_name = ""
netdiag/favorite_dns_servers = ["8.8.8.8", "1.1.1.1", "9.9.9.9"]
netdiag/favorite_targets = ["google.com", "1.1.1.1", "8.8.8.8"]
```

---

## 🧪 Testing Strategy

### Unit Tests

**test_network_adapter_inspector.cpp**:
- Adapter parsing from mock `GetAdaptersAddresses` output
- IPv4/IPv6 address extraction
- DHCP info parsing
- Link speed formatting
- Multiple adapter handling

**test_connectivity_tester.cpp**:
- Ping reply parsing
- Statistics calculation (min/max/avg/jitter)
- Traceroute hop parsing
- MTR statistics accumulation
- Timeout handling

**test_dns_diagnostic_tool.cpp**:
- DNS record parsing (A, AAAA, MX, CNAME, TXT, SOA, NS, SRV, PTR)
- Multi-server comparison logic
- Response time measurement
- Error handling (NXDOMAIN, SERVFAIL, timeout)

**test_port_scanner.cpp**:
- Port state detection (Open/Closed/Filtered)
- Concurrent scanning behavior
- Service name database lookup
- Banner parsing
- Timeout handling

**test_bandwidth_tester.cpp**:
- iPerf3 JSON output parsing
- Throughput calculation
- Interval data extraction
- Error handling (server unreachable)

**test_wifi_analyzer.cpp**:
- WiFi network info parsing
- Channel utilization calculation
- Frequency to channel conversion
- Signal quality to dBm conversion
- OUI vendor lookup

**test_active_connections_monitor.cpp**:
- TCP connection enumeration (mock `GetExtendedTcpTable`)
- UDP listener enumeration
- Process name resolution
- State string mapping
- Connection diff detection (new/closed)

**test_firewall_rule_auditor.cpp**:
- Rule parsing
- Conflict detection logic
- Gap analysis logic
- Search/filter functionality

**test_network_share_browser.cpp**:
- Share enumeration parsing
- Access testing logic

**test_network_diagnostic_report.cpp**:
- HTML report generation (valid HTML)
- JSON report generation (valid JSON)
- Section selection

### Integration Tests

**test_network_diagnostic_integration.cpp**:
- Live adapter scan (verify local machine adapters)
- Live ping to localhost (127.0.0.1)
- Live DNS query for known domains
- iPerf3 server start/stop lifecycle
- Connection monitor start/stop lifecycle
- Live firewall rule enumeration
- Full report generation with live data

### Manual Testing

1. **Wired network** (Ethernet, static IP)
2. **WiFi network** (DHCP, WPA2/WPA3)
3. **VPN active** (split tunnel, full tunnel)
4. **Multiple adapters** (wired + WiFi + VPN simultaneously)
5. **No network** (adapter disabled)
6. **Restricted network** (corporate firewall blocking ICMP, port scanning)
7. **Dense WiFi environment** (10+ visible networks)
8. **Two SAK instances** (iPerf3 server ↔ client test)

---

## 🚧 Limitations & Challenges

### Technical Limitations

**Raw Socket Restrictions**:
- ❌ Windows does not allow raw sockets for non-admin processes
- ✅ SAK Utility runs as admin (manifest), so ICMP API works
- ⚠️ Raw SYN scanning not possible without Npcap — using TCP connect scan instead (reliable but detectable)
- **Mitigation**: TCP connect scan is sufficient for diagnostics; recommend Nmap for stealth scanning

**WiFi Scanning**:
- ⚠️ `WlanScan` may take 1-4 seconds per scan
- ⚠️ Some drivers don't report all BSSIDs (especially in passive scan mode)
- ⚠️ 6 GHz (WiFi 6E) scanning requires Windows 11 and compatible drivers
- **Mitigation**: Use `WlanGetNetworkBssList` for complete BSS info; note 6 GHz limitations in UI

**Firewall COM Interface**:
- ⚠️ `INetFwPolicy2` COM interface requires COM initialization per-thread
- ⚠️ Some third-party firewalls (e.g., Windows Defender Firewall disabled, third-party active) won't show rules
- **Mitigation**: Detect active firewall product, warn if not Windows Firewall

**iPerf3 Firewall Rules**:
- ⚠️ iPerf3 server needs inbound port open (default 5201)
- ⚠️ Windows Firewall will prompt to allow iperf3.exe
- **Mitigation**: Auto-create temporary firewall rule when starting server, remove on stop

**Port Scanning Ethics**:
- ⚠️ Port scanning remote hosts may violate acceptable use policies
- **Mitigation**: Add warning dialog before scanning non-local targets; default to local subnet scanning

**DNS Server Override**:
- ⚠️ `DnsQuery_W` with custom server requires `DNS_QUERY_BYPASS_CACHE` flag
- ⚠️ Some corporate DNS proxies intercept all DNS traffic regardless of target server
- **Mitigation**: Note in results if responses differ unexpectedly; suggest VPN testing

### Workarounds

**No WiFi Adapter**:
```cpp
// Detect WiFi adapter availability
DWORD maxClient = 2;
DWORD curVersion = 0;
HANDLE wlanHandle = nullptr;
if (WlanOpenHandle(maxClient, nullptr, &curVersion, &wlanHandle) != ERROR_SUCCESS) {
    // Show: "WiFi adapter not found. WiFi analysis is unavailable."
    // Disable WiFi sub-tab
}
```

**iPerf3 Server Firewall Auto-Rule**:
```cpp
void BandwidthTester::createFirewallRule(uint16_t port) {
    // Use netsh to add temporary inbound rule
    QProcess::execute("netsh", {
        "advfirewall", "firewall", "add", "rule",
        "name=SAK_iPerf3_Temp",
        "dir=in",
        "action=allow",
        "protocol=TCP",
        QString("localport=%1").arg(port),
        QString("program=%1").arg(m_iperf3Path)
    });
}

void BandwidthTester::removeFirewallRule() {
    QProcess::execute("netsh", {
        "advfirewall", "firewall", "delete", "rule",
        "name=SAK_iPerf3_Temp"
    });
}
```

---

## 🎯 Success Metrics

| Metric | Target | Importance |
|--------|--------|------------|
| Adapter scan time | < 2 seconds | High |
| Ping to localhost | < 1 ms RTT | High |
| Traceroute completion | < 30 seconds (30 hops) | Medium |
| DNS query time | < 100 ms (local DNS) | High |
| Port scan (100 ports) | < 10 seconds | High |
| Port scan (1000 ports) | < 30 seconds | Medium |
| iPerf3 accuracy | Within 5% of actual bandwidth | Critical |
| WiFi scan time | < 5 seconds | Medium |
| Connection monitor overhead | < 1% CPU | High |
| Firewall rule enumeration | < 5 seconds | Medium |
| Report generation | < 3 seconds | Medium |
| Panel tab switch | < 100 ms | High |

---

## 🔒 Security Considerations

### Port Scanning Ethics
- Display warning before scanning non-local targets
- Log all scan targets for audit trail
- Default target presets point to localhost / LAN only
- Do not support stealth scanning (SYN, FIN, Xmas tree) — TCP connect only

### iPerf3 Server Security
- Bind server to specific interface (not 0.0.0.0) by default
- Auto-create/remove firewall rules (minimize exposure window)
- Add option for one-time-use server (auto-stop after first client)
- Display server IP prominently so technician knows what's exposed

### Credential Protection
- Network share browser may trigger Windows auth prompts — SAK does not store credentials
- DNS cache display may reveal browsing history — warn before displaying

### Data Sensitivity
- Active connections reveal all process communications — treat as sensitive
- Firewall rules reveal security posture — sensitive in enterprise environments
- Reports may contain IP addresses, MAC addresses — PII considerations

---

## 💡 Future Enhancements (Post-v1.0)

### v1.1 - Advanced Features
- **Packet Capture**: Basic packet capture using Npcap (if installed) with protocol analysis
- **VLAN Discovery**: Detect VLAN configuration on trunk ports
- **Wake-on-LAN**: Send magic packets to power on remote machines
- **Network Topology Map**: Visual network map showing discovered hosts and routes
- **mDNS/LLMNR Discovery**: Discover local network services via multicast DNS
- **SSL/TLS Certificate Inspector**: Check certificates on HTTPS endpoints (expiry, chain, cipher)

### v1.2 - Enterprise Features
- **SNMP Monitoring**: Basic SNMP queries against managed switches/routers
- **Remote Network Diagnostics**: Run tests on remote SAK instances via orchestration
- **Continuous Monitoring Dashboard**: Track network health over time with alerting
- **Custom Test Suites**: Save/load test configurations for specific scenarios
- **Integration with Network Monitoring**: Export data to Nagios, Zabbix, PRTG formats
- **QoS Testing**: Application-level quality of service measurement (VoIP, video)
- **IPv6 Full Support**: DHCPv6, NDP, IPv6 traceroute with all features

---

## 📚 Resources

### Official Documentation
- [IP Helper API](https://learn.microsoft.com/windows/win32/iphlp/ip-helper-start-page)
- [ICMP API](https://learn.microsoft.com/windows/win32/iphlp/icmp-functions)
- [GetAdaptersAddresses](https://learn.microsoft.com/windows/win32/api/iphlpapi/nf-iphlpapi-getadaptersaddresses)
- [Native WiFi API](https://learn.microsoft.com/windows/win32/nativewifi/native-wifi-api-reference)
- [DnsQuery_W](https://learn.microsoft.com/windows/win32/api/windns/nf-windns-dnsquery_w)
- [INetFwPolicy2](https://learn.microsoft.com/windows/win32/api/netfw/nn-netfw-inetfwpolicy2)
- [GetExtendedTcpTable](https://learn.microsoft.com/windows/win32/api/iphlpapi/nf-iphlpapi-getextendedtcptable)
- [NetShareEnum](https://learn.microsoft.com/windows/win32/api/lmshare/nf-lmshare-netshareenum)
- [iPerf3 Documentation](https://iperf.fr/iperf-doc.php)

### Community Resources
- [iPerf3 GitHub](https://github.com/esnet/iperf)
- [iPerf3 Windows Builds](https://github.com/ar51an/iperf3-win-builds)
- [IEEE OUI Database](https://standards-oui.ieee.org/oui/oui.txt)
- [Nmap Port Database](https://svn.nmap.org/nmap/nmap-services)
- [WinMTR](https://github.com/White-Tiger/WinMTR)

---

## 📞 Support

**Questions?** Open a GitHub Discussion  
**Found a Bug?** Open a GitHub Issue  
**Want to Contribute?** See [CONTRIBUTING.md](CONTRIBUTING.md)

---

**Document Version**: 1.0  
**Last Updated**: February 25, 2026  
**Author**: Randy Northrup  
**Status**: ✅ Ready for Implementation
