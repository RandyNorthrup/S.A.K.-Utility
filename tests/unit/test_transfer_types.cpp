#include <QtTest/QtTest>

#include "sak/network_transfer_types.h"

using namespace sak;

class TransferTypesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void fileEntrySerialization();
};

void TransferTypesTests::fileEntrySerialization() {
    TransferFileEntry entry;
    entry.file_id = "file-123";
    entry.relative_path = "User/Documents/file.txt";
    entry.size_bytes = 1024;
    entry.checksum_sha256 = "abc";
    entry.acl_sddl = "O:BAG:BAD:(A;;FA;;;SY)";

    auto json = entry.toJson();
    auto roundtrip = TransferFileEntry::fromJson(json);

    QCOMPARE(roundtrip.file_id, entry.file_id);
    QCOMPARE(roundtrip.relative_path, entry.relative_path);
    QCOMPARE(roundtrip.size_bytes, entry.size_bytes);
    QCOMPARE(roundtrip.checksum_sha256, entry.checksum_sha256);
    QCOMPARE(roundtrip.acl_sddl, entry.acl_sddl);
}

QTEST_MAIN(TransferTypesTests)
#include "test_transfer_types.moc"
