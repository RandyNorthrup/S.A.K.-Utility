// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file backup_file_codec.cpp
/// @brief Streaming per-file compression/encryption container. See the header for the
/// format and for why a profile backup cannot use the archive-then-encrypt shape.

#include "sak/backup_file_codec.h"

#include "sak/encryption.h"
#include "sak/logger.h"

#include <QDataStream>
#include <QFile>
#include <QFileInfo>

#include <memory>

#include <zlib.h>

namespace sak {

namespace {

constexpr int kMagicBytes = 8;
constexpr int kInnerHeaderBytes = 16;
constexpr int kMinCompressionLevel = 1;
constexpr int kMaxCompressionLevel = 9;
constexpr int kZlibWindowBits = 15;
constexpr int kZlibMemLevel = 8;
constexpr int kDeflateOutBytes = 64 * 1024;

QByteArray plainMagic() {
    return QByteArray("SAKBFC1", kMagicBytes - 1).append('\0');
}

QByteArray encryptedMagic() {
    return QByteArray("SAKBFE1", kMagicBytes - 1).append('\0');
}

quint32 compressedFlag() {
    return 1U;
}

/// flags | reserved | original size, little endian. Lives inside the ciphertext when the
/// container is encrypted, so it is authenticated rather than merely present.
QByteArray buildInnerHeader(bool compressed, qint64 original_size) {
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << (compressed ? compressedFlag() : 0U) << quint32{0} << original_size;
    return header;
}

bool parseInnerHeader(const QByteArray& header, bool* compressed, qint64* original_size) {
    if (header.size() != kInnerHeaderBytes) {
        return false;
    }
    QDataStream stream(header);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 flags = 0;
    quint32 reserved = 0;
    qint64 size = 0;
    stream >> flags >> reserved >> size;
    // Reject anything we do not understand rather than guessing: an unknown flag means a
    // newer writer produced a payload this build cannot correctly interpret.
    if (reserved != 0 || (flags & ~compressedFlag()) != 0 || size < 0) {
        return false;
    }
    *compressed = (flags & compressedFlag()) != 0;
    *original_size = size;
    return true;
}

/// @brief Streaming zlib deflate/inflate over caller-sized chunks.
class ZlibStream {
public:
    ZlibStream() = default;

    ~ZlibStream() {
        if (!m_ready) {
            return;
        }
        if (m_deflating) {
            deflateEnd(&m_zs);
        } else {
            inflateEnd(&m_zs);
        }
    }

    ZlibStream(const ZlibStream&) = delete;
    ZlibStream& operator=(const ZlibStream&) = delete;
    ZlibStream(ZlibStream&&) = delete;
    ZlibStream& operator=(ZlibStream&&) = delete;

    [[nodiscard]] bool initDeflate(int level) {
        m_deflating = true;
        m_ready =
            deflateInit2(
                &m_zs, level, Z_DEFLATED, kZlibWindowBits, kZlibMemLevel, Z_DEFAULT_STRATEGY) ==
            Z_OK;
        if (!m_ready) {
            logError("Backup codec: deflateInit2 failed (level {})", level);
        }
        return m_ready;
    }

    [[nodiscard]] bool initInflate() {
        m_deflating = false;
        m_ready = inflateInit2(&m_zs, kZlibWindowBits) == Z_OK;
        if (!m_ready) {
            logError("Backup codec: inflateInit2 failed");
        }
        return m_ready;
    }

    /// @brief Push @p in through the stream, appending output to @p out.
    /// @param finish True on the last call, which flushes and requires Z_STREAM_END.
    [[nodiscard]] bool run(const QByteArray& in, bool finish, QByteArray* out) {
        if (!m_ready) {
            return false;
        }
        m_zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
        m_zs.avail_in = static_cast<uInt>(in.size());
        QByteArray buffer(kDeflateOutBytes, 0);
        do {
            m_zs.next_out = reinterpret_cast<Bytef*>(buffer.data());
            m_zs.avail_out = static_cast<uInt>(buffer.size());
            const int rc = m_deflating ? deflate(&m_zs, finish ? Z_FINISH : Z_NO_FLUSH)
                                       : inflate(&m_zs, Z_NO_FLUSH);
            if (isFatal(rc)) {
                logError("Backup codec: zlib {} failed (rc {})",
                         m_deflating ? "deflate" : "inflate",
                         rc);
                return false;
            }
            out->append(buffer.constData(), buffer.size() - static_cast<qsizetype>(m_zs.avail_out));
            if (rc == Z_STREAM_END) {
                m_ended = true;
                break;
            }
        } while (m_zs.avail_out == 0 || m_zs.avail_in > 0);
        return true;
    }

