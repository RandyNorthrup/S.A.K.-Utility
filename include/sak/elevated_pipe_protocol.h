// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file elevated_pipe_protocol.h
/// @brief Wire protocol for Named Pipe IPC between main app and elevated helper
///
/// Messages are framed as: [4-byte little-endian length][1-byte type][payload]
/// Payload is UTF-8 JSON for structured data.

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QString>

#include <cstdint>

namespace sak {

// ======================================================================
// Message Types
// ======================================================================

/// @brief IPC message types sent over the named pipe
enum class PipeMessageType : uint8_t {
    // Client → Server
    TaskRequest = 0x01,    ///< Execute a task: { task, payload }
    CancelRequest = 0x02,  ///< Cancel current task: { task }
    Shutdown = 0x03,       ///< Gracefully shut down helper

    // Server → Client
    ProgressUpdate = 0x10,  ///< Progress: { percent, status }
    TaskResult = 0x11,      ///< Final result: { success, data }
    TaskError = 0x12,       ///< Error: { code, message }
    Ready = 0x13,           ///< Helper is ready to accept tasks
};

// ======================================================================
// Protocol Constants
// ======================================================================

/// @brief Header size: 4 bytes length + 1 byte type
inline constexpr int kPipeHeaderSize = 5;

/// @brief Maximum message payload (4 MB — generous for JSON, prevents abuse)
inline constexpr uint32_t kPipeMaxPayload = 4 * 1024 * 1024;

/// @brief Named pipe base path
inline constexpr char kPipeBasePath[] = "\\\\.\\pipe\\SAK_Elevated_";

/// @brief Helper inactivity timeout (5 minutes)
inline constexpr int kHelperTimeoutMs = 5 * 60 * 1000;

/// @brief Pipe connection timeout from client side
inline constexpr int kPipeConnectTimeoutMs = 10'000;

/// @brief Pipe read/write timeout
inline constexpr int kPipeIoTimeoutMs = 30'000;

// ======================================================================
// Message Framing
// ======================================================================

/// @brief Frame a message for transmission: [length][type][payload]
[[nodiscard]] inline QByteArray frameMessage(PipeMessageType type, const QByteArray& payload = {}) {
    uint32_t payload_len = static_cast<uint32_t>(payload.size());
    QByteArray frame;
    frame.reserve(kPipeHeaderSize + static_cast<int>(payload_len));

    // Little-endian length (of payload only, not header)
    frame.append(static_cast<char>(payload_len & 0xFF));
    frame.append(static_cast<char>((payload_len >> 8) & 0xFF));
    frame.append(static_cast<char>((payload_len >> 16) & 0xFF));
    frame.append(static_cast<char>((payload_len >> 24) & 0xFF));

    // Message type
    frame.append(static_cast<char>(type));

    // Payload
    if (!payload.isEmpty()) {
        frame.append(payload);
    }

    return frame;
}

/// @brief Build a JSON task request message
[[nodiscard]] inline QByteArray buildTaskRequest(const QString& task_id,
                                                 const QJsonObject& payload) {
    QJsonObject msg;
    msg["task"] = task_id;
    msg["payload"] = payload;
    return frameMessage(PipeMessageType::TaskRequest,
                        QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

/// @brief Build a JSON progress update message
[[nodiscard]] inline QByteArray buildProgressUpdate(int percent, const QString& status) {
    QJsonObject msg;
    msg["percent"] = percent;
    msg["status"] = status;
    return frameMessage(PipeMessageType::ProgressUpdate,
                        QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

/// @brief Build a JSON task result message
[[nodiscard]] inline QByteArray buildTaskResult(bool success, const QJsonObject& data) {
    QJsonObject msg;
    msg["success"] = success;
    msg["data"] = data;
    return frameMessage(PipeMessageType::TaskResult,
                        QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

/// @brief Build a JSON error message
[[nodiscard]] inline QByteArray buildTaskError(int code, const QString& message) {
    QJsonObject msg;
    msg["code"] = code;
    msg["message"] = message;
    return frameMessage(PipeMessageType::TaskError,
                        QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

/// @brief Build a cancel request message
[[nodiscard]] inline QByteArray buildCancelRequest(const QString& task_id) {
    QJsonObject msg;
    msg["task"] = task_id;
    return frameMessage(PipeMessageType::CancelRequest,
                        QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

/// @brief Build a shutdown message
[[nodiscard]] inline QByteArray buildShutdown() {
    return frameMessage(PipeMessageType::Shutdown);
}

/// @brief Build a ready message
[[nodiscard]] inline QByteArray buildReady() {
    return frameMessage(PipeMessageType::Ready);
}

// ======================================================================
// Parsed Message
// ======================================================================

/// @brief A parsed pipe message
struct PipeMessage {
    PipeMessageType type{};
    QJsonObject json;
    bool valid{false};
};

/// @brief Parse a raw payload into a PipeMessage
[[nodiscard]] inline PipeMessage parsePayload(PipeMessageType type, const QByteArray& payload) {
    PipeMessage msg;
    msg.type = type;

    if (payload.isEmpty()) {
        // Messages like Shutdown and Ready have no payload
        msg.valid = (type == PipeMessageType::Shutdown || type == PipeMessageType::Ready);
        return msg;
    }

    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return msg;  // valid = false
    }

    msg.json = doc.object();
    msg.valid = true;
    return msg;
}

/// @brief Generate a unique pipe name with random nonce
[[nodiscard]] inline QString generatePipeName() {
    // Use process ID + random to prevent pipe squatting
    quint32 pid = static_cast<quint32>(QCoreApplication::applicationPid());
    quint64 nonce = 0;

    // Use crypto-quality randomness from QRandomGenerator
    auto* gen = QRandomGenerator::global();
    nonce = gen->generate64();

    return QString("%1%2_%3").arg(kPipeBasePath).arg(pid).arg(nonce, 16, 16, QChar('0'));
}

}  // namespace sak
