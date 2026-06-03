// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_report_generator.cpp
/// @brief Report generation for Partition Manager.

#include "sak/partition_report_generator.h"

#include "sak/report_style_constants.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace sak {

namespace {

QString escapeHtml(QString value) {
    return value.replace(QStringLiteral("&"), QStringLiteral("&amp;"))
        .replace(QStringLiteral("<"), QStringLiteral("&lt;"))
        .replace(QStringLiteral(">"), QStringLiteral("&gt;"))
        .replace(QStringLiteral("\""), QStringLiteral("&quot;"));
}

QString diskRows(const PartitionInventory& inventory) {
    QString rows;
    for (const auto& disk : inventory.disks) {
        rows += QStringLiteral(
                    "<tr><td>Disk %1</td><td>%2</td><td>%3</td><td>%4</td>"
                    "<td>%5</td><td>%6</td></tr>")
                    .arg(disk.disk_number)
                    .arg(escapeHtml(disk.model))
                    .arg(escapeHtml(disk.partition_style))
                    .arg(formatPartitionBytes(disk.size_bytes))
                    .arg(escapeHtml(disk.health_status))
                    .arg(disk.partitions.size());
    }
    return rows;
}

QString stepRows(const PartitionExecutionResult& result) {
    QString rows;
    for (const auto& step : result.steps) {
        rows += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td><pre>%4</pre></td></tr>")
                    .arg(escapeHtml(step.summary))
                    .arg(step.success ? QStringLiteral("Success") : QStringLiteral("Failed"))
                    .arg(escapeHtml(step.error_message))
                    .arg(escapeHtml(step.stdout_text.left(kPartitionReportOutputPreviewChars)));
    }
    return rows;
}

QJsonArray disksToJson(const PartitionInventory& inventory) {
    QJsonArray array;
    PartitionReportGenerator generator;
    for (const auto& disk : inventory.disks) {
        array.append(generator.diskToJson(disk));
    }
    return array;
}

}  // namespace

QString PartitionReportGenerator::generateHtml(const PartitionInventory& before,
                                               const PartitionInventory& after,
                                               const PartitionExecutionResult& result) const {
    return QString::fromLatin1(report::kPartitionManagerReportDocumentOpen)
               .arg(QString::fromLatin1(report::kPartitionManagerReportStyle)) +
           QStringLiteral(
               "<h1>Partition Manager Report</h1>"
               "<p><b>Status:</b> %1</p><p><b>Message:</b> %2</p>"
               "<p><b>Before layout hash:</b> %3</p><p><b>After layout hash:</b> %4</p>"
               "<h2>Before Layout</h2><table><tr><th>Disk</th><th>Model</th><th>Style</th>"
               "<th>Size</th><th>Health</th><th>Partitions</th></tr>%5</table>"
               "<h2>After Layout</h2><table><tr><th>Disk</th><th>Model</th><th>Style</th>"
               "<th>Size</th><th>Health</th><th>Partitions</th></tr>%6</table>"
               "<h2>Execution</h2><table><tr><th>Step</th><th>Status</th><th>Error</th>"
               "<th>Output</th></tr>%7</table></body></html>")
               .arg(result.success ? QStringLiteral("Success") : QStringLiteral("Failed"))
               .arg(escapeHtml(result.message),
                    escapeHtml(before.layout_hash),
                    escapeHtml(after.layout_hash),
                    diskRows(before),
                    diskRows(after),
                    stepRows(result));
}

QString PartitionReportGenerator::generateJson(const PartitionInventory& before,
                                               const PartitionInventory& after,
                                               const PartitionExecutionResult& result) const {
    QJsonArray steps;
    for (const auto& step : result.steps) {
        steps.append(operationStepToJson(step));
    }

    QJsonObject root;
    root[QStringLiteral("schema_version")] = 1;
    root[QStringLiteral("batch_id")] = result.batch_id;
    root[QStringLiteral("success")] = result.success;
    root[QStringLiteral("dry_run")] = result.dry_run;
    root[QStringLiteral("message")] = result.message;
    root[QStringLiteral("before_layout_hash")] = before.layout_hash;
    root[QStringLiteral("after_layout_hash")] = after.layout_hash;
    root[QStringLiteral("before_disks")] = disksToJson(before);
    root[QStringLiteral("after_disks")] = disksToJson(after);
    root[QStringLiteral("steps")] = steps;
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QJsonObject PartitionReportGenerator::diskToJson(const PartitionDiskInfo& disk) {
    QJsonArray partitions;
    for (const auto& partition : disk.partitions) {
        QJsonObject p;
        p[QStringLiteral("number")] = static_cast<int>(partition.partition_number);
        p[QStringLiteral("guid")] = partition.partition_guid;
        p[QStringLiteral("type")] = partition.type_name;
        p[QStringLiteral("offset_bytes")] = QString::number(partition.offset_bytes);
        p[QStringLiteral("size_bytes")] = QString::number(partition.size_bytes);
        p[QStringLiteral("file_system")] = partition.volume ? partition.volume->file_system
                                                            : QString();
        p[QStringLiteral("drive_letter")] = partition.volume ? partition.volume->drive_letter
                                                             : QString();
        partitions.append(p);
    }

    QJsonObject object;
    object[QStringLiteral("disk_number")] = static_cast<int>(disk.disk_number);
    object[QStringLiteral("model")] = disk.model;
    object[QStringLiteral("serial_number")] = disk.serial_number;
    object[QStringLiteral("partition_style")] = disk.partition_style;
    object[QStringLiteral("size_bytes")] = QString::number(disk.size_bytes);
    object[QStringLiteral("health_status")] = disk.health_status;
    object[QStringLiteral("partitions")] = partitions;
    return object;
}

QJsonObject PartitionReportGenerator::operationStepToJson(const PartitionExecutionStep& step) {
    QJsonObject object;
    object[QStringLiteral("operation_id")] = step.operation_id;
    object[QStringLiteral("summary")] = step.summary;
    object[QStringLiteral("success")] = step.success;
    object[QStringLiteral("skipped")] = step.skipped;
    object[QStringLiteral("stdout")] = step.stdout_text;
    object[QStringLiteral("stderr")] = step.stderr_text;
    object[QStringLiteral("error_message")] = step.error_message;
    return object;
}

}  // namespace sak
