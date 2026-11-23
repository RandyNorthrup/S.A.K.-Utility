// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/offline_profile_source_page.h"

#include "sak/elevation_manager.h"
#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/style_constants.h"
#include "sak/user_profile_backup_wizard.h"
#include "sak/widget_helpers.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QRadioButton>
#include <QVBoxLayout>

#include <algorithm>

namespace sak {

namespace {

// Column layouts and the one sentinel number this page needs, named rather than inline.
enum VolumeColumn {
    VolumeColName = 0,
    VolumeColComputer,
    VolumeColStatus,
    VolumeColCount,
};

enum ProfileColumn {
    ProfileColSelect = 0,
    ProfileColFolder,
    ProfileColPath,
    ProfileColSize,
    ProfileColNote,
    ProfileColCount,
};

constexpr qint64 kUnknownSizeSentinel = -1;

/// @brief The volume row's third column: a count, or WHY there is none. An unreadable volume
///        says why it could not be read and never reads "0 profiles" -- an empty success is the
///        failure the whole offline seam exists to prevent.
QString volumeStatusText(const OfflineVolumeSource& source) {
    if (!source.readable) {
        return source.unread_reason.isEmpty() ? QObject::tr("Could not be read")
                                              : source.unread_reason;
    }
    if (!source.windows_found) {
        return QObject::tr("No Windows installation found");
    }
    const int rescuable = static_cast<int>(std::ranges::count_if(
        source.profiles, [](const OfflineProfileEntry& entry) { return entry.included; }));
    return QObject::tr("%1 of %2 folders rescuable").arg(rescuable).arg(source.profiles.size());
}

/// @brief The size column. A walk that hit its cap says Unknown rather than printing a number
///        that looks exact; a genuinely empty profile says Empty rather than 0 bytes.
QString profileSizeText(qint64 estimated_bytes) {
    if (estimated_bytes == kUnknownSizeSentinel) {
        return QObject::tr("Unknown");
    }
    if (estimated_bytes == 0) {
        return QObject::tr("Empty");
    }
    return formatBytes(estimated_bytes);
}

/// @brief The default recovery-key prompt: the dialog, modal to the page that asked.
std::optional<QString> promptForKey(const QString& volume_display_name, QWidget* parent) {
    BitLockerRecoveryKeyDialog dialog(volume_display_name, parent);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return dialog.recoveryKey();
}

}  // namespace

// ============================================================================
// UserProfileBackupSourcePage
// ============================================================================

UserProfileBackupSourcePage::UserProfileBackupSourcePage(QWidget* parent) : QWizardPage(parent) {
    setTitle(tr("Choose the Windows Installation to Back Up"));
    setSubTitle(tr("This computer, or another Windows installation on a disk attached to it"));

    setupUi();
}

void UserProfileBackupSourcePage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    auto* instruction = new QLabel(
        tr("<h3>What do you want to back up?</h3>"
           "<ul>"
           "<li><b>This PC</b> - the Windows accounts on the computer running S.A.K. Utility. "
           "This is the normal backup.</li>"
           "<li><b>Another Windows installation</b> - the profiles on a disk from a computer "
           "that is not running: a drive from a dead PC, or a second installation on this "
           "machine. S.A.K. reads that disk as data; it does not start the Windows on it.</li>"
           "</ul>"
           "<p>Rescuing another installation needs S.A.K. Utility running as administrator, "
           "and gives files and settings only: a registry hive is never copied from a machine "
           "that is not running.</p>"),
        this);
    instruction->setWordWrap(true);
    layout->addWidget(instruction);

    m_thisPcRadio = new QRadioButton(tr("This PC"), this);
    m_thisPcRadio->setAccessibleName(tr("Back up this PC"));
    m_thisPcRadio->setChecked(true);  // today's behaviour stays the default
    layout->addWidget(m_thisPcRadio);

    m_otherRadio = new QRadioButton(tr("Another Windows installation"), this);
    m_otherRadio->setAccessibleName(tr("Back up another Windows installation on an attached disk"));
    layout->addWidget(m_otherRadio);