    /// @brief True once the compressed stream reached its documented end. Distinguishes a
    /// complete payload from one that was cut short.
    [[nodiscard]] bool ended() const { return m_ended; }

private:
    /// zlib return codes that mean the stream is unusable. Z_BUF_ERROR is deliberately
    /// absent: it only reports that this call made no progress, which is normal when the
    /// output buffer filled exactly.
    [[nodiscard]] static bool isFatal(int rc) {
        return rc == Z_STREAM_ERROR || rc == Z_DATA_ERROR || rc == Z_MEM_ERROR || rc == Z_NEED_DICT;
    }

    z_stream m_zs{};
    bool m_ready{false};
    bool m_deflating{true};
    bool m_ended{false};
};

/// @brief Destination file plus the optional encryptor, so the write path has one sink.
class ContainerSink {
public:
    [[nodiscard]] auto open(const QString& dest_path, const BackupCodecOptions& options)
        -> std::expected<void, error_code> {
        m_file.setFileName(dest_path);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            logError("Backup codec: cannot open destination {}", dest_path.toStdString());
            return std::unexpected(error_code::write_error);
        }
        if (!options.encrypt) {
            return writeRaw(plainMagic());
        }
        auto encryptor = StreamEncryptor::create(options.password);
        if (!encryptor.has_value()) {
            return std::unexpected(encryptor.error());
        }
        m_encryptor = std::move(encryptor.value());
        if (auto written = writeRaw(encryptedMagic()); !written.has_value()) {
            return written;
        }
        return writeRaw(m_encryptor->header());
    }

    /// @brief Append body bytes, encrypting them first when the container is encrypted.
    [[nodiscard]] auto writeBody(const QByteArray& bytes) -> std::expected<void, error_code> {
        if (!m_encryptor) {
            return writeRaw(bytes);
        }
        auto ciphertext = m_encryptor->update(bytes);
        if (!ciphertext.has_value()) {
            return std::unexpected(ciphertext.error());
        }
        return writeRaw(ciphertext.value());
    }

    [[nodiscard]] auto close() -> std::expected<void, error_code> {
        if (m_encryptor) {
            auto tail = m_encryptor->finish();
            if (!tail.has_value()) {
                return std::unexpected(tail.error());
            }
            if (auto written = writeRaw(tail.value()); !written.has_value()) {
                return written;
            }
        }
        // flush() surfaces a full disk that close() alone would swallow.
        if (!m_file.flush()) {
            logError("Backup codec: flush failed for {}", m_file.fileName().toStdString());
            return std::unexpected(error_code::write_error);
        }
        m_file.close();
        return {};
    }

    [[nodiscard]] qint64 bytesWritten() const { return m_written; }

    void discard() {
        m_file.close();
        if (m_file.exists() && !m_file.remove()) {
            logWarning("Backup codec: could not remove partial file {}",
                       m_file.fileName().toStdString());
        }
    }

private:
    [[nodiscard]] auto writeRaw(const QByteArray& bytes) -> std::expected<void, error_code> {
        if (bytes.isEmpty()) {
            return {};
        }
        if (m_file.write(bytes) != bytes.size()) {
            logError("Backup codec: short write to {}", m_file.fileName().toStdString());
            return std::unexpected(error_code::write_error);
        }
        m_written += bytes.size();
        return {};
    }

