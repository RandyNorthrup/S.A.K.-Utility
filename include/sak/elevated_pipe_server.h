// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file elevated_pipe_server.h
/// @brief Named pipe server running inside the elevated helper process

#include "sak/elevated_pipe_protocol.h"
#include "sak/error_codes.h"

#include <QObject>
#include <QString>

#include <expected>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sak {

/// @brief Named pipe server for the elevated helper process
///
/// Creates a named pipe, waits for a single client connection,
/// and provides send/receive primitives for framed messages.
///
/// Thread-Safety: Single-threaded; call from the helper's main thread.
class ElevatedPipeServer : public QObject {
    Q_OBJECT

public:
    /// @param pipe_name  Full pipe path (e.g. "\\\\.\\pipe\\SAK_Elevated_1234_...")
    /// @param parent_pid Expected parent process ID for validation
    explicit ElevatedPipeServer(const QString& pipe_name,
                                qint64 parent_pid,
                                QObject* parent = nullptr);
    ~ElevatedPipeServer() override;

    // Non-copyable
    ElevatedPipeServer(const ElevatedPipeServer&) = delete;
    ElevatedPipeServer& operator=(const ElevatedPipeServer&) = delete;

    /// @brief Create the pipe and wait for the client to connect
    /// @return true if a client connected successfully
    [[nodiscard]] bool start();

    /// @brief Send a progress update to the client
    void sendProgress(int percent, const QString& status);

    /// @brief Send a task result to the client
    void sendResult(bool success, const QJsonObject& data);

    /// @brief Send an error to the client
    void sendError(int code, const QString& message);

    /// @brief Send the Ready message after initialization
    void sendReady();

    /// @brief Read the next framed message from the client
    [[nodiscard]] auto readMessage() -> std::expected<PipeMessage, sak::error_code>;

    /// @brief Check whether at least one byte is pending on the pipe
    [[nodiscard]] bool hasPendingMessage() const;

    /// @brief Tri-state pipe poll that distinguishes a broken pipe from merely no-data, so a
    ///        caller can cancel a privileged task when the client has died instead of looping
    ///        forever treating a dead pipe as "no message".
    enum class PipePoll {
        NoData,
        MessageReady,
        Broken
    };
    /// @brief Pure classifier: a failed PeekNamedPipe means Broken; otherwise data-available maps
    ///        to MessageReady and none to NoData. Unit-testable without a real pipe.
    [[nodiscard]] static PipePoll classifyPeek(bool peek_ok, unsigned long bytes_available) {
        if (!peek_ok) {
            return PipePoll::Broken;
        }
        return bytes_available > 0 ? PipePoll::MessageReady : PipePoll::NoData;
    }
    /// @brief Poll the live pipe and classify the result (Broken on an invalid handle or a failed
    ///        PeekNamedPipe).
    [[nodiscard]] PipePoll pollPipe() const;

    /// @brief Check if client is still connected
    [[nodiscard]] bool isConnected() const;

    /// @brief Close the pipe
    void stop();

    /// @brief Whether a connecting client's PID is the authorized parent.
    /// @param expectedParentPid the parent PID the helper was launched with.
    /// @param clientPid the PID reported for the connected client.
    /// @return true ONLY when expectedParentPid is valid (>0) AND clientPid
    ///         equals it.
    /// @note Fail-closed: a missing/invalid expected PID (<=0) authorizes NOBODY.
    ///       Previously the helper skipped validation entirely when no parent PID
    ///       was supplied, accepting any client. Pure + static (inline) for unit
    ///       testing without linking the server implementation.
    [[nodiscard]] static bool clientPidMatchesParent(qint64 expectedParentPid, qint64 clientPid) {
        return expectedParentPid > 0 && clientPid == expectedParentPid;
    }

    /// @brief Whether a connecting client's image path is the authorized executable.
    /// @param expectedImagePath the full path of the executable the client MUST be.
    /// @param clientImagePath the full image path reported for the connected client.
    /// @return true ONLY when both paths are non-empty AND compare equal
    ///         case-insensitively (Win32 paths are case-insensitive).
    /// @note Defense against PID reuse: a raw PID match alone can be satisfied by an
    ///       unrelated process that inherited the recycled parent PID. Binding to the
    ///       image path requires the client to also be running from the exact expected
    ///       executable. Fail-closed: an empty expected or client path authorizes
    ///       NOBODY. Pure + static (inline) for unit testing without linking the
    ///       server implementation.
    [[nodiscard]] static bool clientImageMatchesExpected(const QString& expectedImagePath,
                                                         const QString& clientImagePath) {
        return !expectedImagePath.isEmpty() && !clientImagePath.isEmpty() &&
               QString::compare(expectedImagePath, clientImagePath, Qt::CaseInsensitive) == 0;
    }

    /// @brief Decoded 5-byte frame header: the little-endian payload length and type
    ///        byte, plus whether the declared length is within the protocol ceiling.
    struct DecodedFrameHeader {
        uint32_t payload_len{0};
        PipeMessageType type{};
        bool length_within_cap{false};
    };

    /// @brief Pure decode of the 5-byte frame header ([4-byte little-endian length]
    ///        [1-byte type]); the exact inverse of frameMessage()'s encode.
    /// @param header pointer to at least kPipeHeaderSize readable bytes.
    /// @note The 4-byte length prefix is fully attacker-controlled -- it arrives off the
    ///       untrusted pipe BEFORE any payload. length_within_cap is false when the
    ///       declared length exceeds kPipeMaxPayload, so readMessage() fails closed
    ///       BEFORE it resizes a buffer and reads an attacker-declared oversized payload.
    ///       Decoding each byte through uint32_t keeps the top-byte (<<24) shift well
    ///       defined; a signed-int shift of a 0x80..0xFF byte would be UB. Pure + static
    ///       (inline) for unit testing without a live pipe, matching classifyPeek() and
    ///       clientPidMatchesParent() in this class.
    [[nodiscard]] static DecodedFrameHeader decodeFrameHeader(const char* header) {
        const uint32_t payload_len =
            static_cast<uint32_t>(static_cast<uint8_t>(header[kPipeFrameLengthByte0])) |
            (static_cast<uint32_t>(static_cast<uint8_t>(header[kPipeFrameLengthByte1]))
             << kPipeFrameByteShift1) |
            (static_cast<uint32_t>(static_cast<uint8_t>(header[kPipeFrameLengthByte2]))
             << kPipeFrameByteShift2) |
            (static_cast<uint32_t>(static_cast<uint8_t>(header[kPipeFrameLengthByte3]))
             << kPipeFrameByteShift3);
        return DecodedFrameHeader{
            payload_len,
            static_cast<PipeMessageType>(static_cast<uint8_t>(header[kPipeFrameTypeByte])),
            payload_len <= kPipeMaxPayload,
        };
    }

private:
    /// @brief Send raw bytes
    [[nodiscard]] bool sendRaw(const QByteArray& data);

    /// @brief Read exactly N bytes
    [[nodiscard]] bool readExact(char* buffer, int size, int timeout_ms);

    /// @brief Validate the connecting client's parent PID
    [[nodiscard]] bool validateClient() const;

#ifdef _WIN32
    /// @brief Verify the client PID's image path is the authorized app executable
    [[nodiscard]] bool validateClientImage(unsigned long client_pid) const;
#endif

    QString m_pipe_name;
    qint64 m_parent_pid{0};

#ifdef _WIN32
    HANDLE m_pipe_handle{INVALID_HANDLE_VALUE};
#endif
};

}  // namespace sak