    auto* note = new QLabel(
        tr("A BitLocker-encrypted disk can be unlocked on the next page with its recovery key. "
           "The key is used once and never stored."),
        this);
    note->setWordWrap(true);
    note->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    layout->addWidget(note);

    layout->addStretch();

    // The radios decide where Next goes, so a change re-runs the page's completeness.
    connect(m_thisPcRadio, &QRadioButton::toggled, this, [this]() { Q_EMIT completeChanged(); });
    connect(m_otherRadio, &QRadioButton::toggled, this, [this]() { Q_EMIT completeChanged(); });
}

UserProfileBackupSourcePage::Source UserProfileBackupSourcePage::selectedSource() const {
    return (m_otherRadio != nullptr && m_otherRadio->isChecked()) ? SourceOtherWindows
                                                                  : SourceThisPc;
}

void UserProfileBackupSourcePage::setSourceSelected(Source source) {
    Q_ASSERT(m_thisPcRadio != nullptr);
    Q_ASSERT(m_otherRadio != nullptr);
    m_otherRadio->setChecked(source == SourceOtherWindows);
    m_thisPcRadio->setChecked(source == SourceThisPc);
    Q_EMIT completeChanged();
}

int UserProfileBackupSourcePage::nextId() const {
    const auto* wiz = qobject_cast<const UserProfileBackupWizard*>(wizard());
    if (wiz == nullptr) {
        return -1;  // Without its wizard there is nowhere to route; finish rather than guess.
    }
    return selectedSource() == SourceOtherWindows ? UserProfileBackupWizard::Page_OfflineProfiles
                                                  : UserProfileBackupWizard::Page_SelectUsers;
}

// ============================================================================
// BitLockerRecoveryKeyDialog
// ============================================================================

QString BitLockerRecoveryKeyDialog::keyProblem(const QString& typed) {
    const auto normalized = OfflineProfileSource::normalizeRecoveryKey(typed);
    return normalized ? QString() : normalized.error();
}