    QFile m_file;
    std::unique_ptr<StreamEncryptor> m_encryptor;
    qint64 m_written{0};
};

bool cancelRequested(const std::function<bool()>& cancelled) {
    return cancelled && cancelled();
}

auto validateWriteOptions(const BackupCodecOptions& options) -> std::expected<void, error_code> {
    if (options.isPassThrough()) {
        // The caller decides between a verbatim copy and a container; being asked for a
        // container that transforms nothing means the two disagree.
        logError("Backup codec: asked to write a container with neither transform enabled");
        return std::unexpected(error_code::invalid_argument);
    }
    if (options.encrypt && options.password.isEmpty()) {
        // Fail closed. Silently writing plaintext because no password arrived is exactly
        // the "advertised but not applied" behaviour this feature exists to end.
        logError("Backup codec: encryption requested without a password");
        return std::unexpected(error_code::invalid_argument);
    }
    if (options.compress && (options.compression_level < kMinCompressionLevel ||
                             options.compression_level > kMaxCompressionLevel)) {
        logError("Backup codec: compression level {} out of range", options.compression_level);
        return std::unexpected(error_code::invalid_argument);
    }
    return {};
}

/// @brief Feed one plaintext chunk through the optional compressor into the sink.
auto pushChunk(ZlibStream* deflater, ContainerSink* sink, const QByteArray& chunk, bool finish)
    -> std::expected<void, error_code> {
    if (!deflater) {
        return finish ? std::expected<void, error_code>{} : sink->writeBody(chunk);
    }
    QByteArray compressed;
    if (!deflater->run(chunk, finish, &compressed)) {
        return std::unexpected(error_code::corrupted_data);
    }
    return sink->writeBody(compressed);
}

/// @brief Read @p source to end, pushing each chunk through the optional compressor into
/// @p sink and flushing the compressor at the end.
auto copyThroughCodec(QFile* source,
                      ZlibStream* deflater,
                      ContainerSink* sink,
                      const std::function<bool()>& cancelled,
                      qint64* plain_bytes) -> std::expected<void, error_code> {
    QByteArray chunk(kBackupCodecChunkBytes, 0);
    while (!source->atEnd()) {
        if (cancelRequested(cancelled)) {
            return std::unexpected(error_code::operation_cancelled);
        }
        const qint64 read = source->read(chunk.data(), chunk.size());
        if (read < 0) {
            logError("Backup codec: read failed on {}", source->fileName().toStdString());
            return std::unexpected(error_code::read_error);
        }
        *plain_bytes += read;
        if (auto pushed = pushChunk(deflater, sink, QByteArray(chunk.constData(), read), false);
            !pushed.has_value()) {
            return pushed;
        }
    }
    if (!deflater) {
        return {};
    }
    return pushChunk(deflater, sink, QByteArray{}, true);
}

}  // namespace

BackupContainerKind backupContainerKind(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return BackupContainerKind::None;
    }
    const QByteArray magic = file.read(kMagicBytes);
    file.close();
    if (magic == plainMagic()) {
        return BackupContainerKind::Plain;
    }
    if (magic == encryptedMagic()) {
        return BackupContainerKind::Encrypted;
    }
    return BackupContainerKind::None;
}

auto writeBackupFile(const QString& source_path,
                     const QString& dest_path,
                     const BackupCodecOptions& options,
                     const std::function<bool()>& cancelled)
    -> std::expected<BackupCodecResult, error_code> {
    if (auto valid = validateWriteOptions(options); !valid.has_value()) {
        return std::unexpected(valid.error());
    }

    QFile source(source_path);
    if (!source.open(QIODevice::ReadOnly)) {
        logError("Backup codec: cannot read source {}", source_path.toStdString());
        return std::unexpected(error_code::read_error);
    }

    ContainerSink sink;
    if (auto opened = sink.open(dest_path, options); !opened.has_value()) {
        sink.discard();
        return std::unexpected(opened.error());
    }

    std::unique_ptr<ZlibStream> deflater;
    if (options.compress) {
        deflater = std::make_unique<ZlibStream>();
        if (!deflater->initDeflate(options.compression_level)) {
            sink.discard();
            return std::unexpected(error_code::corrupted_data);
        }
    }

    // The inner header goes straight to the sink, NOT through the deflater: the reader has
    // to parse it to learn whether a payload is compressed at all, so it cannot itself be
    // inside the compressed stream. It is still inside the ciphertext when encrypting,
    // which is what makes the compressed flag and the size authenticated.
    const qint64 original_size = source.size();
    if (auto header = sink.writeBody(buildInnerHeader(options.compress, original_size));
        !header.has_value()) {
        sink.discard();
        return std::unexpected(header.error());
    }

    qint64 plain_bytes = 0;
    if (auto copied = copyThroughCodec(&source, deflater.get(), &sink, cancelled, &plain_bytes);
        !copied.has_value()) {
        sink.discard();
        return std::unexpected(copied.error());
    }
    if (auto closed = sink.close(); !closed.has_value()) {
        sink.discard();
        return std::unexpected(closed.error());
    }
    return BackupCodecResult{plain_bytes, sink.bytesWritten()};
}

