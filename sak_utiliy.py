# -------------------- ALL IMPORTS AT TOP --------------------
import sys
import os
import re
import shutil
import time
import hashlib
import datetime
import json
import threading
import random
from concurrent.futures import ThreadPoolExecutor, as_completed

from PySide6.QtCore import QThread, Signal, Qt, QTimer
from PySide6.QtWidgets import (
    QMainWindow, QApplication, QPushButton, QWidget, QFileDialog, QMessageBox,
    QGridLayout, QProgressDialog, QSpacerItem, QSizePolicy, QCheckBox,
    QDialog, QVBoxLayout, QListWidget, QAbstractItemView, QTableWidget,
    QTableWidgetItem, QLabel
)

# Optional stdlib
try:
    import plistlib
except ImportError:
    plistlib = None

# Windows-specific imports (optional)
if sys.platform.startswith("win"):
    try:
        import winreg
    except ImportError:
        winreg = None
else:
    winreg = None

# Optional pywin32
try:
    import win32api, win32con  # type: ignore
except Exception:
    win32api = win32con = None

# ---------------------------------------------------------------------------
# Constants & Helpers
# ---------------------------------------------------------------------------
USER_ROOT = os.path.expandvars(r"C:\\Users") if sys.platform.startswith("win") else "/Users"
TIMESTAMP_FMT = "%Y-%m-%d_%H-%M-%S"


def ensure_logs_dir(root_dir: str) -> str:
    logs = os.path.join(root_dir, "_logs")
    os.makedirs(logs, exist_ok=True)
    return logs


def new_log_file(root_dir: str, prefix: str) -> str:
    ts = datetime.datetime.now().strftime(TIMESTAMP_FMT)
    return os.path.join(ensure_logs_dir(root_dir), f"{prefix}_{ts}.log")


def log_line(path: str, text: str) -> None:
    try:
        with open(path, "a", encoding="utf-8") as f:
            f.write(text.rstrip("\n") + "\n")
    except Exception:
        pass


def chunked_file_hash(filepath: str, chunk_size: int = 1024 * 1024) -> str:
    hasher = hashlib.md5()
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(chunk_size), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def fast_scan(base_path: str):
    """
    Faster, stack-based directory traversal using os.scandir.
    Yields (dir_path, [subdirs], [files]) without following symlinks.
    """
    stack = [base_path]
    visited = set()
    while stack:
        path = stack.pop()
        real = os.path.realpath(path)
        if real in visited:
            continue
        visited.add(real)
        try:
            dirs, files = [], []
            with os.scandir(path) as it:
                for entry in it:
                    try:
                        if entry.is_dir(follow_symlinks=False):
                            dirs.append(entry.path)
                        else:
                            files.append(entry.path)
                    except Exception:
                        # Broken symlink or permission error on entry
                        continue
            yield path, dirs, files
            stack.extend(dirs)
        except Exception:
            # Permission error on the directory itself – skip
            continue


