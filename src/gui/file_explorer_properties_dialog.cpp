// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_properties_dialog.cpp
/// @brief Files-style Properties window for the File Management Explorer.

#include "sak/file_explorer_properties_dialog.h"

#include "sak/file_explorer_item_model.h"
#include "sak/layout_constants.h"

#include <QCheckBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTabWidget>
#include <QtConcurrent>
#include <QVBoxLayout>

#include <zlib.h>

namespace sak {

namespace {

// Per-file raw read window for hashing, matching the explorer's on-demand
// hash cap (a larger raw file reports a capped digest).
constexpr uint64_t kPropertiesHashMaxBytes = 512ULL * 1024 * 1024;
// Tree-size walk bounds, shared spirit with the bridge directory walks.
constexpr int kSizeWalkMaxDepth = 32;
constexpr int kSizeWalkMaxEntriesPerDirectory = 10'000;

// Recursive size of a directory tree through the bridge, so raw APFS/HFS
// folders report sizes exactly like mounted ones.
quint64 treeSizeBytes(const FileManagementTarget& target, const QString& path, const int depth) {
    if (depth > kSizeWalkMaxDepth) {
        return 0;
    }
    const FileManagementListResult listing = FileManagementFileSystemBridge::listDirectory(
        target, path, kSizeWalkMaxEntriesPerDirectory);
    if (!listing.ok) {
        return 0;
    }
    quint64 total = 0;
    for (const FileManagementEntry& entry : listing.entries) {
        if (entry.directory) {
            total += treeSizeBytes(target, entry.path, depth + 1);
        } else if (entry.regular_file) {
            total += entry.size_bytes;
        }
    }
    return total;
}

quint64 combinedSizeBytes(const FileManagementTarget& target,
                          const QVector<FileManagementEntry>& entries) {
    quint64 total = 0;
    for (const FileManagementEntry& entry : entries) {
        total += entry.directory ? treeSizeBytes(target, entry.path, 0) : entry.size_bytes;
    }
    return total;
}

QString crc32Hex(const QByteArray& data) {
    const auto value = static_cast<quint32>(crc32(
        0L, reinterpret_cast<const Bytef*>(data.constData()), static_cast<uInt>(data.size())));
    return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0'));
}

// Compute the enabled digests over one bridge read of the file.
QMap<QString, QString> computeHashes(const FileManagementTarget& target,
                                     const QString& path,
                                     const QStringList& algorithms) {
    QMap<QString, QString> hashes;
    const FileManagementReadResult read =
        FileManagementFileSystemBridge::readFile(target, path, kPropertiesHashMaxBytes);
    if (!read.ok) {
        for (const QString& algorithm : algorithms) {
            hashes.insert(algorithm, QStringLiteral("(read failed)"));
        }
        return hashes;
    }
    static const QMap<QString, QCryptographicHash::Algorithm> kQtAlgorithms = {
        {QStringLiteral("MD5"), QCryptographicHash::Md5},
        {QStringLiteral("SHA-1"), QCryptographicHash::Sha1},
        {QStringLiteral("SHA-256"), QCryptographicHash::Sha256},
        {QStringLiteral("SHA-384"), QCryptographicHash::Sha384},
        {QStringLiteral("SHA-512"), QCryptographicHash::Sha512},
    };
    for (const QString& algorithm : algorithms) {
        if (algorithm == QStringLiteral("CRC32")) {
            hashes.insert(algorithm, crc32Hex(read.data));
            continue;
        }
        hashes.insert(
            algorithm,
            QString::fromLatin1(
                QCryptographicHash::hash(read.data, kQtAlgorithms.value(algorithm)).toHex()));
    }
    return hashes;
}

}  // namespace

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

    auto* type_label = new QLabel(single ? first.type : tr("Multiple types"), page);
    type_label->setObjectName(QStringLiteral("fileExplorerPropertiesType"));
    form->addRow(tr("Type:"), type_label);

    auto* location_label = new QLabel(m_target.label, page);
    location_label->setObjectName(QStringLiteral("fileExplorerPropertiesLocation"));
    location_label->setText(single ? first.path : m_target.label);
    location_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Location:"), location_label);

    m_size_label = new QLabel(tr("Calculating..."), page);
    m_size_label->setObjectName(QStringLiteral("fileExplorerPropertiesSize"));
    form->addRow(tr("Size:"), m_size_label);

    auto* fs_label = new QLabel(m_target.file_system, page);
    fs_label->setObjectName(QStringLiteral("fileExplorerPropertiesFileSystem"));
    form->addRow(tr("File system:"), fs_label);

    if (single && !first.identifier.isEmpty()) {
        auto* id_label = new QLabel(first.identifier, page);
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
                     new QLabel(FileExplorerItemModel::attributeSummary(first), page));
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
    connect(&m_size_watcher, &QFutureWatcher<quint64>::finished, this, [this]() {
        if (m_size_label) {
            m_size_label->setText(FileExplorerItemModel::sizeText(m_size_watcher.result()));
        }
    });
    const FileManagementTarget target = m_target;
    const QVector<FileManagementEntry> entries = m_entries;
    m_size_watcher.setFuture(
        QtConcurrent::run([target, entries]() { return combinedSizeBytes(target, entries); }));
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
    m_hash_watcher.setFuture(QtConcurrent::run(
        [target, path, pending]() { return computeHashes(target, path, pending); }));
}

void FileExplorerPropertiesDialog::applyHashes(const QMap<QString, QString>& hashes) {
    for (auto it = hashes.cbegin(); it != hashes.cend(); ++it) {
        if (QLabel* value = m_hash_values.value(it.key())) {
            value->setText(it.value());
        }
    }
}

}  // namespace sak
