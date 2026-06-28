// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file apfs_keybag.h
/// @brief APFS FileVault keybag + key-blob construction (A6 / A-f).
///
/// Builds the on-disk keybag objects and DER key-blobs for a software-encrypted
/// (FileVault) APFS volume, byte-compatible with what macOS writes (harvested
/// from a real `diskutil apfs addVolume -passphrase` volume). Layout:
///   - keybag block = obj_phys(32B) + kb_locker(16B) + packed keybag_entry_t[]
///     (each entry 16-byte aligned), the whole 4096B block then AES-XTS-encrypted
///     with `uuid||uuid` (container keybag = container UUID, volume keybag =
///     volume UUID), tweak base = block_addr * 8.
///   - KEK / VEK blobs = a custom DER/BER SEQUENCE (see buildKekBlob/buildVekBlob).
/// Encryption + checksum stamping are applied by the caller after assembly.

#pragma once

#include <QByteArray>
#include <QList>

#include <cstdint>

namespace sak::apfs_keybag {

/// keybag object-type magics (uint32 value; stored little-endian on disk).
inline constexpr uint32_t kApfsObjectTypeContainerKeybag = 0x6B'65'79'73;  // 'keys'
inline constexpr uint32_t kApfsObjectTypeVolumeKeybag = 0x73'63'65'72;     // 'recs'

inline constexpr uint16_t kApfsKeybagVersion = 2;

/// keybag entry tags (ke_tag).
inline constexpr uint16_t kKbTagVolumeKey = 2;             ///< wrapped VEK blob
inline constexpr uint16_t kKbTagVolumeUnlockRecords = 3;   ///< KEK blob / volume-keybag prange
inline constexpr uint16_t kKbTagVolumePassphraseHint = 4;  ///< UTF-8 hint

/// Personal-recovery-key credential UUID (fixed by Apple).
inline constexpr char kApfsRecoveryKeyUuid[] = "EBC6C064-0000-11AA-AA11-00306543ECAC";

/// @brief One keybag entry (ke_uuid, ke_tag, ke_keydata).
struct KeybagEntry {
    QByteArray uuid;  ///< 16 bytes
    uint16_t tag{0};
    QByteArray keydata;
};

/// @brief Build a plaintext keybag block (obj_phys + kb_locker + entries), padded
/// to @p blockSize. The object checksum is left zero and the block is NOT yet
/// encrypted; the caller stamps Fletcher-64 then AES-XTS-encrypts it.
[[nodiscard]] QByteArray buildKeybagBlock(
    uint32_t magic, uint64_t oid, uint64_t xid, const QList<KeybagEntry>& entries, int blockSize);

/// @brief Inputs for a VEK / KEK key-blob. @c iterations and @c salt are used
/// only by buildKekBlob (the password-derived KEK); buildVekBlob ignores them.
struct KeyBlobParams {
    QByteArray uuid;         ///< 16 bytes (volume UUID)
    QByteArray wrappedKey;   ///< 40 bytes (RFC 3394-wrapped VEK or KEK)
    QByteArray flags8;       ///< 8-byte flags field
    QByteArray hmac32;       ///< 32-byte outer HMAC
    QByteArray outerSalt;    ///< outer salt
    uint64_t iterations{0};  ///< PBKDF2 iterations (KEK only)
    QByteArray salt;         ///< PBKDF2 salt, 16 bytes (KEK only)
};

/// @brief Build a VEK key-blob (container keybag KB_TAG_VOLUME_KEY payload).
/// DER: SEQUENCE { [0x80] unknown=0; [0x81] hmac(32); [0x82] outerSalt;
///                 [0xA3] { [0x80] 0; [0x81] uuid(16); [0x82] flags(8);
///                          [0x83] wrappedKey(40) } }.
[[nodiscard]] QByteArray buildVekBlob(const KeyBlobParams& p);

/// @brief Build a KEK key-blob (volume keybag KB_TAG_VOLUME_UNLOCK_RECORDS payload).
/// As buildVekBlob but the inner keyblob also carries [0x84] iterations and
/// [0x85] salt(16): SEQUENCE { [0x80] 0; [0x81] hmac(32); [0x82] outerSalt;
///   [0xA3] { [0x80] 0; [0x81] uuid(16); [0x82] flags(8); [0x83] wrappedKey(40);
///            [0x84] iterations; [0x85] salt(16) } }.
[[nodiscard]] QByteArray buildKekBlob(const KeyBlobParams& p);

/// @brief Parse a plaintext (already XTS-decrypted) keybag block into its entries.
/// Returns empty if the block is not a valid keybag (wrong magic / version).
[[nodiscard]] QList<KeybagEntry> parseKeybagBlock(const QByteArray& block);

/// @brief Parse a VEK / KEK key-blob (inverse of buildVekBlob/buildKekBlob).
/// Fills @p out (uuid, wrappedKey, flags8, iterations, salt as present).
/// @return true on a well-formed blob.
[[nodiscard]] bool parseKeyBlob(const QByteArray& blob, KeyBlobParams* out);

/// @brief Encode a DER length prefix (short form < 128, else long form).
[[nodiscard]] QByteArray derLength(int length);

/// @brief Encode a DER TLV (tag byte + length + value).
[[nodiscard]] QByteArray derTlv(uint8_t tag, const QByteArray& value);

}  // namespace sak::apfs_keybag
