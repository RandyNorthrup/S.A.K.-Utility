// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/deployment_summary_report.h"

#include <QFile>
#include <QPdfWriter>
#include <QSaveFile>
#include <QTextDocument>
#include <QTextStream>
#include <algorithm>

namespace sak {

static QString escapeCsv(const QString& value) {
    QString escaped = value;
    escaped.replace('"', "\"\"");
    return '"' + escaped + '"';
}

bool DeploymentSummaryReport::exportCsv(const QString& filePath,
                                        const DeploymentSummaryData& data) {
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QTextStream stream(&file);
    stream << "Deployment Summary\n";
    stream << "deployment_id," << escapeCsv(data.deployment_id) << "\n";
    stream << "started_at," << escapeCsv(data.started_at.toString(Qt::ISODate)) << "\n";
    stream << "completed_at," << escapeCsv(data.completed_at.toString(Qt::ISODate)) << "\n";
    stream << "\nDestinations\n";
    stream << "destination_id,hostname,ip_address,status,progress_percent,last_seen,events\n";

    for (const auto& destination : data.destinations) {
        stream << escapeCsv(destination.destination_id) << ',' << escapeCsv(destination.hostname)
               << ',' << escapeCsv(destination.ip_address) << ',' << escapeCsv(destination.status)
               << ',' << destination.progress_percent << ','
               << escapeCsv(destination.last_seen.toString(Qt::ISODate)) << ','
               << escapeCsv(destination.status_events.join(" | ")) << "\n";
    }

    stream << "\nJobs\n";
    stream << "job_id,source_user,destination_id,status,bytes_transferred,total_bytes,error\n";
    for (const auto& job : data.jobs) {
        stream << escapeCsv(job.job_id) << ',' << escapeCsv(job.source_user) << ','
               << escapeCsv(job.destination_id) << ',' << escapeCsv(job.status) << ','
               << job.bytes_transferred << ',' << job.total_bytes << ','
               << escapeCsv(job.error_message) << "\n";
    }

    stream.flush();
    return file.commit();
}

bool DeploymentSummaryReport::exportPdf(const QString& filePath,
                                        const DeploymentSummaryData& data) {
    QPdfWriter writer(filePath);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setTitle(QString("Deployment Summary - %1").arg(data.deployment_id));

    QString html;
    html += QString("<h1>Deployment Summary</h1>");
    html += QString("<p><b>Deployment ID:</b> %1</p>").arg(data.deployment_id.toHtmlEscaped());
    html += QString("<p><b>Started:</b> %1</p>")
                .arg(data.started_at.toString(Qt::ISODate).toHtmlEscaped());
    html += QString("<p><b>Completed:</b> %1</p>")
                .arg(data.completed_at.toString(Qt::ISODate).toHtmlEscaped());

    html += "<h2>Destinations</h2><table border='1' cellspacing='0' cellpadding='4'>";
    html +=
        "<tr><th>ID</th><th>Host</th><th>IP</th><th>Status</th><th>Progress</th><th>Last "
        "Seen</th><th>Events</th></tr>";
    std::for_each(data.destinations.begin(), data.destinations.end(),
        [&html](const auto& destination) {
            html += QString(
                        "<tr><td>%1</td><td>%2</td><td>%3</td>"
                        "<td>%4</td><td>%5%</td><td>%6</td>"
                        "<td>%7</td></tr>")
                        .arg(destination.destination_id.toHtmlEscaped())
                        .arg(destination.hostname.toHtmlEscaped())
                        .arg(destination.ip_address.toHtmlEscaped())
                        .arg(destination.status.toHtmlEscaped())
                        .arg(destination.progress_percent)
                        .arg(destination.last_seen.toString(Qt::ISODate).toHtmlEscaped())
                        .arg(destination.status_events.join(" | ").toHtmlEscaped());
        });
    html += "</table>";

    html += "<h2>Jobs</h2><table border='1' cellspacing='0' cellpadding='4'>";
    html +=
        "<tr><th>Job "
        "ID</th><th>Source</th><th>Destination</th>"
        "<th>Status</th><th>Transferred</th>"
        "<th>Total</th><th>Error</th></tr>";
    std::for_each(data.jobs.begin(), data.jobs.end(),
        [&html](const auto& job) {
            html += QString(
                        "<tr><td>%1</td><td>%2</td><td>%3</td>"
                        "<td>%4</td><td>%5</td><td>%6</td>"
                        "<td>%7</td></tr>")
                        .arg(job.job_id.toHtmlEscaped())
                        .arg(job.source_user.toHtmlEscaped())
                        .arg(job.destination_id.toHtmlEscaped())
                        .arg(job.status.toHtmlEscaped())
                        .arg(job.bytes_transferred)
                        .arg(job.total_bytes)
                        .arg(job.error_message.toHtmlEscaped());
        });
    html += "</table>";

    QTextDocument doc;
    doc.setHtml(html);
    doc.print(&writer);
    return true;
}

}  // namespace sak
