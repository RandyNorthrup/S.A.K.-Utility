// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_properties_dialog.cpp
/// @brief Files-style Properties window for the File Management Explorer.

#include "sak/file_explorer_properties_dialog.h"

#include "sak/file_explorer_item_model.h"
#include "sak/layout_constants.h"
#include "sak/widget_helpers.h"

#include <QCheckBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTabWidget>
#include <QtConcurrent>
#include <QVariant>
#include <QVBoxLayout>

#include <atomic>
#include <memory>

#include <zlib.h>

// The cooperative cancel flag is stashed on the dialog as a dynamic property so
// the destructor can reach it without a header member; a shared_ptr keeps the
// flag alive for the worker even if the dialog is torn down first.
Q_DECLARE_METATYPE(std::shared_ptr<std::atomic_bool>)

namespace sak {

namespace {

// Per-file raw read window for hashing, matching the explorer's on-demand
// hash cap (a larger raw file reports a capped digest).
constexpr uint64_t kPropertiesHashMaxBytes = 512ULL * 1024 * 1024;
// Tree-size walk bounds, shared spirit with the bridge directory walks.
constexpr int kSizeWalkMaxDepth = 32;
constexpr int kSizeWalkMaxEntriesPerDirectory = 10'000;

QString crc32Hex(const QByteArray& data) {
    const auto value = static_cast<quint32>(crc32(
        0L, reinterpret_cast<const Bytef*>(data.constData()), static_cast<uInt>(data.size())));
    return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0'));
}

// True once the shared cancel flag has been set (dialog closing). Cheap wrapper
// so the size/hash workers can bail cooperatively.
bool scanCancelled(const std::shared_ptr<const std::atomic_bool>& cancel) {
    return cancel && cancel->load();
}

const char* const kScanCancelProperty = "sakScanCancelFlag";

// Fetch (creating on first use) the dialog's shared cancel flag. Stored as a
// dynamic property so the destructor can flip it without a header member.
std::shared_ptr<std::atomic_bool> scanCancelFlag(QObject* owner) {
    auto flag = owner->property(kScanCancelProperty).value<std::shared_ptr<std::atomic_bool>>();
    if (!flag) {
        flag = std::make_shared<std::atomic_bool>(false);
        owner->setProperty(kScanCancelProperty, QVariant::fromValue(flag));
    }
    return flag;
}

// Compute the enabled digests over one bridge read of the file. Reads are capped
// at kPropertiesHashMaxBytes, so a larger file is hashed from a prefix only; those
// digests are marked so they are never taken for the whole-file hash. A set cancel
// flag short-circuits before the read and between digests so a closing dialog is
// not held hostage by a 512 MB hash.
QMap<QString, QString> computeHashes(const FileManagementTarget& target,
                                     const QString& path,
                                     const QStringList& algorithms,
                                     const quint64 fullSize,
                                     const std::shared_ptr<const std::atomic_bool>& cancel) {
    QMap<QString, QString> hashes;
    if (scanCancelled(cancel)) {
        return hashes;
    }
    const FileManagementReadResult read =
        FileManagementFileSystemBridge::readFile(target, path, kPropertiesHashMaxBytes);
    if (!read.ok) {
        for (const QString& algorithm : algorithms) {
            hashes.insert(algorithm, QStringLiteral("(read failed)"));
        }
        return hashes;
    }
    const bool truncated = hashInputTruncated(fullSize, kPropertiesHashMaxBytes);
    static const QMap<QString, QCryptographicHash::Algorithm> kQtAlgorithms = {
        {QStringLiteral("MD5"), QCryptographicHash::Md5},
        {QStringLiteral("SHA-1"), QCryptographicHash::Sha1},
        {QStringLiteral("SHA-256"), QCryptographicHash::Sha256},
        {QStringLiteral("SHA-384"), QCryptographicHash::Sha384},
        {QStringLiteral("SHA-512"), QCryptographicHash::Sha512},
    };
    for (const QString& algorithm : algorithms) {
        if (scanCancelled(cancel)) {
            break;
        }
        const QString digest =
            algorithm == QStringLiteral("CRC32")
                ? crc32Hex(read.data)
                : QString::fromLatin1(
                      QCryptographicHash::hash(read.data, kQtAlgorithms.value(algorithm)).toHex());
        hashes.insert(algorithm, formatHashValue(digest, truncated, kPropertiesHashMaxBytes));
    }
    return hashes;
}

}  // namespace

// Wrap a directory lister so that, once @p cancel is set, it stops calling @p base
// and reports each directory as unlistable. treeSize then marks the total
// incomplete and stops descending, so a cancelled walk unwinds promptly instead
// of freezing the GUI in the destructor's waitForFinished(). Fail-closed: a
// cancelled size is a lower bound, never a wrong exact figure.
DirectoryLister makeCancelableLister(DirectoryLister base,
                                     std::shared_ptr<const std::atomic_bool> cancel) {
    return [base = std::move(base), cancel = std::move(cancel)](const QString& path,
                                                                const int max_entries) {
        if (scanCancelled(cancel)) {
            return FileManagementListResult{};
        }
        return base(path, max_entries);
    };
}

FileExplorerPropertiesDialog::FileExplorerPropertiesDialog(FileManagementTarget target,
                                                           QVector<FileManagementEntry> entries,
                                                           QWidget* parent)
    : QDialog(parent), m_target(std::move(target)), m_entries(std::move(entries)) {
    setObjectName(QStringLiteral("fileExplorerPropertiesDialog"));
    const QString title = m_entries.size() == 1 ? m_entries.first().name
                                                : tr("%1 items").arg(m_entries.size());
    setWindowTitle(tr("%1 Properties").arg(title));
    resize(kDialogWidthXLarge, kDialogHeightMedium);

    auto* layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("fileExplorerPropertiesTabs"));
    layout->addWidget(m_tabs, 1);
    buildGeneralPage();
    // Files PropertiesNavigationItemsFactory: the Hashes page exists for a
    // single regular file only; digests compute lazily on first tab open.
    if (m_entries.size() == 1 && m_entries.first().regular_file) {
        buildHashesPage();
        connect(&m_hash_watcher, &QFutureWatcher<QMap<QString, QString>>::finished, this, [this]() {
            applyHashes(m_hash_watcher.result());
        });
        connect(m_tabs, &QTabWidget::currentChanged, this, [this](const int index) {
            if (m_tabs->tabText(index) == tr("Hashes")) {
                startHashCalculation();
            }
        });
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    startSizeCalculation();
}

FileExplorerPropertiesDialog::~FileExplorerPropertiesDialog() {
    // QtConcurrent::run futures ignore QFuture::cancel(), so flip the shared flag
    // first: the walk's lister and the hash loop check it and bail, letting the
    // waits below return promptly instead of freezing the GUI on close.
    if (auto flag = property(kScanCancelProperty).value<std::shared_ptr<std::atomic_bool>>()) {
        flag->store(true);
    }
    m_size_watcher.cancel();
    m_hash_watcher.cancel();
    m_size_watcher.waitForFinished();
    m_hash_watcher.waitForFinished();
}

QString FileExplorerPropertiesDialog::editedName() const {
    return m_name_edit ? m_name_edit->text().trimmed() : QString();
}

QString FileExplorerPropertiesDialog::originalName() const {
    return m_entries.size() == 1 ? m_entries.first().name : QString();
}

void FileExplorerPropertiesDialog::buildGeneralPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    const bool single = m_entries.size() == 1;
    const FileManagementEntry& first = m_entries.first();