BitLockerRecoveryKeyDialog::BitLockerRecoveryKeyDialog(const QString& volume_display_name,
                                                       QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Unlock Encrypted Drive"));
    auto* layout = new QVBoxLayout(this);

    auto* explanation = new QLabel(
        tr("<p>Enter the BitLocker recovery key for <b>%1</b>.</p>"
           "<p>The key is 48 digits, shown in 8 groups of 6 when the disk was encrypted. S.A.K. "
           "uses it once to unlock the drive, never stores it, and never writes it to the "
           "log.</p>")
            .arg(volume_display_name.toHtmlEscaped()),
        this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    m_keyEdit = new QLineEdit(this);
    m_keyEdit->setEchoMode(QLineEdit::Password);
    m_keyEdit->setAccessibleName(tr("BitLocker recovery key"));
    m_keyEdit->setPlaceholderText(tr("6 digits, 8 groups"));
    layout->addWidget(m_keyEdit);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setObjectName(QStringLiteral("bitlockerKeyError"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorError));
    layout->addWidget(m_errorLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &BitLockerRecoveryKeyDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void BitLockerRecoveryKeyDialog::onAccept() {
    Q_ASSERT(m_keyEdit);
    Q_ASSERT(m_errorLabel);
    const QString typed = m_keyEdit->text();
    const QString problem = keyProblem(typed);
    if (!problem.isEmpty()) {
        // Said on the spot so a key of the wrong shape never reaches the elevation helper, and
        // the reason never repeats the digits that were typed.
        m_errorLabel->setText(problem);
        m_keyEdit->setFocus();
        return;
    }
    m_key = typed;
    accept();
}

// ============================================================================
// UserProfileBackupOfflineProfilesPage
// ============================================================================

UserProfileBackupOfflineProfilesPage::UserProfileBackupOfflineProfilesPage(
    QVector<UserProfile>& users, QWidget* parent)
    : QWizardPage(parent)
    , m_users(users)
    , m_scanner([](bool elevated) { return OfflineProfileSource::discover(elevated); })
    , m_rescan([](const QString& volume_name, bool elevated) {
        return OfflineProfileSource::discoverVolume(volume_name, elevated);
    })
    , m_preparer([](const QString& wanted) {
        return OfflineProfileSource::prepareVolume(wanted,
                                                   QStringLiteral("profile rescue"),
                                                   ElevationManager::isElevated());
    })
    , m_unlock([](const QString& volume_name, const QString& typed_key) {
        return OfflineProfileSource::unlockEncryptedVolume(
            volume_name, typed_key, OfflineProfileSource::sessionScriptRunner());
    })
    , m_keyPrompt(promptForKey)
    , m_elevated([] { return ElevationManager::isElevated(); }) {
    setTitle(tr("Profiles on Another Windows Installation"));
    setSubTitle(tr("S.A.K. reads that disk as data; it does not start the Windows on it"));

    setupUi();
}

void UserProfileBackupOfflineProfilesPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    m_statusLabel = new QLabel(tr("Click Rescan to look for other Windows installations."), this);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    auto* controls = new QHBoxLayout();
    m_scanButton = new QPushButton(tr("Rescan"), this);
    m_scanButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(m_scanButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupOfflineProfilesPage::onScanClicked);
    controls->addWidget(m_scanButton);

    m_unlockButton = new QPushButton(tr("Unlock with Recovery Key..."), this);
    m_unlockButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_unlockButton->setEnabled(false);
    // The audit rule (accessible_name_rule.h) is that the name must HOLD the words on the button:
    // someone using voice control says what they see. The extra words tell this Unlock apart from
    // any other.
    m_unlockButton->setAccessibleName(
        tr("Unlock with Recovery Key for the selected encrypted drive"));
    connect(m_unlockButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupOfflineProfilesPage::onUnlockClicked);
    controls->addWidget(m_unlockButton);
    controls->addStretch();
    layout->addLayout(controls);

    m_volumeTable = new QTableWidget(0, VolumeColCount, this);
    m_volumeTable->setAccessibleName(tr("Offline Windows installations found"));
    m_volumeTable->setHorizontalHeaderLabels(
        {tr("Drive"), tr("Computer"), tr("What S.A.K. found")});
    m_volumeTable->horizontalHeader()->setSectionResizeMode(VolumeColName,
                                                            QHeaderView::ResizeToContents);
    m_volumeTable->horizontalHeader()->setSectionResizeMode(VolumeColComputer,
                                                            QHeaderView::ResizeToContents);
    m_volumeTable->horizontalHeader()->setSectionResizeMode(VolumeColStatus, QHeaderView::Stretch);
    m_volumeTable->verticalHeader()->setVisible(false);
    m_volumeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_volumeTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_volumeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_volumeTable->setMaximumHeight(sak::kLogAreaSmallMaxH);
    connect(m_volumeTable,
            &QTableWidget::itemSelectionChanged,
            this,
            &UserProfileBackupOfflineProfilesPage::onVolumeRowChanged);
    layout->addWidget(m_volumeTable);

    m_profileTable = buildProfileTableWidget();
    layout->addWidget(m_profileTable);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);
    updateSummary();
}

/// @brief The profile table: a checkable box only where the technician's decision is real, and
///        a "Why not" column beside every folder that cannot go in.
QTableWidget* UserProfileBackupOfflineProfilesPage::buildProfileTableWidget() {
    auto* table = new QTableWidget(0, ProfileColCount, this);
    table->setAccessibleName(tr("Profiles to rescue"));
    table->setHorizontalHeaderLabels(
        {tr("Rescue"), tr("Folder"), tr("Path"), tr("Est. Size"), tr("Why not")});
    table->horizontalHeader()->setSectionResizeMode(ProfileColSelect,
                                                    QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(ProfileColFolder,
                                                    QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(ProfileColPath, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(ProfileColSize, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(ProfileColNote, QHeaderView::ResizeToContents);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(table,
            &QTableWidget::itemChanged,
            this,
            &UserProfileBackupOfflineProfilesPage::onProfileCheckChanged);
    return table;
}

#ifdef SAK_GUI_TEST_HOOKS
// Compiled only into the test that drives this page (see the header and tests/CMakeLists.txt);
// the shipped sak_gui never sees a definition, so its binary carries no ForTesting symbol.
void UserProfileBackupOfflineProfilesPage::setSeamsForTesting(TestSeams seams) {
    m_scanner = std::move(seams.scanner);
    m_rescan = std::move(seams.rescan);
    m_preparer = std::move(seams.preparer);
    m_unlock = std::move(seams.unlock);
    m_keyPrompt = std::move(seams.key_prompt);
    m_elevated = std::move(seams.elevated);
}
#endif

void UserProfileBackupOfflineProfilesPage::initializePage() {
    // The scan walks every volume on the machine, so it runs once per visit and on an explicit
    // Rescan -- not on every click that lands back here.
    if (m_volumes.isEmpty()) {
        onScanClicked();
    } else {
        updateSummary();
    }
}

void UserProfileBackupOfflineProfilesPage::cleanupPage() {
    QWizardPage::cleanupPage();
    // Leaving this page backward abandons the prepared source, so the mount that came with it
    // goes with it. A release that could not be proven is reported: the technician needs to know
    // the mount point is still on their PC.
    if (m_mount.has_value()) {
        const QString mount_directory = m_mount->directory();
        const auto released = m_mount->release();
        if (!released) {
            sak::logWarning("The offline mount '{}' could not be removed: {}",
                            mount_directory.toStdString(),
                            released.error().toStdString());
        }
        m_mount.reset();
    }
    m_prepared.reset();
    m_checked.clear();
    m_users.clear();
    m_volumes.clear();
    if (m_profileTable != nullptr) {
        m_profileTable->setRowCount(0);
    }
    if (m_volumeTable != nullptr) {
        m_volumeTable->setRowCount(0);
    }
    Q_EMIT completeChanged();
}

QString UserProfileBackupOfflineProfilesPage::statusText() const {
    return m_statusLabel == nullptr ? QString() : m_statusLabel->text();
}

bool UserProfileBackupOfflineProfilesPage::unlockOffered() const {
    return m_unlockButton != nullptr && m_unlockButton->isEnabled();
}

int UserProfileBackupOfflineProfilesPage::selectedVolumeRow() const {
    return m_volumeTable == nullptr ? -1 : m_volumeTable->currentRow();
}

bool UserProfileBackupOfflineProfilesPage::isFolderChecked(const QString& folder_name) const {
    for (const QString& checked : m_checked) {
        if (checked.compare(folder_name, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QStringList UserProfileBackupOfflineProfilesPage::rescuableFolders() const {
    QStringList folders;
    const int row = selectedVolumeRow();
    if (row < 0 || row >= m_volumes.size()) {
        return folders;
    }
    for (const OfflineProfileEntry& entry : m_volumes[row].profiles) {
        if (entry.included) {
            folders.append(entry.folder_name);
        }
    }
    return folders;
}

QString UserProfileBackupOfflineProfilesPage::sourceComputerName() const {
    if (!m_prepared.has_value()) {
        return {};
    }
    // An offline machine's name comes from its SYSTEM hive; when that could not be read, the
    // volume label is what a technician can still recognize. Never a guess.
    return m_prepared->computer_name.isEmpty() ? m_prepared->display_name
                                               : m_prepared->computer_name;
}

void UserProfileBackupOfflineProfilesPage::sayNoProfiles(const QString& reason) {
    Q_ASSERT(m_statusLabel);
    m_statusLabel->setText(reason);
    sak::logWarning("Offline profile scan refused: {}", reason.toStdString());
    if (m_volumeTable != nullptr) {
        m_volumeTable->setRowCount(0);
    }
    if (m_profileTable != nullptr) {
        m_profileTable->setRowCount(0);
    }
    m_volumes.clear();
    m_checked.clear();
    updateSummary();
    Q_EMIT completeChanged();
}

void UserProfileBackupOfflineProfilesPage::onScanClicked() {
    Q_ASSERT(m_statusLabel);
    m_statusLabel->setText(tr("Looking for Windows installations on other volumes..."));
    const OfflineSourceDiscovery found = m_scanner(m_elevated());
    if (!found.enumeration_ok) {
        // The standard "run as administrator" refusal arrives through here too, in its own words.
        sayNoProfiles(found.error.isEmpty() ? tr("The offline scan could not run.") : found.error);
        return;
    }
    m_volumes = found.volumes;
    populateVolumeTable();
    if (m_volumes.isEmpty()) {
        m_statusLabel->setText(
            tr("No other Windows installation was found. The disk may not be attached, or Windows "
               "may not be installed on it."));
    } else {
        populateProfileTable();
        m_statusLabel->setText(tr("Found %1. Pick one to list its profiles.")
                                   .arg(countOf(m_volumes.size(),
                                                tr("Windows installation"),
                                                tr("Windows installations"))));
    }
    Q_EMIT completeChanged();
}

void UserProfileBackupOfflineProfilesPage::populateVolumeTable() {
    Q_ASSERT(m_volumeTable);
    m_volumeTable->blockSignals(true);
    m_volumeTable->setRowCount(static_cast<int>(m_volumes.size()));
    for (int row = 0; row < m_volumes.size(); ++row) {
        const OfflineVolumeSource& source = m_volumes[row];
        m_volumeTable->setItem(row,
                               VolumeColName,
                               new QTableWidgetItem(source.display_name.isEmpty()
                                                        ? tr("(unnamed volume)")
                                                        : source.display_name));
        m_volumeTable->setItem(row,
                               VolumeColComputer,
                               new QTableWidgetItem(source.computer_name.isEmpty()
                                                        ? tr("Unknown")
                                                        : source.computer_name));
        m_volumeTable->setItem(row,
                               VolumeColStatus,
                               new QTableWidgetItem(volumeStatusText(source)));
    }
    m_volumeTable->blockSignals(false);
    if (!m_volumes.isEmpty()) {
        m_volumeTable->selectRow(0);
    }
}

void UserProfileBackupOfflineProfilesPage::onVolumeRowChanged() {
    populateProfileTable();
    Q_EMIT completeChanged();
}

void UserProfileBackupOfflineProfilesPage::populateProfileTable() {
    Q_ASSERT(m_profileTable);
    Q_ASSERT(m_statusLabel);
    Q_ASSERT(m_unlockButton);
    const int row = selectedVolumeRow();
    if (row < 0 || row >= m_volumes.size()) {
        m_profileTable->setRowCount(0);
        m_unlockButton->setEnabled(false);
        updateSummary();
        return;
    }
    const OfflineVolumeSource& source = m_volumes[row];
    // Unlock only makes sense for a volume S.A.K. could not read at all and can name by GUID:
    // Unlock-BitLocker takes a mount point, and a GUID path is the only one it has here.
    m_unlockButton->setEnabled(!source.readable &&
                               OfflineProfileSource::isVolumeGuidPath(source.volume_name));

    m_profileTable->blockSignals(true);
    m_profileTable->setRowCount(static_cast<int>(source.profiles.size()));
    m_checked.clear();
    for (int index = 0; index < source.profiles.size(); ++index) {
        const OfflineProfileEntry& entry = source.profiles[index];
        auto* check_item = new QTableWidgetItem();
        if (entry.included) {
            check_item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            check_item->setCheckState(Qt::Checked);  // a technician rescues; unchecking is the edit
            m_checked.insert(entry.folder_name);
        } else {
            // Not a box left unticked: this folder CANNOT go in, and the reason stands beside it.
            check_item->setFlags(Qt::ItemIsEnabled);
            check_item->setCheckState(Qt::Unchecked);
        }
        m_profileTable->setItem(index, ProfileColSelect, check_item);
        m_profileTable->setItem(index, ProfileColFolder, new QTableWidgetItem(entry.folder_name));
        m_profileTable->setItem(index, ProfileColPath, new QTableWidgetItem(entry.profile_path));
        auto* size_item = new QTableWidgetItem(profileSizeText(entry.estimated_size_bytes));
        size_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_profileTable->setItem(index, ProfileColSize, size_item);
        m_profileTable->setItem(index,
                                ProfileColNote,
                                new QTableWidgetItem(
                                    entry.skip_reason.isEmpty() ? QString() : entry.skip_reason));
    }
    m_profileTable->blockSignals(false);

    m_statusLabel->setText(sourceStatusText(source));
    rebuildUsersFromChecks();
    updateSummary();
}

/// @brief The sentence under the tables, chosen by what the scan could and could not do. Each
///        branch names the NEXT thing the technician can do: read the reason, pick profiles, or
///        accept that this volume holds nothing to rescue.
QString UserProfileBackupOfflineProfilesPage::sourceStatusText(
    const OfflineVolumeSource& source) const {
    if (!source.readable) {
        return source.unread_reason.isEmpty() ? tr("This volume could not be read.")
                                              : source.unread_reason;
    }
    if (!source.hive_error.isEmpty()) {
        return tr("Registry facts could not be read (%1). Profiles are listed by "
                  "folder name only, with no account SID.")
            .arg(source.hive_error);
    }
    if (source.profiles.isEmpty()) {
        return tr("That volume holds a Windows installation but no user profiles to rescue.");
    }
    return tr("Pick the profiles to rescue.");
}

void UserProfileBackupOfflineProfilesPage::onProfileCheckChanged() {
    Q_ASSERT(m_profileTable);
    const int row = selectedVolumeRow();
    if (row < 0 || row >= m_volumes.size()) {
        return;
    }
    const QVector<OfflineProfileEntry>& profiles = m_volumes[row].profiles;
    for (int index = 0; index < m_profileTable->rowCount() && index < profiles.size(); ++index) {
        QTableWidgetItem* check_item = m_profileTable->item(index, ProfileColSelect);
        if (check_item == nullptr || !profiles[index].included) {
            continue;  // a folder that cannot be rescued has no box and no decision to read
        }
        if (check_item->checkState() == Qt::Checked) {
            m_checked.insert(profiles[index].folder_name);
        } else {
            m_checked.remove(profiles[index].folder_name);
        }
    }
    rebuildUsersFromChecks();
    updateSummary();
    Q_EMIT completeChanged();
}

void UserProfileBackupOfflineProfilesPage::rebuildUsersFromChecks() {
    const int row = selectedVolumeRow();
    if (row < 0 || row >= m_volumes.size()) {
        m_users.clear();
        return;
    }
    // Once a volume is prepared its paths are the mounted ones; until then they are what the scan
    // reported. The folder names are the same list either way, which is what the checks key on.
    const QVector<OfflineProfileEntry>& profiles = m_prepared.has_value() ? m_prepared->profiles
                                                                          : m_volumes[row].profiles;
    m_users.clear();
    for (const OfflineProfileEntry& entry : profiles) {
        if (!entry.included) {
            continue;
        }
        UserProfile user = OfflineProfileSource::toUserProfile(entry);
        user.is_selected = isFolderChecked(entry.folder_name);
        m_users.append(user);
    }
}

void UserProfileBackupOfflineProfilesPage::updateSummary() {
    Q_ASSERT(m_summaryLabel);
    int selected_count = 0;
    qint64 total_bytes = 0;
    for (const UserProfile& user : m_users) {
        if (user.is_selected) {
            ++selected_count;
            total_bytes += user.total_size_estimated;
        }
    }
    if (selected_count == 0) {
        m_summaryLabel->setText(tr("No profiles selected."));
        return;
    }
    const QString size_text = total_bytes > 0 ? formatBytes(total_bytes) : tr("unknown size");
    m_summaryLabel->setText(tr("%1 selected | about %2")
                                .arg(countOf(selected_count, tr("profile"), tr("profiles")))
                                .arg(size_text));
}

bool UserProfileBackupOfflineProfilesPage::isComplete() const {
    return std::ranges::any_of(m_users, [](const UserProfile& user) { return user.is_selected; });
}

bool UserProfileBackupOfflineProfilesPage::validatePage() {
    Q_ASSERT(m_statusLabel);
    const int row = selectedVolumeRow();
    if (row < 0 || row >= m_volumes.size()) {
        m_statusLabel->setText(tr("Pick the Windows installation to rescue from."));
        return false;
    }
    const OfflineVolumeSource chosen = m_volumes[row];
    if (!chosen.readable) {
        m_statusLabel->setText(tr("%1 is still not readable. Unlock it with its recovery key, or "
                                  "go back and choose another source.")
                                   .arg(chosen.display_name));
        return false;
    }
    auto prepared = m_preparer(chosen.volume_name);
    if (!prepared) {
        m_statusLabel->setText(prepared.error());
        sak::logWarning("Offline source preparation refused: {}", prepared.error().toStdString());
        return false;
    }
    // A second press of Next (after a later refusal) replaces the source, so the mount the
    // previous one carried comes down first and its failure is reported rather than dropped.
    if (m_mount.has_value()) {
        const QString previous_directory = m_mount->directory();
        const auto released_previous = m_mount->release();
        if (!released_previous) {
            sak::logWarning("The offline mount '{}' could not be removed: {}",
                            previous_directory.toStdString(),
                            released_previous.error().toStdString());
        }
        m_mount.reset();
    }
    if (prepared->mount.has_value()) {
        // emplace, not operator=: OfflineVolumeMount is move-constructible only, and m_mount is
        // empty here because the block above took the previous one down.
        m_mount.emplace(std::move(*prepared->mount));
    }
    m_prepared = prepared->source;
    rebuildUsersFromChecks();
    if (!isComplete()) {
        m_statusLabel->setText(tr("Select at least one profile to rescue."));
        return false;
    }
    return true;
}

void UserProfileBackupOfflineProfilesPage::onUnlockClicked() {
    Q_ASSERT(m_statusLabel);
    Q_ASSERT(m_volumeTable);
    const int row = selectedVolumeRow();
    if (row < 0 || row >= m_volumes.size()) {
        m_statusLabel->setText(tr("Pick the encrypted drive to unlock first."));
        return;
    }
    const OfflineVolumeSource target = m_volumes[row];
    const std::optional<QString> typed = m_keyPrompt(target.display_name, this);
    if (!typed.has_value()) {
        m_statusLabel->setText(tr("Unlock cancelled. Nothing was changed on the disk."));
        return;
    }
    const auto unlocked = m_unlock(target.volume_name, *typed);
    if (!unlocked) {
        // The reason is Windows' own and never carries the key. A wrong key is a fact the
        // technician needs, not a generic failure.
        m_statusLabel->setText(
            tr("%1 did not unlock: %2").arg(target.display_name, unlocked.error()));
        return;
    }
    OfflineVolumeSource refreshed = m_rescan(target.volume_name, m_elevated());
    if (refreshed.volume_name.isEmpty()) {
        refreshed.volume_name = target.volume_name;  // the row must still name what it is
    }
    m_volumes[row] = refreshed;
    populateVolumeTable();
    m_volumeTable->selectRow(row);
    populateProfileTable();
    m_statusLabel->setText(
        refreshed.readable
            ? tr("%1 unlocked. Its profiles are listed below.").arg(target.display_name)
            : refreshed.unread_reason);
    Q_EMIT completeChanged();
}

int UserProfileBackupOfflineProfilesPage::nextId() const {
    const auto* wiz = qobject_cast<const UserProfileBackupWizard*>(wizard());
    if (wiz == nullptr) {
        return -1;
    }
    // The this-PC pages describe the machine running S.A.K., so an offline run goes straight to
    // per-user customization and skips installed apps, application data, Wi-Fi and adapters.
    return UserProfileBackupWizard::Page_CustomizeData;
}

}  // namespace sak
