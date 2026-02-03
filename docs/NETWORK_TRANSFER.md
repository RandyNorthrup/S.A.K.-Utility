# Network Transfer

## Overview
Network Transfer provides secure, encrypted, LAN-based migration of Windows user profile data between PCs. It supports discovery, authentication, encryption, resume, and integrity verification.

## Quick Start

### Source (Send)
1. Open **Network Transfer** tab.
2. Scan and select users.
3. Discover peers or enter destination IP/port.
4. Set passphrase (required if encryption enabled).
5. Start transfer.

### Destination (Receive)
1. Open **Network Transfer** tab.
2. Set destination base path.
3. Set passphrase (if encryption enabled).
4. Start listening.
5. Approve transfer after manifest review.

### Orchestrator (Multi-PC Deployment)
1. Switch to **Orchestrator (Deploy)** mode.
2. Start the orchestrator server and confirm status shows “Listening”.
3. Scan source users and select profiles for deployment.
4. Wait for destination PCs to register (Destinations table will populate).
5. Choose mapping type and strategy (One-to-Many, Many-to-Many, Custom).
6. Configure concurrency and bandwidth limits (global/per‑job).
7. Start deployment and monitor progress in the dashboard tables.
8. Use pause/resume/cancel for the overall deployment or individual jobs.
9. Export deployment summary (CSV/PDF) and history (CSV) when complete.

## Ports
- Discovery: 54321/UDP
- Control: 54322/TCP
- Data: 54323/TCP

## Security
- AES‑256‑GCM per chunk.
- Pre‑shared passphrase key derivation (PBKDF2).
- Auth challenge/response before manifest exchange.

## Resume
Interrupted transfers can resume using persisted resume ranges and partial files.

## Reports
Transfer reports are saved as JSON:
- Source: Documents/SAK/TransferReports
- Destination: <DestinationBase>/TransferReports

Deployment summaries and history are exported from the Orchestrator dashboard:
- Summary: CSV/PDF
- History: CSV

## Logs
Network operations are logged to the application log directory:
- <working directory>/_logs

Use these logs for debugging timeouts, authentication failures, and file verification errors.

## Troubleshooting
- Ensure firewall rules allow TCP 54322/54323 and UDP 54321.
- Verify both PCs are on the same subnet for discovery.
- If discovery is disabled, use manual IP.
- If authentication fails, confirm both sides use the same passphrase and time is synchronized.
- If transfer stalls, verify disk space on the destination and disable aggressive antivirus scanning.
- For permission-related errors, try switching the Permissions mode to "Assign to Destination" or "Strip All".
- Check <working directory>/_logs for detailed error traces and retry attempts.
- For Orchestrator deployments, ensure destination PCs can reach the orchestrator on TCP 54322.
- If a destination disconnects, it will re‑register automatically and resume queued assignments.
