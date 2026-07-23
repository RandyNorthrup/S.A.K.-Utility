---
id: windows-update-repair
description: Diagnose Windows Update (services, pending reboot, CBS/DISM, error code) before low-risk-first repair.
when_to_use: Windows Update failures; update error codes; stuck or failing updates
---
# Windows Update Repair Skill

Diagnose before repair:

- Windows Update services.
- Pending reboot.
- Recent WindowsUpdateClient events.
- CBS/DISM health.
- Network/proxy clues.
- Update error code.

Repair order should prefer low-risk service/cache checks before DISM/SFC or broad
resets. Verify with update scan after repair.