    // Files GeneralPage.xaml order: name, type, location, size, then the
    // timestamps; file system and the on-disk identifier are the S.A.K.
    // raw-evidence additions (size-on-disk and attribute toggles are backlog).
    m_name_edit = new QLineEdit(single ? first.name : tr("%1 items").arg(m_entries.size()), page);
    m_name_edit->setObjectName(QStringLiteral("fileExplorerPropertiesName"));
    m_name_edit->setReadOnly(!single);
    form->addRow(tr("Name:"), m_name_edit);

    // Everything below is read out of the mounted target -- names, paths, type and file-system
    // strings, on-disk identifiers -- and a raw ext4/HFS+/APFS image can carry any bytes it
    // likes in those fields, so every value label renders its text verbatim.
    auto* type_label = plainTextLabel(single ? first.type : tr("Multiple types"), page);
    type_label->setObjectName(QStringLiteral("fileExplorerPropertiesType"));
    form->addRow(tr("Type:"), type_label);

    auto* location_label = plainTextLabel(m_target.label, page);
    location_label->setObjectName(QStringLiteral("fileExplorerPropertiesLocation"));
    location_label->setText(single ? first.path : m_target.label);
    location_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Location:"), location_label);

    m_size_label = new QLabel(tr("Calculating..."), page);
    m_size_label->setObjectName(QStringLiteral("fileExplorerPropertiesSize"));
    form->addRow(tr("Size:"), m_size_label);

    auto* fs_label = plainTextLabel(m_target.file_system, page);
    fs_label->setObjectName(QStringLiteral("fileExplorerPropertiesFileSystem"));
    form->addRow(tr("File system:"), fs_label);

