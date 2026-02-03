#include "sak/orchestration_types.h"

namespace sak {

QJsonObject DestinationHealth::toJson() const {
    QJsonObject json;
    json["cpu_usage_percent"] = cpu_usage_percent;
    json["ram_usage_percent"] = ram_usage_percent;
    json["free_disk_bytes"] = static_cast<qint64>(free_disk_bytes);
    json["network_latency_ms"] = network_latency_ms;
    json["sak_service_running"] = sak_service_running;
    json["admin_rights"] = admin_rights;
    return json;
}

DestinationHealth DestinationHealth::fromJson(const QJsonObject& json) {
    DestinationHealth health;
    health.cpu_usage_percent = json.value("cpu_usage_percent").toInt();
    health.ram_usage_percent = json.value("ram_usage_percent").toInt();
    health.free_disk_bytes = json.value("free_disk_bytes").toVariant().toLongLong();
    health.network_latency_ms = json.value("network_latency_ms").toInt();
    health.sak_service_running = json.value("sak_service_running").toBool(true);
    health.admin_rights = json.value("admin_rights").toBool(true);
    return health;
}

QJsonObject DestinationPC::toJson() const {
    QJsonObject json;
    json["destination_id"] = destination_id;
    json["hostname"] = hostname;
    json["ip_address"] = ip_address;
    json["control_port"] = static_cast<int>(control_port);
    json["data_port"] = static_cast<int>(data_port);
    json["status"] = status;
    json["last_seen"] = last_seen.toSecsSinceEpoch();
    json["health"] = health.toJson();
    return json;
}

DestinationPC DestinationPC::fromJson(const QJsonObject& json) {
    DestinationPC pc;
    pc.destination_id = json.value("destination_id").toString();
    pc.hostname = json.value("hostname").toString();
    pc.ip_address = json.value("ip_address").toString();
    pc.control_port = static_cast<quint16>(json.value("control_port").toInt(54322));
    pc.data_port = static_cast<quint16>(json.value("data_port").toInt(54323));
    pc.status = json.value("status").toString("unknown");
    pc.last_seen = QDateTime::fromSecsSinceEpoch(json.value("last_seen").toVariant().toLongLong());
    if (json.contains("health") && json.value("health").isObject()) {
        pc.health = DestinationHealth::fromJson(json.value("health").toObject());
    }
    return pc;
}

QJsonObject DeploymentAssignment::toJson() const {
    QJsonObject json;
    json["deployment_id"] = deployment_id;
    json["job_id"] = job_id;
    json["source_user"] = source_user;
    json["profile_size_bytes"] = static_cast<qint64>(profile_size_bytes);
    json["priority"] = priority;
    json["max_bandwidth_kbps"] = max_bandwidth_kbps;
    return json;
}

DeploymentAssignment DeploymentAssignment::fromJson(const QJsonObject& json) {
    DeploymentAssignment assignment;
    assignment.deployment_id = json.value("deployment_id").toString();
    assignment.job_id = json.value("job_id").toString();
    assignment.source_user = json.value("source_user").toString();
    assignment.profile_size_bytes = json.value("profile_size_bytes").toVariant().toLongLong();
    assignment.priority = json.value("priority").toString("normal");
    assignment.max_bandwidth_kbps = json.value("max_bandwidth_kbps").toInt(0);
    return assignment;
}

QJsonObject DeploymentProgress::toJson() const {
    QJsonObject json;
    json["deployment_id"] = deployment_id;
    json["job_id"] = job_id;
    json["destination_id"] = destination_id;
    json["progress_percent"] = progress_percent;
    json["bytes_transferred"] = static_cast<qint64>(bytes_transferred);
    json["bytes_total"] = static_cast<qint64>(bytes_total);
    json["files_transferred"] = files_transferred;
    json["files_total"] = files_total;
    json["current_file"] = current_file;
    json["transfer_speed_mbps"] = transfer_speed_mbps;
    json["eta_seconds"] = eta_seconds;
    return json;
}

DeploymentProgress DeploymentProgress::fromJson(const QJsonObject& json) {
    DeploymentProgress progress;
    progress.deployment_id = json.value("deployment_id").toString();
    progress.job_id = json.value("job_id").toString();
    progress.destination_id = json.value("destination_id").toString();
    progress.progress_percent = json.value("progress_percent").toInt();
    progress.bytes_transferred = json.value("bytes_transferred").toVariant().toLongLong();
    progress.bytes_total = json.value("bytes_total").toVariant().toLongLong();
    progress.files_transferred = json.value("files_transferred").toInt();
    progress.files_total = json.value("files_total").toInt();
    progress.current_file = json.value("current_file").toString();
    progress.transfer_speed_mbps = json.value("transfer_speed_mbps").toDouble();
    progress.eta_seconds = json.value("eta_seconds").toInt();
    return progress;
}

QJsonObject DeploymentCompletion::toJson() const {
    QJsonObject json;
    json["deployment_id"] = deployment_id;
    json["job_id"] = job_id;
    json["destination_id"] = destination_id;
    json["status"] = status;
    json["summary"] = summary;
    return json;
}

DeploymentCompletion DeploymentCompletion::fromJson(const QJsonObject& json) {
    DeploymentCompletion completion;
    completion.deployment_id = json.value("deployment_id").toString();
    completion.job_id = json.value("job_id").toString();
    completion.destination_id = json.value("destination_id").toString();
    completion.status = json.value("status").toString();
    completion.summary = json.value("summary").toObject();
    return completion;
}

} // namespace sak
