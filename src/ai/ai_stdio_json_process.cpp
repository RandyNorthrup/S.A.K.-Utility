// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_stdio_json_process.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace sak::ai {

namespace {

constexpr int kStopGraceMs = 2000;
constexpr int kJsonRpcInternalError = -32'603;

}  // namespace

AiStdioJsonProcess::AiStdioJsonProcess(QObject* parent)
    : QObject(parent), m_process(new QProcess(this)) {
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &AiStdioJsonProcess::handleStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &AiStdioJsonProcess::handleStderr);
    connect(m_process, &QProcess::started, this, &AiStdioJsonProcess::started);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            Q_EMIT startFailed(m_process->errorString());
        }
    });
    connect(m_process, &QProcess::finished, this, [this](int exit_code, QProcess::ExitStatus) {
        handleStdout();
        Q_EMIT finished(exit_code, stderrTail());
    });
}

AiStdioJsonProcess::~AiStdioJsonProcess() {
    m_process->disconnect(this);
    stop();
}

void AiStdioJsonProcess::start(const LaunchSpec& spec) {
    m_stdout_buffer.clear();
    m_stderr_tail.clear();
    m_process->setProgram(spec.program);
    m_process->setArguments(spec.arguments);
    m_process->setProcessEnvironment(spec.environment);
    m_process->setWorkingDirectory(spec.working_directory);
    m_process->start(QIODevice::ReadWrite);
}

void AiStdioJsonProcess::stop() {
    if (m_process->state() == QProcess::NotRunning) {
        return;
    }
    // Closing stdin is the documented shutdown signal for every runtime.
    m_process->closeWriteChannel();
    if (!m_process->waitForFinished(kStopGraceMs)) {
        m_process->kill();
        m_process->waitForFinished(kStopGraceMs);
    }
}

bool AiStdioJsonProcess::isRunning() const {
    return m_process->state() == QProcess::Running;
}

bool AiStdioJsonProcess::send(const QJsonObject& message) {
    if (!isRunning()) {
        return false;
    }
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');
    return m_process->write(line) == line.size();
}

QString AiStdioJsonProcess::stderrTail() const {
    return QString::fromUtf8(m_stderr_tail).trimmed();
}

QList<QJsonObject> AiStdioJsonProcess::takeJsonLines(QByteArray* buffer) {
    QList<QJsonObject> messages;
    qsizetype newline = buffer->indexOf('\n');
    while (newline >= 0) {
        QByteArray line = buffer->left(newline).trimmed();
        buffer->remove(0, newline + 1);
        if (line.startsWith('{')) {
            QJsonParseError error;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
            if (error.error == QJsonParseError::NoError && doc.isObject()) {
                messages.append(doc.object());
            }
        }
        newline = buffer->indexOf('\n');
    }
    if (buffer->size() > kMaxAgentFrameBytes) {
        buffer->clear();  // Oversized frame: drop rather than grow unbounded.
    }
    return messages;
}

void AiStdioJsonProcess::handleStdout() {
    m_stdout_buffer.append(m_process->readAllStandardOutput());
    for (const auto& message : takeJsonLines(&m_stdout_buffer)) {
        Q_EMIT jsonReceived(message);
    }
}

void AiStdioJsonProcess::handleStderr() {
    m_stderr_tail.append(m_process->readAllStandardError());
    if (m_stderr_tail.size() > kAgentStderrTailBytes) {
        m_stderr_tail = m_stderr_tail.right(kAgentStderrTailBytes);
    }
}

AiJsonRpcPeer::AiJsonRpcPeer(AiStdioJsonProcess* process, bool include_jsonrpc_field)
    : QObject(process), m_process(process), m_include_jsonrpc_field(include_jsonrpc_field) {
    connect(process, &AiStdioJsonProcess::jsonReceived, this, &AiJsonRpcPeer::handleMessage);
}

QJsonObject AiJsonRpcPeer::envelope() const {
    QJsonObject message;
    if (m_include_jsonrpc_field) {
        message[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    }
    return message;
}

void AiJsonRpcPeer::request(const QString& method,
                            const QJsonObject& params,
                            ResultHandler handler) {
    const qint64 id = m_next_id++;
    QJsonObject message = envelope();
    message[QStringLiteral("id")] = id;
    message[QStringLiteral("method")] = method;
    message[QStringLiteral("params")] = params;
    if (handler) {
        m_pending.insert(id, std::move(handler));
    }
    if (!m_process->send(message)) {
        const ResultHandler failed = m_pending.take(id);
        if (failed) {
            failed({}, tr("Agent runtime is not running"));
        }
    }
}

void AiJsonRpcPeer::notify(const QString& method, const QJsonObject& params) {
    QJsonObject message = envelope();
    message[QStringLiteral("method")] = method;
    message[QStringLiteral("params")] = params;
    m_process->send(message);
}

void AiJsonRpcPeer::respond(const QJsonValue& id, const QJsonObject& result) {
    QJsonObject message = envelope();
    message[QStringLiteral("id")] = id;
    message[QStringLiteral("result")] = result;
    m_process->send(message);
}

void AiJsonRpcPeer::respondError(const QJsonValue& id, int code, const QString& error_message) {
    QJsonObject message = envelope();
    message[QStringLiteral("id")] = id;
    message[QStringLiteral("error")] = QJsonObject{{QStringLiteral("code"), code},
                                                   {QStringLiteral("message"), error_message}};
    m_process->send(message);
}

void AiJsonRpcPeer::failAll(const QString& error_message) {
    const auto pending = std::exchange(m_pending, {});
    for (const auto& handler : pending) {
        handler({}, error_message);
    }
}

void AiJsonRpcPeer::handleMessage(const QJsonObject& message) {
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonValue id = message.value(QStringLiteral("id"));
    if (!method.isEmpty()) {
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        if (id.isUndefined() || id.isNull()) {
            Q_EMIT notificationReceived(method, params);
        } else {
            Q_EMIT requestReceived(id, method, params);
        }
        return;
    }
    const ResultHandler handler = m_pending.take(static_cast<qint64>(id.toDouble(-1)));
    if (!handler) {
        return;
    }
    const QJsonObject error = message.value(QStringLiteral("error")).toObject();
    if (!error.isEmpty()) {
        const QString text = error.value(QStringLiteral("message")).toString();
        handler({},
                text.isEmpty()
                    ? tr("Agent error %1")
                          .arg(error.value(QStringLiteral("code")).toInt(kJsonRpcInternalError))
                    : text);
        return;
    }
    handler(message.value(QStringLiteral("result")).toObject(), {});
}

}  // namespace sak::ai
