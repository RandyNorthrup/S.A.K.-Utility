// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/offline_profile_source.h"
#include "sak/user_profile_types.h"

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QTableWidget>
#include <QWizardPage>

#include <expected>
#include <functional>
#include <optional>

namespace sak {

/**
 * @brief Wizard step 2: which Windows installation the backup reads.
 *
 * "This PC" is today's flow and the default; "Another Windows installation" runs the offline
 * scan (docs/OFFLINE_PROFILE_RESCUE_PLAN.md). Nothing is read here: the
 * scan happens on the next page, so choosing this option costs the user no disk time.
 */
class UserProfileBackupSourcePage : public QWizardPage {
    Q_OBJECT

public:
    /// Unscoped so QWizard field values and tests compare without casts.
    enum Source {
        SourceThisPc = 0,
        SourceOtherWindows,
    };

    explicit UserProfileBackupSourcePage(QWidget* parent = nullptr);

    [[nodiscard]] Source selectedSource() const;
    /// @brief Choose a source without a click. The unit tests use it; production uses the radios.
    void setSourceSelected(Source source);

    /// @brief This PC goes to today's live scan page; another installation goes to the offline
    ///        list. Both jumps are stated here so the page-number order never decides the flow.
    int nextId() const override;

private:
    void setupUi();

    QRadioButton* m_thisPcRadio{nullptr};
    QRadioButton* m_otherRadio{nullptr};
};

/**
 * @brief The one place a BitLocker recovery key is typed.
 *
 * The field echoes as a password, the key is handed back to the caller as typed, and nothing in
 * this dialog logs it. Validation is OfflineProfileSource::normalizeRecoveryKey, so a key that
 * reaches the elevation helper is already 48 digits in 8 groups -- and a wrong-length key is
 * refused on screen without ever leaving this window.
 */
class BitLockerRecoveryKeyDialog : public QDialog {
    Q_OBJECT

public:
    explicit BitLockerRecoveryKeyDialog(const QString& volume_display_name,
                                        QWidget* parent = nullptr);

    [[nodiscard]] QString recoveryKey() const { return m_key; }

    /// @brief Empty when @p typed is a usable key; otherwise the reason, which never repeats
    ///        the digits. Public so the unit tests prove the refusal without showing a dialog.
    [[nodiscard]] static QString keyProblem(const QString& typed);

private Q_SLOTS:
    void onAccept();

private:
    QLineEdit* m_keyEdit{nullptr};
    QLabel* m_errorLabel{nullptr};
    QString m_key;
};

/**
 * @brief Wizard step 3 (offline mode only): the profiles found on another Windows installation.
 *
 * Volumes are listed with what the scan could and could not read -- an unreadable volume is a
 * row that says WHY, never an empty list (fail closed). Profiles under the chosen volume are
 * listed with a size estimate and, for a folder v1 cannot rescue, the skip reason beside it.
 *
 * Every disk, elevation and process touch point is a seam the unit tests replace, so none of
 * these decisions can only be proved against a real drive.
 */
class UserProfileBackupOfflineProfilesPage : public QWizardPage {
    Q_OBJECT

public:
    using VolumeScanner = std::function<OfflineSourceDiscovery(bool is_elevated)>;
    using VolumeReScanner =
        std::function<OfflineVolumeSource(const QString& volume_name, bool is_elevated)>;
    using VolumePreparer =
        std::function<std::expected<OfflineVolumePrep, QString>(const QString& wanted_volume)>;
    using UnlockRequest = std::function<std::expected<QString, QString>(const QString& volume_name,
                                                                        const QString& typed_key)>;
    using ElevationProbe = std::function<bool()>;
    /// @brief How the technician types a recovery key: the dialog by default. Returns the key as
    ///        typed, or nothing when the prompt was canceled. A seam so an unlock is testable
    ///        without a modal window.
    using KeyPrompt =
        std::function<std::optional<QString>(const QString& volume_display_name, QWidget* parent)>;

    explicit UserProfileBackupOfflineProfilesPage(QVector<UserProfile>& users,
                                                  QWidget* parent = nullptr);

    void initializePage() override;
    /// @brief Going back releases the mount this page created: an offline mount outliving the
    ///        step that made it is damage on the technician's PC, not a leftover to ignore.
    void cleanupPage() override;
    bool isComplete() const override;
    /// @brief Prepares the chosen volume (mounting it when it has no drive letter) and writes
    ///        the checked profiles into the wizard's user list. A refusal keeps the page open.
    bool validatePage() override;
    /// @brief The customize step: this PC's installed apps, Wi-Fi and adapter pages describe the
    ///        machine running S.A.K., not the dead one, so the offline run never visits them.
    int nextId() const override;

    /// @brief The name to stamp on the manifest: the offline machine's computer name, or its
    ///        volume label when the hive could not be read. Empty before a volume is prepared.
    [[nodiscard]] QString sourceComputerName() const;