# ---------------------------------------------------------------------------
# PermissionUpdateWorker: set RW for all (files) and RWX for all (dirs)
# ---------------------------------------------------------------------------
class PermissionUpdateWorker(QThread):
    progress = Signal(int, int, str)  # processed, total, current_path
    finished = Signal(bool, str)      # success, message

    def __init__(self, root_paths: list[str], log_file: str):
        super().__init__()
        self.root_paths = root_paths
        self.log_file = log_file
        self._cancel = False

    def cancel(self):
        self._cancel = True

    def _set_perms_posix(self, path: str, is_dir: bool):
        # Read/write for everyone on files (0o666), and rwx on dirs (0o777) to allow traversal
        mode = 0o777 if is_dir else 0o666
        try:
            os.chmod(path, mode)
        except Exception as e:
            log_line(self.log_file, f"PERM ERROR (chmod) {path}: {e}")

    def _set_perms_windows(self, base: str):
        """
        On Windows, it's much faster and more reliable to grant at the root
        using inheritance and let it cascade, than to call icacls per-item.
        We still compute progress using a dry traversal for the progress bar.
        """
        try:
            import subprocess
            # Grant Everyone Modify with inheritance to files (OI) and subcontainers (CI)
            subprocess.run(
                ["icacls", base, "/grant", "Everyone:(OI)(CI)M", "/T", "/C"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
            log_line(self.log_file, f"PERM WINDOWS ROOT-GRANT {base} -> Everyone:(OI)(CI)M (/T)")
        except Exception as e:
            log_line(self.log_file, f"PERM ERROR (icacls) {base}: {e}")

    def run(self):
        try:
            # Build list of items for progress (count dirs + files)
            all_items = []
            for base in self.root_paths:
                for root, dirs, files in fast_scan(base):
                    all_items.append(root)
                    all_items.extend(files)

            total = len(all_items)
            processed = 0

            if sys.platform.startswith("win"):
                # Apply grant at each selected root once (fast); then just "count through" for progress bar
                for base in self.root_paths:
                    self._set_perms_windows(base)

                for path in all_items:
                    if self._cancel:
                        self.finished.emit(False, "Permission update cancelled.")
                        return
                    # progress only (inheritance already applied)
                    processed += 1
                    self.progress.emit(processed, total, path)

            else:
                # POSIX: set perms on every item (files 666, dirs 777)
                for path in all_items:
                    if self._cancel:
                        self.finished.emit(False, "Permission update cancelled.")
                        return
                    self._set_perms_posix(path, is_dir=os.path.isdir(path))
                    processed += 1
                    self.progress.emit(processed, total, path)

            self.finished.emit(True, f"Permissions updated for {len(self.root_paths)} top-level path(s).")
        except Exception as e:
            log_line(self.log_file, f"PERM FATAL: {e}")
            self.finished.emit(False, f"Permission update failed: {e}")


# ---------------------------------------------------------------------------
# BackupWorker: threaded copy with optional MD5 verification
# ---------------------------------------------------------------------------
class BackupWorker(QThread):
    progress_update = Signal(int, str, int, int, float)  # files_processed, file, file_size, copied_size, speed
    finished = Signal(int, bool, str)  # files_copied, success, message

    def __init__(self, source_dir: str, destination_root: str, patterns, log_file: str,
                 max_workers: int | None = None, verify_hash: bool = True):
        super().__init__()
        self.source_dir = source_dir
        self.destination_root = destination_root
        self.patterns = patterns
        self.cancelled = False
        self.files_copied = 0
        self.total_size = 0
        self.copied_size = 0
        self.start_time = 0.0
        self.log_file = log_file
        self.verify_hash = verify_hash
        self.max_workers = max_workers or max(2, min(8, (os.cpu_count() or 4) * 2))
        self._lock = threading.Lock()

    def cancel(self):
        self.cancelled = True

    def run(self):
        try:
            # Build plan
            plan = []  # (src, dst, size)
            total_size = 0
            total_files = 0

            log_line(self.log_file, f"START Backup\n  Source: {self.source_dir}\n  DestRoot: {self.destination_root}\n  Verify: {self.verify_hash}")

            for root, dirs, files in fast_scan(self.source_dir):
                rel_path = os.path.relpath(root, self.source_dir)
                dst_folder = os.path.join(self.destination_root, rel_path)
                try:
                    os.makedirs(dst_folder, exist_ok=True)
                except Exception as e:
                    log_line(self.log_file, f"MKDIR ERROR {dst_folder}: {e}")

                for fname in files:
                    if not any(fname.lower().endswith(p.lstrip("*").lower()) or p == "*" for p in self.patterns):
                        continue
                    src = os.path.join(root, fname)
                    dst = os.path.join(dst_folder, fname)
                    try:
                        fsize = os.path.getsize(src)
                    except Exception:
                        fsize = 0
                    plan.append((src, dst, fsize))
                    total_files += 1
                    total_size += fsize

            self.total_size = total_size
            if total_files == 0:
                msg = f"No files found to backup in: {self.source_dir}"
                log_line(self.log_file, msg)
                self.finished.emit(0, False, msg)
                return

            self.start_time = time.time()

            def copy_one(task):
                src, dst, fsize = task
                if self.cancelled:
                    return False, src, dst, fsize, "cancelled"
                try:
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy2(src, dst)
                    if self.verify_hash:
                        if chunked_file_hash(src) != chunked_file_hash(dst):
                            log_line(self.log_file, f"HASH MISMATCH {src} -> {dst}")
                            return False, src, dst, fsize, "hash mismatch"
                    with self._lock:
                        self.files_copied += 1
                        self.copied_size += int(fsize)
                        files_processed = self.files_copied
                        copied_size = self.copied_size
                    elapsed = max(0.1, time.time() - self.start_time)
                    speed = copied_size / elapsed
                    self.progress_update.emit(files_processed, os.path.basename(src), int(fsize), copied_size, speed)
                    log_line(self.log_file, f"COPIED {src} -> {dst} ({int(fsize)} bytes)")
                    return True, src, dst, fsize, None
                except Exception as e:
                    log_line(self.log_file, f"ERROR copying {src} -> {dst}: {e}")
                    return False, src, dst, fsize, str(e)

            errors = 0
            with ThreadPoolExecutor(max_workers=self.max_workers) as pool:
                futures = [pool.submit(copy_one, t) for t in plan]
                for fut in as_completed(futures):
                    ok, _, _, _, err = fut.result()
                    if self.cancelled:
                        break
                    if not ok:
                        errors += 1

            if self.cancelled:
                log_line(self.log_file, "CANCELLED by user")
                self.finished.emit(self.files_copied, False, "Backup cancelled by user.")
            else:
                summary = f"DONE. Files copied: {self.files_copied}; Errors: {errors}"
                log_line(self.log_file, summary)
                self.finished.emit(self.files_copied, errors == 0,
                                   "Backup completed successfully." if errors == 0 else summary)

        except Exception as e:
            log_line(self.log_file, f"FATAL: {e}")
            self.finished.emit(self.files_copied, False, f"Backup failed: {e}")


# ---------------------------------------------------------------------------
# LicenseScanWorker: Windows Registry + macOS plist recursive scanning
# ---------------------------------------------------------------------------
class LicenseScanWorker(QThread):
    progress = Signal(int, int, str)   # processed, total, label
    finished = Signal(dict, str)       # results, log_file
    failed = Signal(str)               # error message

    def __init__(self, log_root: str):
        super().__init__()
        self.log_root = log_root or os.getcwd()
        self.log_file = new_log_file(self.log_root, "licenses")
        self._cancel = False

    def cancel(self):
        self._cancel = True

    def run(self):
        try:
            if sys.platform.startswith("win"):
                results = self._scan_windows()
            elif sys.platform.startswith("darwin"):
                results = self._scan_macos()
            else:
                self.failed.emit("Unsupported platform for license key scan.")
                return
            self.finished.emit(results, self.log_file)
        except Exception as e:
            log_line(self.log_file, f"FATAL: {e}")
            self.failed.emit(str(e))

    # -------------------------- Windows ---------------------------
    def _scan_windows(self) -> dict:
        if not winreg:
            return {}

        targets = [
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Wow6432Node\Microsoft\Windows NT\CurrentVersion"),
            (winreg.HKEY_CURRENT_USER,  r"SOFTWARE"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Office"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Wow6432Node\Microsoft\Office"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),
            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Wow6432Node\Microsoft\Windows\CurrentVersion\Uninstall"),
        ]

        total = len(targets)
        processed = 0
        results = {}
        pattern = re.compile(r"[A-Z0-9]{5}(?:-[A-Z0-9]{5}){4}")

        def safe_tostr(value):
            try:
                if isinstance(value, bytes):
                    return value.decode("utf-8", errors="ignore")
                return str(value)
            except Exception:
                return None

        def looks_like_key(name: str, value: str) -> bool:
            if not isinstance(value, str):
                return False
            nl = name.lower()
            return ("key" in nl or "license" in nl or pattern.search(value) is not None)

        def scan_path(hive, subkey):
            found_local = {}
            stack = [subkey]
            while stack and not self._cancel:
                key_path = stack.pop()
                try:
                    reg = winreg.OpenKey(hive, key_path)
                except Exception as e:
                    log_line(self.log_file, f"SKIP {key_path}: {e}")
                    continue
                try:
                    i = 0
                    while True and not self._cancel:
                        try:
                            name, value, vtype = winreg.EnumValue(reg, i)
                            sval = safe_tostr(value)
                            if sval and looks_like_key(name, sval):
                                found_local[f"{key_path}\\{name}"] = sval
                                log_line(self.log_file, f"FOUND {key_path}\\{name} -> {sval}")
                            i += 1
                        except OSError:
                            break
                    j = 0
                    while True and not self._cancel:
                        try:
                            sub = winreg.EnumKey(reg, j)
                            stack.append(key_path + "\\" + sub)
                            j += 1
                        except OSError:
                            break
                finally:
                    try:
                        winreg.CloseKey(reg)
                    except Exception:
                        pass
            return found_local

        with ThreadPoolExecutor(max_workers=min(8, (os.cpu_count() or 4) * 2)) as pool:
            futures = [pool.submit(scan_path, hive, sub) for hive, sub in targets]
            for fut in as_completed(futures):
                if self._cancel:
                    break
                results.update(fut.result())
                processed += 1
                self.progress.emit(processed, total, f"Windows Registry ({processed}/{total})")

        return results

    # --------------------------- macOS ----------------------------
    def _scan_macos(self) -> dict:
        if not plistlib:
            return {}

        results = {}
        pattern = re.compile(r"[A-Z0-9]{5}(?:-[A-Z0-9]{5}){4}")
        search_paths = ["/Library/Preferences", os.path.expanduser("~/Library/Preferences")]
        files = []
        for base in search_paths:
            if os.path.exists(base):
                try:
                    for f in os.listdir(base):
                        if f.endswith(".plist"):
                            files.append(os.path.join(base, f))
                except Exception as e:
                    log_line(self.log_file, f"LIST ERROR {base}: {e}")

        total = len(files)
        processed = 0

        def looks_like_key(name: str, value: str) -> bool:
            if not isinstance(value, str):
                return False
            nl = (name or "").lower()
            return ("key" in nl or "license" in nl or pattern.search(value) is not None)

        def flatten_items(obj, prefix=""):
            if isinstance(obj, dict):
                for k, v in obj.items():
                    yield from flatten_items(v, f"{prefix}{k}.")
            elif isinstance(obj, list):
                for i, v in enumerate(obj):
                    yield from flatten_items(v, f"{prefix}{i}.")
            else:
                yield prefix[:-1], str(obj)

        def scan_one(path):
            local_found = {}
            try:
                with open(path, "rb") as fp:
                    data = plistlib.load(fp)
                for k, v in flatten_items(data):
                    if looks_like_key(k, v):
                        key = f"{os.path.basename(path)}:{k}"
                        local_found[key] = v
                        log_line(self.log_file, f"FOUND {path}:{k} -> {v}")
            except Exception as e:
                log_line(self.log_file, f"ERROR reading {path}: {e}")
            return local_found

        with ThreadPoolExecutor(max_workers=min(8, (os.cpu_count() or 4) * 2)) as pool:
            futures = [pool.submit(scan_one, p) for p in files]
            for fut in as_completed(futures):
                if self._cancel:
                    break
                results.update(fut.result())
                processed += 1
                self.progress.emit(processed, total, f"Plists ({processed}/{total})")

        return results


# ---------------------------------------------------------------------------
# Main Window
# ---------------------------------------------------------------------------
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()

        self.setWindowTitle("S.A.K. Utility")
        self.setFixedSize(940, 700)

        # State
        self.backup_location = ""
        self.selected_users = {}        # username -> absolute path under USER_ROOT
        self.resolved_sources = {}      # button name -> list[absolute paths]
        self._backup_queue = []
        self.keep_screen_on_running = False

        # UI layout
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        layout = QGridLayout()
        central_widget.setLayout(layout)

        cols = 3
        layout.setHorizontalSpacing(12)
        layout.setVerticalSpacing(12)
        layout.setContentsMargins(16, 16, 16, 16)
        for c in range(cols):
            layout.setColumnStretch(c, 1)

        # Keep Screen On
        self.setup_keep_screen_on(layout, 8, cols)

        # Hash verify checkbox
        self.verify_checkbox = QCheckBox("Verify Copies (MD5)", self)
        self.verify_checkbox.setChecked(True)
        layout.addWidget(self.verify_checkbox, 7, 0, 1, cols)

        # Hidden placeholder for layout symmetry (dedupe table holder)
        self.setup_dedupe_table(layout, 9, cols)

        # Backup location button
        self.backup_location_button = QPushButton("Set Backup Location", self)
        self.backup_location_button.setToolTip("Choose the folder where backups will be stored.")
        self.backup_location_button.clicked.connect(self.set_backup_location)
        layout.addWidget(self.backup_location_button, 0, 0, 1, cols)

        # Backup sources per platform
        if sys.platform.startswith("win"):
            user_subpaths = {
                "Backup Contacts": (os.path.join("{user}", "Contacts"), ["*"], "Contacts"),
                "Backup Photos": (os.path.join("{user}", "Pictures"), ["*"], "Pictures"),
                "Backup Documents": (os.path.join("{user}", "Documents"), ["*"], "Documents"),
                "Backup Videos": (os.path.join("{user}", "Videos"), ["*"], "Videos"),
                "Backup Music": (os.path.join("{user}", "Music"), ["*"], "Music"),
                "Backup Desktop": (os.path.join("{user}", "Desktop"), ["*"], "Desktop"),
                "Backup Downloads": (os.path.join("{user}", "Downloads"), ["*"], "Downloads"),
                "Backup Outlook Files": (os.path.join("{user}", "AppData", "Local", "Microsoft", "Outlook"), ["*.pst", "*.ost", "*.nst"], "Outlook"),
            }
        else:
            user_subpaths = {
                "Backup Contacts": (os.path.join("{user}", "Contacts"), ["*"], "Contacts"),
                "Backup Photos": (os.path.join("{user}", "Pictures"), ["*"], "Pictures"),
                "Backup Documents": (os.path.join("{user}", "Documents"), ["*"], "Documents"),
                "Backup Videos": (os.path.join("{user}", "Movies"), ["*"], "Movies"),
                "Backup Music": (os.path.join("{user}", "Music"), ["*"], "Music"),
                "Backup Desktop": (os.path.join("{user}", "Desktop"), ["*"], "Desktop"),
                "Backup Downloads": (os.path.join("{user}", "Downloads"), ["*"], "Downloads"),
            }
        self.backup_sources = user_subpaths

        # Create backup buttons
        self.backup_buttons = []
        for idx, (name, (subpath_tpl, pats, dest)) in enumerate(self.backup_sources.items()):
            btn = self.create_button(name, lambda checked=False, n=name: self.trigger_backup(n))
            btn.setToolTip(f"Backup all files from selected users' {dest} folders.")
            btn.setFixedSize(220, 44)
            btn.setEnabled(False)
            self.backup_buttons.append((btn, name))
            row = 1 + (idx // cols)
            col = idx % cols
            layout.addWidget(btn, row, col)

        # Action buttons row
        action_row = 2 + ((len(self.backup_sources) + cols - 1) // cols)

        self.organize_button = QPushButton("Organize Directory", self)
        self.organize_button.setToolTip("Organize all files in a selected directory into subfolders by extension.")
        self.organize_button.clicked.connect(self.organize_directory)
        self.organize_button.setFixedSize(220, 44)
        layout.addWidget(self.organize_button, action_row, 0)

        self.dedup_button = QPushButton("De-Duplicate", self)
        self.dedup_button.setToolTip("Scan a folder for duplicate files and take action on them.")
        self.dedup_button.clicked.connect(self.run_deduplication)
        self.dedup_button.setFixedSize(220, 44)
        layout.addWidget(self.dedup_button, action_row, 1)

        self.license_button = QPushButton("Scan for License Keys", self)
        self.license_button.setToolTip("Scan your system for software license keys and save a report.")
        self.license_button.clicked.connect(self.scan_for_license_keys)
        self.license_button.setFixedSize(220, 44)
        layout.addWidget(self.license_button, action_row, 2)

        # Select users button
        select_users_row = action_row + 1
        self.select_users_button = QPushButton("Select User(s) for Backup", self)
        self.select_users_button.setToolTip("Pick one or more user profiles to include in backups.")
        self.select_users_button.clicked.connect(self.select_users_for_backup)
        self.select_users_button.setFixedSize(240, 44)
        layout.addWidget(self.select_users_button, select_users_row, 0, 1, cols)

        # Spacer
        total_rows = 1 + ((len(self.backup_sources) + cols - 1) // cols)
        layout.setRowStretch(total_rows + 2, 1)
        layout.addItem(QSpacerItem(0, 0, QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Expanding), total_rows + 2, 0, 1, cols)

        # Auto-refresh timer
        self.refresh_timer = QTimer(self)
        self.refresh_timer.timeout.connect(self.check_source_folders)
        self.refresh_timer.start(5000)

    # ------------------------------------------------------------------
    # UI helpers
    # ------------------------------------------------------------------
    def create_button(self, text, command):
        btn = QPushButton(text, self)
        btn.clicked.connect(command)
        return btn

    def setup_dedupe_table(self, layout, row, cols):
        self.dedupe_table = QTableWidget(self)
        self.dedupe_table.hide()
        layout.addWidget(self.dedupe_table, row, 0, 1, cols)

    def setup_keep_screen_on(self, layout, row, cols):
        self.keep_screen_on_checkbox = QCheckBox("Keep Screen On", self)
        self.keep_screen_on_checkbox.stateChanged.connect(self.toggle_keep_screen_on)
        layout.addWidget(self.keep_screen_on_checkbox, row, 0, 1, cols)
        if sys.platform != "win32":
            self.keep_screen_on_checkbox.setEnabled(False)
            self.keep_screen_on_checkbox.setToolTip("This feature is only available on Windows.")

    # ------------------------------------------------------------------
    # Keep-awake (Windows)
    # ------------------------------------------------------------------
    def toggle_keep_screen_on(self, state):
        if sys.platform == "win32":
            try:
                import ctypes
                ES_CONTINUOUS = 0x80000000
                ES_DISPLAY_REQUIRED = 0x00000002
                ES_SYSTEM_REQUIRED = 0x00000001
                if state:
                    ctypes.windll.kernel32.SetThreadExecutionState(
                        ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED
                    )
                else:
                    ctypes.windll.kernel32.SetThreadExecutionState(0)
            except Exception:
                if state and win32api and win32con:
                    self.keep_screen_on_running = True
                    self.keep_screen_on_thread = threading.Thread(target=self.keep_screen_on_worker, daemon=True)
                    self.keep_screen_on_thread.start()
                else:
                    self.keep_screen_on_running = False

    def keep_screen_on_worker(self):
        self.keep_screen_on_running = True
        while getattr(self, "keep_screen_on_running", False):
            x = random.randint(0, 100)
            y = random.randint(0, 100)
            try:
                win32api.SetCursorPos((x, y))
                win32api.mouse_event(win32con.MOUSEEVENTF_LEFTDOWN, x, y, 0, 0)
                win32api.mouse_event(win32con.MOUSEEVENTF_LEFTUP, x, y, 0, 0)
            except Exception:
                break
            time.sleep(15)

    # ------------------------------------------------------------------
    # Backup location & user selection
    # ------------------------------------------------------------------
    def set_backup_location(self):
        folder = QFileDialog.getExistingDirectory(self, "Select Backup Location")
        if folder:
            self.backup_location = folder
            self.update_backup_buttons()

    def select_users_for_backup(self):
        import getpass
        current_user = getpass.getuser()
        try:
            user_dirs = [d for d in os.listdir(USER_ROOT) if os.path.isdir(os.path.join(USER_ROOT, d))]
        except Exception as e:
            QMessageBox.warning(self, "Error", f"Failed to list user directories: {e}")
            return
        if not user_dirs:
            QMessageBox.information(self, "No Users", "No user directories found.")
            return

        dlg = QDialog(self)
        dlg.setWindowTitle("Select User(s) for Backup")
        vbox = QVBoxLayout(dlg)
        listw = QListWidget(dlg)
        listw.addItems(sorted(user_dirs))
        listw.setSelectionMode(QAbstractItemView.SelectionMode.MultiSelection)
        vbox.addWidget(listw)
        ok_btn = QPushButton("OK", dlg)
        ok_btn.clicked.connect(dlg.accept)
        vbox.addWidget(ok_btn)
        dlg.setMinimumWidth(350)

        if not dlg.exec():
            return

        selected = [item.text() for item in listw.selectedItems()] or [current_user]
        self.selected_users = {u: os.path.join(USER_ROOT, u) for u in selected}

        # Resolve per-button sources
        self.resolved_sources.clear()
        for btn, name in self.backup_buttons:
            subpath_tpl, _, dest = self.backup_sources[name]
            paths = []
            for u, base in self.selected_users.items():
                subpath = subpath_tpl.format(user=u)
                abs_path = os.path.join(USER_ROOT, subpath)
                if os.path.exists(abs_path):
                    paths.append(abs_path)
            self.resolved_sources[name] = paths

        self.update_backup_buttons()
        QMessageBox.information(self, "Selection Saved", "User selection has been saved. Use the backup buttons to start backups.")

    def update_backup_buttons(self):
        if not self.backup_location:
            for btn, _ in self.backup_buttons:
                btn.setEnabled(False)
                btn.setToolTip("Please set a backup location first.")
            return

        for btn, name in self.backup_buttons:
            paths = self.resolved_sources.get(name, [])
            dest_folder = self.backup_sources[name][2]
            if paths:
                targets = []
                for p in paths:
                    user = os.path.basename(os.path.dirname(p))
                    targets.append(os.path.join(self.backup_location, user, dest_folder))
                btn.setEnabled(True)
                btn.setToolTip("\n".join(["Will back up to:"] + targets))
            else:
                btn.setEnabled(False)
                btn.setToolTip("No valid source selected for this backup.")

    def check_source_folders(self):
        if not self.backup_location:
            for btn, _ in self.backup_buttons:
                btn.setEnabled(False)
            return
        for btn, name in self.backup_buttons:
            btn.setEnabled(bool(self.resolved_sources.get(name)))

    # ------------------------------------------------------------------
    # Backup flow (permissions first, then backup)
    # ------------------------------------------------------------------
    def trigger_backup(self, name: str):
        paths = self.resolved_sources.get(name)
        if not paths:
            QMessageBox.warning(self, "Error", f"No valid sources for {name}.")
            return
        _, patterns, dest = self.backup_sources[name]
        self._backup_queue = []
        self._pending_permission_update = []

        for p in paths:
            user = os.path.basename(os.path.dirname(p))
            destination_root = os.path.join(self.backup_location, user, dest)
            os.makedirs(destination_root, exist_ok=True)
            log_file = new_log_file(destination_root, f"backup_{dest}_{user}")
            log_line(log_file, f"Preparing backup for {user}:{dest}\n  Source: {p}\n  Destination root: {destination_root}")
            self._backup_queue.append((p, patterns, destination_root, log_file))
            self._pending_permission_update.append((p, log_file))

        # Start permission update before backup
        self._start_permission_update()

    def _start_permission_update(self):
        if not self._pending_permission_update:
            self._start_next_backup()
            return

        paths = [p for p, _ in self._pending_permission_update]
        log_file = self._pending_permission_update[0][1]

        self.perm_dialog = QProgressDialog("Updating permissions...", "Cancel", 0, 100, self)
        self.perm_dialog.setWindowTitle("Permission Update Progress")
        self.perm_dialog.setWindowModality(Qt.WindowModality.ApplicationModal)
        self.perm_dialog.setValue(0)
        self.perm_dialog.canceled.connect(self._cancel_permission_update)

        self.perm_worker = PermissionUpdateWorker(paths, log_file)
        self.perm_worker.progress.connect(self._update_perm_progress)
        self.perm_worker.finished.connect(self._permission_update_finished)
        self.perm_worker.start()
        self.perm_dialog.show()

    def _update_perm_progress(self, processed, total, current_path):
        pct = int((processed / max(1, total)) * 100)
        pct = min(100, max(0, pct))
        self.perm_dialog.setValue(pct)
        self.perm_dialog.setLabelText(f"Setting permissions: {current_path} ({pct}%)")

    def _cancel_permission_update(self):
        if hasattr(self, 'perm_worker') and self.perm_worker.isRunning():
            self.perm_worker.cancel()
            self.perm_dialog.setLabelText("Cancelling permission update...")

    def _permission_update_finished(self, success, message):
        self.perm_dialog.close()
        if not success:
            QMessageBox.warning(self, "Permission Update", message)
            self._backup_queue = []
            return
        self._start_next_backup()

    def _start_next_backup(self):
        if not self._backup_queue:
            return
        source_dir, patterns, destination_root, log_file = self._backup_queue.pop(0)
        self.start_backup(source_dir, patterns, destination_root, log_file)

    def start_backup(self, source_dir, patterns, destination_root, log_file):
        if not self.backup_location:
            QMessageBox.warning(self, "Error", "No backup location set. Please set a backup location first.")
            return
        if not source_dir:
            QMessageBox.warning(self, "Error", "No source directory selected for backup.")
            return

        self.progress_dialog = QProgressDialog("Backing up files...", "Cancel", 0, 100, self)
        self.progress_dialog.setWindowTitle("Backup Progress")
        self.progress_dialog.setWindowModality(Qt.WindowModality.ApplicationModal)
        self.progress_dialog.canceled.connect(self.cancel_backup)
        self.progress_dialog.show()

        verify_hash = self.verify_checkbox.isChecked()
        self.worker = BackupWorker(source_dir, destination_root, patterns, log_file, verify_hash=verify_hash)
        self.worker.progress_update.connect(self.update_progress)
        self.worker.finished.connect(self.backup_finished)
        self.worker.start()

    def update_progress(self, files_processed, current_file, file_size, copied_size, speed):
        total = getattr(self.worker, 'total_size', 0)
        if total > 0:
            pct = int((copied_size / max(1, total)) * 100)
            pct = min(100, max(0, pct))
        else:
            pct = 0
        self.progress_dialog.setValue(pct)
        self.progress_dialog.setLabelText(
            f"{files_processed} files | {copied_size} / {total} bytes "
            f"({pct}%) @ {int(speed)} B/s\nCurrent: {current_file} ({file_size} bytes)"
        )

    def cancel_backup(self):
        if hasattr(self, 'worker') and self.worker.isRunning():
            self.worker.cancel()
            self.progress_dialog.setLabelText("Cancelling backup...")

    def backup_finished(self, files_copied, success, message):
        self.progress_dialog.close()
        if success:
            QMessageBox.information(self, "Backup Completed", message)
            self._start_next_backup()
        else:
            QMessageBox.warning(self, "Backup Status", message)
            self._backup_queue = []

    # ------------------------------------------------------------------
    # Organize
    # ------------------------------------------------------------------
    def organize_directory(self):
        path = QFileDialog.getExistingDirectory(self, "Select Directory to Organize")
        if not path:
            return
        preview_msg = (
            "This will move files into subfolders named by extension under:\n"
            f"  {path}\n\nA detailed log will be saved under: {ensure_logs_dir(path)}"
        )
        confirm = QMessageBox.question(
            self,
            "Confirm Organize",
            preview_msg,
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
        )
        if confirm != QMessageBox.StandardButton.Yes:
            return
        if not os.path.exists(path):
            QMessageBox.warning(self, "Error", "The specified directory does not exist.")
            return

        log_file = new_log_file(path, "organize")
        moved_count = 0
        for file in os.listdir(path):
            file_path = os.path.join(path, file)
            if os.path.isdir(file_path):
                continue
            filename, extension = os.path.splitext(file)
            extension = extension[1:] if extension else "NoExtension"
            dest_folder = os.path.join(path, extension)
            os.makedirs(dest_folder, exist_ok=True)
            dest_file_path = os.path.join(dest_folder, file)
            counter = 1
            while os.path.exists(dest_file_path):
                new_filename = f"{filename}_{counter}.{extension}" if extension != "NoExtension" else f"{filename}_{counter}"
                dest_file_path = os.path.join(dest_folder, new_filename)
                counter += 1
            try:
                shutil.move(file_path, dest_file_path)
                moved_count += 1
                log_line(log_file, f"MOVED {file_path} -> {dest_file_path}")
            except Exception as e:
                log_line(log_file, f"ERROR moving {file_path}: {e}")
        QMessageBox.information(self, "Organize Directory", f"Moved {moved_count} files into subfolders by extension.\nLog: {log_file}")

    # ------------------------------------------------------------------
    # De-duplicate
    # ------------------------------------------------------------------
    def run_deduplication(self):
        directory = QFileDialog.getExistingDirectory(self, "Select Directory to Scan for Duplicates")
        if not directory:
            return

        min_size = 0
        file_extensions = None
        duplicates = self.find_duplicates(directory, min_size, file_extensions)
        if not duplicates:
            QMessageBox.information(self, "De-Duplicate", "No duplicates found.")
            return

        dialog = QDialog(self)
        dialog.setWindowTitle("Duplicate Files Found")
        dialog.setMinimumSize(860, 520)
        vbox = QVBoxLayout(dialog)
        vbox.addWidget(QLabel(f"Scanning directory: {directory}"))
        table = QTableWidget(dialog)
        table.setColumnCount(2)
        table.setHorizontalHeaderLabels(["Hash", "File Path"])
        table.setEditTriggers(table.EditTrigger.NoEditTriggers)
        table.setSelectionBehavior(table.SelectionBehavior.SelectRows)
        table.setSelectionMode(table.SelectionMode.SingleSelection)
        table.verticalHeader().setVisible(False)
        table.setRowCount(0)
        for hash_val, paths in duplicates.items():
            for path in paths:
                row = table.rowCount()
                table.insertRow(row)
                table.setItem(row, 0, QTableWidgetItem(hash_val))
                table.setItem(row, 1, QTableWidgetItem(path))
        table.resizeColumnsToContents()
        vbox.addWidget(table)

        close_btn = QPushButton("Close", dialog)
        close_btn.clicked.connect(dialog.accept)
        vbox.addWidget(close_btn)
        dialog.exec()

        action_box = QMessageBox(self)
        action_box.setWindowTitle("Choose Action")
        action_box.setText("What would you like to do with the duplicates?")
        delete_btn = action_box.addButton("Delete Duplicates", QMessageBox.ButtonRole.AcceptRole)
        move_btn = action_box.addButton("Move Duplicates", QMessageBox.ButtonRole.ActionRole)
        report_btn = action_box.addButton("Save Report", QMessageBox.ButtonRole.ActionRole)
        action_box.addButton(QMessageBox.StandardButton.Cancel)
        action_box.exec()

        log_file = new_log_file(directory, "dedupe")
        clicked = action_box.clickedButton()
        if clicked == delete_btn:
            total_deleted = 0
            for _, paths in duplicates.items():
                for path in paths[1:]:
                    try:
                        os.remove(path)
                        total_deleted += 1
                        log_line(log_file, f"DELETED {path}")
                    except Exception as e:
                        log_line(log_file, f"ERROR deleting {path}: {e}")
            QMessageBox.information(self, "Done", f"Duplicates deleted: {total_deleted}\nLog: {log_file}")
        elif clicked == move_btn:
            target_dir = QFileDialog.getExistingDirectory(self, "Select Directory to Move Duplicates To")
            if not target_dir:
                return
            moved = 0
            for _, paths in duplicates.items():
                for path in paths[1:]:
                    try:
                        base = os.path.basename(path)
                        target_path = os.path.join(target_dir, base)
                        if os.path.exists(target_path):
                            stem, ext = os.path.splitext(base)
                            counter = 1
                            while os.path.exists(target_path):
                                target_path = os.path.join(target_dir, f"{stem}_{counter}{ext}")
                                counter += 1
                        os.rename(path, target_path)
                        moved += 1
                        log_line(log_file, f"MOVED {path} -> {target_path}")
                    except Exception as e:
                        log_line(log_file, f"ERROR moving {path}: {e}")
            QMessageBox.information(self, "Done", f"Duplicates moved: {moved}\nDestination: {target_dir}\nLog: {log_file}")
        elif clicked == report_btn:
            report_path, _ = QFileDialog.getSaveFileName(self, "Save Duplicates Report", "duplicates_report.json", "JSON Files (*.json);;All Files (*)")
            if not report_path:
                return
            self.generate_report(duplicates, report_path)
            log_line(log_file, f"REPORT saved to {report_path}")
            QMessageBox.information(self, "Done", f"Report saved to {report_path}\nLog: {log_file}")
        else:
            QMessageBox.information(self, "No Action", "No action taken.")

    def find_duplicates(self, directory, min_size=0, file_extensions=None):
        hashes = {}
        duplicates = {}
        for dirpath, dirnames, filenames in fast_scan(directory):
            for filename in filenames:
                if file_extensions and not filename.lower().endswith(tuple(file_extensions)):
                    continue
                filepath = os.path.join(dirpath, os.path.basename(filename)) if not os.path.isabs(filename) else filename
                try:
                    if os.path.getsize(filepath) < min_size:
                        continue
                except Exception:
                    continue
                try:
                    file_hash = chunked_file_hash(filepath)
                except Exception:
                    continue
                if file_hash in hashes:
                    duplicates.setdefault(file_hash, []).append(filepath)
                    if hashes[file_hash] not in duplicates[file_hash]:
                        duplicates[file_hash].append(hashes[file_hash])
                else:
                    hashes[file_hash] = filepath
        return {k: v for k, v in duplicates.items() if len(v) > 1}

    def generate_report(self, duplicates, report_path):
        with open(report_path, 'w', encoding='utf-8') as report_file:
            json.dump(duplicates, report_file, indent=4)

    # ------------------------------------------------------------------
    # License keys scanning (uses LicenseScanWorker, elevation prompt on Windows)
    # ------------------------------------------------------------------
    def scan_for_license_keys(self):
        # If Windows, check admin and offer elevation
        if sys.platform.startswith('win'):
            if not self._is_user_admin():
                resp = QMessageBox.question(
                    self,
                    "Administrator Privileges Recommended",
                    "Scanning the registry works best with administrator rights.\n\nRestart this app as Administrator now?",
                    QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                )
                if resp == QMessageBox.StandardButton.Yes:
                    self._restart_as_admin()
                    return  # If relaunch succeeded, current process will quit
                # else continue without admin (limited scan)

        # Start worker
        root_for_logs = self.backup_location or os.getcwd()
        self.lic_worker = LicenseScanWorker(root_for_logs)

        # Progress dialog
        self.lic_prog = QProgressDialog("Scanning for license keys...", "Cancel", 0, 100, self)
        self.lic_prog.setWindowModality(Qt.WindowModality.ApplicationModal)
        self.lic_prog.setAutoClose(False)
        self.lic_prog.setAutoReset(False)
        self.lic_prog.setMinimumDuration(0)
        self.lic_prog.setValue(0)

        def on_progress(done, total, label):
            total = max(1, total)
            pct = int((done / total) * 100)
            pct = min(100, max(0, pct))
            self.lic_prog.setValue(pct)
            self.lic_prog.setLabelText(f"{label} — {pct}%")

        def on_finished(results: dict, log_file: str):
            self.lic_prog.close()
            if results:
                save_path, _ = QFileDialog.getSaveFileName(self, "Save License Keys Report", "licenses.json", "JSON Files (*.json);;All Files (*)")
                if save_path:
                    with open(save_path, 'w', encoding='utf-8') as f:
                        json.dump(results, f, indent=4)
                QMessageBox.information(self, "License Scan Complete", f"Found {len(results)} potential keys.\nLog: {log_file}")
            else:
                QMessageBox.information(self, "License Scan Complete", f"No obvious license keys found.\nLog: {log_file}")

        def on_failed(msg: str):
            self.lic_prog.close()
            QMessageBox.warning(self, "License Scan Failed", msg)

        def on_cancel():
            self.lic_worker.cancel()

        self.lic_worker.progress.connect(on_progress)
        self.lic_worker.finished.connect(on_finished)
        self.lic_worker.failed.connect(on_failed)
        self.lic_prog.canceled.connect(on_cancel)

        self.lic_worker.start()
        self.lic_prog.show()

    # ---- Elevation helpers (Windows) ----
    def _is_user_admin(self) -> bool:
        if not sys.platform.startswith('win'):
            return False
        try:
            import ctypes
            return bool(ctypes.windll.shell32.IsUserAnAdmin())
        except Exception:
            return False

    def _restart_as_admin(self):
        try:
            import ctypes, sys as _sys
            params = ' '.join([f'"{a}"' for a in _sys.argv])
            r = ctypes.windll.shell32.ShellExecuteW(None, "runas", _sys.executable, params, None, 1)
            if r <= 32:
                QMessageBox.warning(self, "Elevation", "Failed to restart as Administrator. You can still run a limited scan.")
            else:
                QApplication.instance().quit()
        except Exception as e:
            QMessageBox.warning(self, "Elevation", f"Could not request elevation: {e}")


# ---------------------------------------------------------------------------
# Entrypoint
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())
