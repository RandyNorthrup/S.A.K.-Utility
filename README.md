# S.A.K. Utility

> **Swiss Army Knife (S.A.K.) Utility** – Your all-in-one desktop companion for **backups**, **file organization**, **duplicate removal**, and **license key scanning**, with **multi-threaded speed** and **cross-platform support** for Windows & macOS.

---

## ✨ Features

### 🗂 Multi-User Backup System
- **Smart folder structure**: Backups saved as `backup/<user>/<folder>` automatically.
- **Blazing fast**: Multi-threaded file copying for large datasets.
- **Directory hierarchy preserved**: Nested folders remain intact.
- **Real-time progress**: Live percentage, speed, and file tracking.
- **Cancel anytime**: Abort backups on demand.
- **Detailed logs**: Every operation logged under `_logs/`.

### 📂 Directory Organizer
- **Auto-sort by file extension**: Group files into extension-based folders.
- **Collision handling**: Automatically renames duplicates.
- **Action logs**: Full record of all file moves.

### 🔍 Duplicate File Finder
- **Accurate MD5 detection**: Finds true duplicates regardless of name.
- **Flexible actions**: Delete, move, or export reports.
- **Safe deletions**: Keeps one original copy by default.
- **Full logging**: Every action documented.

### 🗝 License Key Scanner
- **Windows Registry & macOS Plists**: Detects potential license keys.
- **Admin prompt on Windows**: Elevation for deeper registry access.
- **Multi-threaded scanning**: Faster results on large systems.
- **Live progress bar**: Track scan percentage with cancel option.
- **Exportable results**: Save keys & logs for future reference.

### 🖥 Keep Screen Awake (Windows Only)
- **Prevents system sleep**: Ideal for long-running backups.
- **Fallback mode**: Cursor simulation if needed.

---

## ⚡ Installation

### Requirements
- **Python 3.9+**
- **PySide6** for GUI
- **Windows** or **macOS** (Linux support planned)

### Install dependencies
```bash
pip install PySide6
```

For Windows registry scanning:
```bash
pip install pywin32
```

### Run the app
```bash
python SAK_Utility_Full_With_MT_LicenseScan.py
```

---

## 🚀 Usage Guide

1. **Set Backup Location** → Choose where backups will be stored.
2. **Select Users** → Pick user profiles to include in backups.
3. **Start Backups** → One-click backups for Documents, Photos, etc.
4. **Organize Folders** → Sort files by extension automatically.
5. **Find Duplicates** → Delete, move, or export duplicate reports.
6. **Scan License Keys** → With admin privileges for full registry access.
7. **Keep Screen On** → Prevents sleep during long operations.

---

## ⚠️ Known Issues

- **License Key Limitations**: Many modern apps store keys in encrypted or cloud locations.
- **Registry Access**: Some Windows registry keys remain inaccessible even with admin rights.
- **macOS Plists**: Binary or encrypted plists may not reveal keys.
- **Backup Speed**: Disk I/O limits speed on slow drives.
- **Platform Differences**: Some features are Windows-only.

---

## 🔮 Future Enhancements

- **Modern GUI**: Tabs, dark mode, and custom themes.
- **Backup Scheduler**: Automate recurring backups.
- **Cloud Integration**: Direct backups to Google Drive, OneDrive, Dropbox.
- **Thread Control**: User-defined thread limits.
- **Integrated Log Viewer**: Read logs inside the app.
- **Linux Support**: Extend compatibility to Linux systems.
- **Incremental Backups**: Copy only changed files after first backup.

---

## 📜 License

This project is provided under the **MIT License** – use, modify, and distribute freely.
