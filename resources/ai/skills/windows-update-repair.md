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
