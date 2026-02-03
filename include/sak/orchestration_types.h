#pragma once

#include <QString>
#include <QDateTime>
#include <QJsonObject>
#include <QVector>

namespace sak {

struct DestinationHealth {
    int cpu_usage_percent{0};
    int ram_usage_percent{0};
    qint64 free_disk_bytes{0};
    int network_latency_ms{0};
    bool sak_service_running{true};
    bool admin_rights{true};

    QJsonObject toJson() const;
    static DestinationHealth fromJson(const QJsonObject& json);
};

struct DestinationPC {
    QString destination_id;
    QString hostname;
    QString ip_address;
    quint16 control_port{54322};
    quint16 data_port{54323};
    QString status{"unknown"};
    QDateTime last_seen;
    DestinationHealth health;

    QJsonObject toJson() const;
    static DestinationPC fromJson(const QJsonObject& json);
};

struct DeploymentAssignment {
    QString deployment_id;
    QString job_id;
    QString source_user;
    qint64 profile_size_bytes{0};
    QString priority{"normal"};
    int max_bandwidth_kbps{0};

    QJsonObject toJson() const;
    static DeploymentAssignment fromJson(const QJsonObject& json);
};

struct DeploymentProgress {
    QString deployment_id;
    QString job_id;
    QString destination_id;
    int progress_percent{0};
    qint64 bytes_transferred{0};
    qint64 bytes_total{0};
    int files_transferred{0};
    int files_total{0};
    QString current_file;
    double transfer_speed_mbps{0.0};
    int eta_seconds{0};

    QJsonObject toJson() const;
    static DeploymentProgress fromJson(const QJsonObject& json);
};

struct DeploymentCompletion {
    QString deployment_id;
    QString job_id;
    QString destination_id;
    QString status;
    QJsonObject summary;

    QJsonObject toJson() const;
    static DeploymentCompletion fromJson(const QJsonObject& json);
};

} // namespace sak

Q_DECLARE_METATYPE(sak::DeploymentAssignment)
Q_DECLARE_METATYPE(sak::DeploymentProgress)
Q_DECLARE_METATYPE(sak::DeploymentCompletion)