namespace {

/// @brief Decoding state shared by the two read paths.
struct ReadContext {
    QFile* source{nullptr};
    QFile* staged{nullptr};
    std::unique_ptr<StreamDecryptor> decryptor;
    std::unique_ptr<ZlibStream> inflater;
    QByteArray header_pending;  // buffers the inner header out of the plaintext stream
    bool header_parsed{false};
    bool compressed{false};
    qint64 original_size{0};
    qint64 plain_bytes{0};
};

/// @brief Accumulate the leading inner header out of the plaintext stream. On the call
/// that completes it, @p body is rewritten to whatever followed the header.
auto takeInnerHeader(ReadContext* ctx, QByteArray* body) -> std::expected<void, error_code> {
    ctx->header_pending.append(*body);
    body->clear();
    if (ctx->header_pending.size() < kInnerHeaderBytes) {
        return {};
    }
    if (!parseInnerHeader(
            ctx->header_pending.left(kInnerHeaderBytes), &ctx->compressed, &ctx->original_size)) {
        logError("Backup codec: container header is malformed or from a newer writer");
        return std::unexpected(error_code::invalid_format);
    }
    ctx->header_parsed = true;
    *body = ctx->header_pending.mid(kInnerHeaderBytes);
    ctx->header_pending.clear();
    if (!ctx->compressed) {
        return {};
    }
    ctx->inflater = std::make_unique<ZlibStream>();
    if (!ctx->inflater->initInflate()) {
        return std::unexpected(error_code::corrupted_data);
    }
    return {};
}

/// @brief Split the inner header off the front of the plaintext stream, then write the
/// rest (inflating first when the payload is compressed).
auto consumePlaintext(ReadContext* ctx, const QByteArray& plain)
    -> std::expected<void, error_code> {
    QByteArray body = plain;
    if (!ctx->header_parsed) {
        if (auto header = takeInnerHeader(ctx, &body); !header.has_value()) {
            return header;
        }
    }
    if (body.isEmpty()) {
        return {};
    }

    QByteArray out;
    if (ctx->inflater) {
        if (!ctx->inflater->run(body, false, &out)) {
            return std::unexpected(error_code::corrupted_data);
        }
    } else {
        out = body;
    }
    if (out.isEmpty()) {
        return {};
    }
    if (ctx->staged->write(out) != out.size()) {
        logError("Backup codec: short write while restoring");
        return std::unexpected(error_code::write_error);
    }
    ctx->plain_bytes += out.size();
    return {};
}

/// @brief Stream an encrypted container body, holding the trailing tag back from update().
auto readEncryptedBody(ReadContext* ctx, const std::function<bool()>& cancelled)
    -> std::expected<void, error_code> {
    const qint64 body_end = ctx->source->size() - kEncryptionMacBytes;
    if (ctx->source->pos() > body_end) {
        logError("Backup codec: encrypted container is too short to hold a tag");
        return std::unexpected(error_code::invalid_format);
    }
    QByteArray chunk(kBackupCodecChunkBytes, 0);
    while (ctx->source->pos() < body_end) {
        if (cancelRequested(cancelled)) {
            return std::unexpected(error_code::operation_cancelled);
        }
        const qint64 want = qMin<qint64>(chunk.size(), body_end - ctx->source->pos());
        const qint64 read = ctx->source->read(chunk.data(), want);
        if (read <= 0) {
            logError("Backup codec: read failed inside the encrypted body");
            return std::unexpected(error_code::read_error);
        }
        auto plain = ctx->decryptor->update(QByteArray(chunk.constData(), read));
        if (!plain.has_value()) {
            return std::unexpected(plain.error());
        }
        if (auto consumed = consumePlaintext(ctx, plain.value()); !consumed.has_value()) {
            return consumed;
        }
    }
    auto tail = ctx->decryptor->finish(ctx->source->read(kEncryptionMacBytes));
    if (!tail.has_value()) {
        return std::unexpected(tail.error());
    }
    return consumePlaintext(ctx, tail.value());
}

/// @brief Stream an unencrypted container body.
auto readPlainBody(ReadContext* ctx, const std::function<bool()>& cancelled)
    -> std::expected<void, error_code> {
    QByteArray chunk(kBackupCodecChunkBytes, 0);
    while (!ctx->source->atEnd()) {
        if (cancelRequested(cancelled)) {
            return std::unexpected(error_code::operation_cancelled);
        }
        const qint64 read = ctx->source->read(chunk.data(), chunk.size());
        if (read < 0) {
            logError("Backup codec: read failed inside the container body");
            return std::unexpected(error_code::read_error);
        }
        if (auto consumed = consumePlaintext(ctx, QByteArray(chunk.constData(), read));
            !consumed.has_value()) {
            return consumed;
        }
    }
    return {};
}

/// @brief Reject a decode whose result does not match what the (authenticated) header
/// promised. Catches a truncated deflate stream, which zlib alone would report as merely
/// "no more input".
auto verifyDecoded(const ReadContext& ctx) -> std::expected<void, error_code> {
    if (!ctx.header_parsed) {
        logError("Backup codec: container ended before its header was complete");
        return std::unexpected(error_code::invalid_format);
    }
    if (ctx.inflater && !ctx.inflater->ended()) {
        logError("Backup codec: compressed payload ended early");
        return std::unexpected(error_code::invalid_format);
    }
    if (ctx.plain_bytes != ctx.original_size) {
        logError("Backup codec: restored {} bytes but the header recorded {}",
                 ctx.plain_bytes,
                 ctx.original_size);
        return std::unexpected(error_code::invalid_format);
    }
    return {};
}

/// @brief Set up the decryptor when needed and stream the body through the right reader.
auto decodeBody(ReadContext* ctx,
                BackupContainerKind kind,
                const QString& password,
                const std::function<bool()>& cancelled) -> std::expected<void, error_code> {
    if (kind != BackupContainerKind::Encrypted) {
        return readPlainBody(ctx, cancelled);
    }
    if (password.isEmpty()) {
        logError("Backup codec: this backup is encrypted and no password was supplied");
        return std::unexpected(error_code::invalid_argument);
    }
    auto decryptor = StreamDecryptor::create(password,
                                             ctx->source->read(StreamDecryptor::headerSize()));
    if (!decryptor.has_value()) {
        return std::unexpected(decryptor.error());
    }
    ctx->decryptor = std::move(decryptor.value());
    return readEncryptedBody(ctx, cancelled);
}

/// @brief Move the fully verified staging file onto the destination name.
auto publishRestored(const QString& staged_path, const QString& dest_path)
    -> std::expected<void, error_code> {
    if (QFile::exists(dest_path) && !QFile::remove(dest_path)) {
        logError("Backup codec: cannot replace {}", dest_path.toStdString());
        return std::unexpected(error_code::write_error);
    }
    if (!QFile::rename(staged_path, dest_path)) {
        logError("Backup codec: cannot publish restored file {}", dest_path.toStdString());
        return std::unexpected(error_code::write_error);
    }
    return {};
}

}  // namespace

