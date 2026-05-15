# Drive Health Diagnostics Skill

Collect read-only evidence first:

- Physical disk model, media type, health, operational status.
- SMART data when bundled tools are available.
- Storage reliability counters.
- Recent Disk, Ntfs, storahci, stornvme, and volmgr events.
- Volume free space and file-system scan state.

Avoid `chkdsk /f`, `/r`, format, partition changes, and cleanup before user
approval.