    /// @brief The full replacement set for every disk, elevation and process touch point.
    ///        Bundled as one value so the entry point is "these are the seams", not six
    ///        positional arguments a test must get right in order.
    struct TestSeams {
        VolumeScanner scanner;
        VolumeReScanner rescan;
        VolumePreparer preparer;
        UnlockRequest unlock;
        KeyPrompt key_prompt;
        ElevationProbe elevated;
    };

#ifdef SAK_GUI_TEST_HOOKS
    /// Test-only: replace every seam. Compiled only into the tests that drive this page
    /// (tests/CMakeLists.txt, SAK_GUI_OFFLINE_SOURCE_TEST_HOOK_SOURCES), so the shipped binary
    /// carries no hook (owner ruling 2026-10-24, scripts/test_release_binary_test_hooks.ps1).
    /// It is a member function, not a member, so hooked and product objects agree on the class
    /// layout; the seam fields below always exist and hold the real operations in the product.
    void setSeamsForTesting(TestSeams seams);
#endif

    /// @brief The rows the page currently offers for rescue (included profiles only). Test and
    ///        reporting seam: it says what the technician can pick, not what was scanned.
    [[nodiscard]] QStringList rescuableFolders() const;

    /// @brief The volume this page prepared for the run (its paths are what m_users point at).
    ///        Empty until Next is pressed with a volume chosen.
    [[nodiscard]] const std::optional<OfflineVolumeSource>& preparedSource() const {
        return m_prepared;
    }

    /// @brief What the page tells the technician right now, and the two tables it drew. Read-only
    ///        accessors so a caller (test or reporting) can see a decision without clicking. The
    ///        tables themselves are returned non-const because a caller drives them the way a
    ///        technician does: selecting a row is how a test reaches a decision.
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QTableWidget* volumeTable() { return m_volumeTable; }
    [[nodiscard]] QTableWidget* profileTable() { return m_profileTable; }
    /// @brief Whether the Unlock button stands ready: only a volume the scan could not read at
    ///        all, can name by GUID path, and that is not merely an empty drive, offers one.
    [[nodiscard]] bool unlockOffered() const;
    /// @brief True while a scan, prepare or unlock runs on its worker thread. The page's
    ///        controls are disabled and Next refuses until it finishes.
    [[nodiscard]] bool isBusy() const { return m_busy; }

private Q_SLOTS:
    void onScanClicked();
    void onUnlockClicked();
    void onVolumeRowChanged();
    void onProfileCheckChanged();

private:
    void setupUi();
    [[nodiscard]] QTableWidget* buildProfileTableWidget();
    void populateVolumeTable();
    void populateProfileTable();
    /// @brief The line the page shows for a selected volume: why it could not be read, why the
    ///        registry facts are missing, or what to do next. One chain so the table builder and
    ///        the sentence are decided in one place.
    [[nodiscard]] QString sourceStatusText(const OfflineVolumeSource& source) const;
    void rebuildUsersFromChecks();
    void updateSummary();
    /// @brief Busy state for work on the worker thread: controls off, busy cursor, status text.
    void setBusy(bool busy, const QString& status);
    /// @brief Draws what a finished scan found, or says why it could not run.
    void showScanResult(const OfflineSourceDiscovery& found);
    /// @brief Prepares @p chosen off the GUI thread; on success adopts it and presses Next.
    void startPrepare(const OfflineVolumeSource& chosen);
    /// @brief Takes a prepared source in: its mount and profiles replace the previous ones, and
    ///        the previous mount is released only after nothing references paths under it.
    void adoptPrepared(OfflineVolumePrep& prepared);
    /// @brief Puts the re-read @p refreshed in @p row after @p target unlocked.
    void showUnlocked(int row, const OfflineVolumeSource& target, OfflineVolumeSource refreshed);
    void sayNoProfiles(const QString& reason);
    [[nodiscard]] int selectedVolumeRow() const;
    /// @brief Case-insensitive, because Windows folder names are: the checks key on the folder
    ///        name the scan reported, and the prepared source may spell it differently.
    [[nodiscard]] bool isFolderChecked(const QString& folder_name) const;

    QVector<UserProfile>& m_users;
    QVector<OfflineVolumeSource> m_volumes;
    /// @brief The volume this page prepared for the run: its paths are what m_users point at,
    ///        and its mount is what cleanupPage takes back down.
    std::optional<OfflineVolumeSource> m_prepared;
    std::optional<OfflineVolumeMount> m_mount;
    bool m_busy{false};
    /// @brief Bumped by cleanupPage: work that finishes after the technician went back belongs
    ///        to an abandoned visit, so its result (and any mount in it) is dropped, not adopted.
    quint64 m_visit{0};
    QSet<QString> m_checked;  // folder names, case-insensitive compare at use

    QTableWidget* m_volumeTable{nullptr};
    QTableWidget* m_profileTable{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QPushButton* m_scanButton{nullptr};
    QPushButton* m_unlockButton{nullptr};

    VolumeScanner m_scanner;
    VolumeReScanner m_rescan;
    VolumePreparer m_preparer;
    UnlockRequest m_unlock;
    KeyPrompt m_keyPrompt;
    ElevationProbe m_elevated;
};

}  // namespace sak