auto readBackupFile(const QString& source_path,
                    const QString& dest_path,
                    const QString& password,
                    const std::function<bool()>& cancelled)
    -> std::expected<BackupCodecResult, error_code> {
    const BackupContainerKind kind = backupContainerKind(source_path);
    if (kind == BackupContainerKind::None) {
        logError("Backup codec: {} is not a backup container", source_path.toStdString());
        return std::unexpected(error_code::invalid_format);
    }

    QFile source(source_path);
    if (!source.open(QIODevice::ReadOnly)) {
        return std::unexpected(error_code::read_error);
    }
    source.seek(kMagicBytes);

    // Stage beside the destination: an encrypted payload is not authenticated until the
    // very last chunk, so it must never appear under the final name before then.
    const QString staged_path = dest_path + QStringLiteral(".sakpart");
    QFile staged(staged_path);
    if (!staged.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        logError("Backup codec: cannot stage restore at {}", staged_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

    ReadContext ctx;
    ctx.source = &source;
    ctx.staged = &staged;

    auto fail = [&staged](error_code code) -> std::expected<BackupCodecResult, error_code> {
        staged.close();
        if (!staged.remove()) {
            logWarning("Backup codec: could not remove staged file {}",
                       staged.fileName().toStdString());
        }
        return std::unexpected(code);
    };

    if (auto decoded = decodeBody(&ctx, kind, password, cancelled); !decoded.has_value()) {
        return fail(decoded.error());
    }
    if (auto verified = verifyDecoded(ctx); !verified.has_value()) {
        return fail(verified.error());
    }
    if (!staged.flush()) {
        return fail(error_code::write_error);
    }
    staged.close();

    // Publish only now, once the tag verified and the size matched.
    if (auto published = publishRestored(staged_path, dest_path); !published.has_value()) {
        return fail(published.error());
    }
    return BackupCodecResult{ctx.plain_bytes, QFileInfo(source_path).size()};
}

}  // namespace sak