    if (single && !first.identifier.isEmpty()) {
        auto* id_label = plainTextLabel(first.identifier, page);
        id_label->setObjectName(QStringLiteral("fileExplorerPropertiesIdentifier"));
        form->addRow(FileManagementFileSystemBridge::identifierLabel(m_target.file_system) +
                         QStringLiteral(":"),
                     id_label);
    }
    if (single) {
        form->addRow(tr("Created:"),
                     new QLabel(FileExplorerItemModel::timeText(first.created_time), page));
        form->addRow(tr("Modified:"),
                     new QLabel(FileExplorerItemModel::timeText(first.modified_time), page));
        form->addRow(tr("Attributes:"),
                     plainTextLabel(FileExplorerItemModel::attributeSummary(first), page));
    }
    m_tabs->addTab(page, tr("General"));
}

void FileExplorerPropertiesDialog::buildHashesPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    // Files HashesViewModel defaults: CRC32/MD5/SHA-1/SHA-256 on, the long
    // digests opt-in; values compute only once the page is opened.
    const QStringList algorithms = {QStringLiteral("CRC32"),
                                    QStringLiteral("MD5"),
                                    QStringLiteral("SHA-1"),
                                    QStringLiteral("SHA-256"),
                                    QStringLiteral("SHA-384"),
                                    QStringLiteral("SHA-512")};
    for (const QString& algorithm : algorithms) {
        auto* check = new QCheckBox(algorithm, page);
        check->setChecked(algorithm != QStringLiteral("SHA-384") &&
                          algorithm != QStringLiteral("SHA-512"));
        auto* value = new QLabel(QString(), page);
        value->setObjectName(QStringLiteral("fileExplorerPropertiesHash-") + algorithm);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        m_hash_checks.insert(algorithm, check);
        m_hash_values.insert(algorithm, value);
        form->addRow(check, value);
        connect(check, &QCheckBox::toggled, this, [this](const bool checked) {
            if (checked) {
                m_hashes_started = false;
                startHashCalculation();
            }
        });
    }
    m_tabs->addTab(page, tr("Hashes"));
}

void FileExplorerPropertiesDialog::startSizeCalculation() {
    connect(&m_size_watcher, &QFutureWatcher<TreeSizeResult>::finished, this, [this]() {
        if (!m_size_label) {
            return;
        }
        const TreeSizeResult result = m_size_watcher.result();
        const QString text = FileExplorerItemModel::sizeText(result.bytes);
        // A truncated walk under-counts, so present the figure as a lower bound
        // rather than a wrong exact size.
        m_size_label->setText(
            result.complete ? text
                            : tr("at least %1 (some folders could not be fully read)").arg(text));
    });
    const FileManagementTarget target = m_target;
    const QVector<FileManagementEntry> entries = m_entries;
    const std::shared_ptr<const std::atomic_bool> cancel = scanCancelFlag(this);
    m_size_watcher.setFuture(QtConcurrent::run([target, entries, cancel]() {
        DirectoryLister base = [&target](const QString& path, const int max_entries) {
            return FileManagementFileSystemBridge::listDirectory(target, path, max_entries);
        };
        const DirectoryLister lister = makeCancelableLister(std::move(base), cancel);
        return combinedSize(lister, entries, kSizeWalkMaxDepth, kSizeWalkMaxEntriesPerDirectory);
    }));
}

void FileExplorerPropertiesDialog::startHashCalculation() {
    if (m_hashes_started || m_hash_watcher.isRunning()) {
        return;
    }
    QStringList pending;
    for (auto it = m_hash_checks.cbegin(); it != m_hash_checks.cend(); ++it) {
        if (it.value()->isChecked() && m_hash_values.value(it.key())->text().isEmpty()) {
            pending.append(it.key());
            m_hash_values.value(it.key())->setText(tr("Calculating..."));
        }
    }
    if (pending.isEmpty()) {
        return;
    }
    m_hashes_started = true;
    const FileManagementTarget target = m_target;
    const QString path = m_entries.first().path;
    const quint64 full_size = m_entries.first().size_bytes;
    const std::shared_ptr<const std::atomic_bool> cancel = scanCancelFlag(this);
    m_hash_watcher.setFuture(QtConcurrent::run([target, path, pending, full_size, cancel]() {
        return computeHashes(target, path, pending, full_size, cancel);
    }));
}

void FileExplorerPropertiesDialog::applyHashes(const QMap<QString, QString>& hashes) {
    for (auto it = hashes.cbegin(); it != hashes.cend(); ++it) {
        if (QLabel* value = m_hash_values.value(it.key())) {
            value->setText(it.value());
        }
    }
}

}  // namespace sak
