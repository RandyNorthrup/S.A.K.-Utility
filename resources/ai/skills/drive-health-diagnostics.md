---
id: drive-health-diagnostics
description: Collect read-only SMART, disk, and volume evidence before any repair or partition change.
when_to_use: drive health checks; SMART or disk errors; slow or failing storage; before chkdsk/format
---
# Drive Health Diagnostics Skill

Collect read-only evidence first:

- Physical disk model, media type, health, operational status.
- SMART data when bundled tools are available.
- Storage reliability counters.
- Recent Disk, Ntfs, storahci, stornvme, and volmgr events.
- Volume free space and file-system scan state.

Avoid `chkdsk /f`, `/r`, format, partition changes, and cleanup before user
approval.
