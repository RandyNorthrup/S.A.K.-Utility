// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_elevation_hardening.cpp
/// @brief Phase 5 hardening tests — legacy removal verification, tier coverage,
///        gate behavior, cancellation, and elevated-mode bypass

#include "sak/elevated_pipe_protocol.h"
#include "sak/elevated_task_dispatcher.h"
#include "sak/elevation_gate.h"
#include "sak/elevation_manager.h"
#include "sak/elevation_tier.h"
#include "sak/error_codes.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

#include <set>
#include <string>

class TestElevationHardening : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ====================================================================
    // 5.1 — Legacy Removal Verification
    // ====================================================================
    void legacyElevationManager_delegatesToIsElevated();

    // ====================================================================
    // Tier 1 — All Standard features must not require elevation
    // ====================================================================
    void tier1_advancedSearchIsStandard();
    void tier1_fileOrganizerIsStandard();
    void tier1_duplicateFinderIsStandard();
    void tier1_emailInspectorIsStandard();
    void tier1_generateSystemReportIsStandard();
    void tier1_screenshotSettingsIsStandard();
    void tier1_optimizePowerSettingsIsStandard();
    void tier1_backupCurrentUserIsStandard();
    void tier1_restoreToUserPathsIsStandard();
    void tier1_appScanningIsStandard();
    void tier1_chocolateyBrowseIsStandard();
    void tier1_networkDiagnosticsIsStandard();
    void tier1_wifiAnalyzerIsStandard();
    void tier1_activeConnectionsIsStandard();
    void tier1_firewallAuditReadIsStandard();
    void tier1_portScannerIsStandard();
    void tier1_bandwidthTestIsStandard();
    void tier1_internetSpeedTestIsStandard();
    void tier1_networkShareBrowserIsStandard();
    void tier1_dnsLookupIsStandard();
    void tier1_hardwareInventoryIsStandard();
    void tier1_stressTestIsStandard();

    // ====================================================================
    // Tier 2 — All Elevated features must require elevation
    // ====================================================================
    void tier2_backupBitlockerKeysIsElevated();
    void tier2_verifySystemFilesIsElevated();
    void tier2_checkDiskErrorsIsElevated();
    void tier2_resetNetworkStackIsElevated();
    void tier2_flashUsbDriveIsElevated();
    void tier2_uupIsoConversionIsElevated();
    void tier2_backupCrossUserIsElevated();
    void tier2_restoreWithAclsIsElevated();
    void tier2_chocolateyInstallIsElevated();
    void tier2_chocolateyUninstallIsElevated();
    void tier2_advancedUninstallIsElevated();
    void tier2_firewallAuditModifyIsElevated();
    void tier2_dnsCacheFlushIsElevated();
    void tier2_modifyAdapterConfigIsElevated();
    void tier2_cpuThermalDataIsElevated();
    void tier2_permissionTakeOwnershipIsElevated();

    // ====================================================================
    // Tier 2 — Gate function classification
    // ====================================================================
    void gate_enumContainsAllThreeResults();
    void gate_alreadyElevatedSkipsDialog();

    // ====================================================================
    // Tier 3 — Mixed features need elevation for some sub-tasks
    // ====================================================================
    void tier3_permissionStripInheritIsMixed();
    void tier3_permissionSetAclIsMixed();
    void tier3_mixedFeaturesNeedElevation();

    // ====================================================================
    // Feature table completeness
    // ====================================================================
    void featureTable_allElevatedHaveNonEmptyReasons();
    void featureTable_noStandardFeatureHasReason();
    void featureTable_countMatchesExpected();

    // ====================================================================
    // Dispatcher — allowlist hardening
    // ====================================================================
    void dispatcher_rejectsEmptyTaskId();
    void dispatcher_rejectsUnregisteredTaskId();
    void dispatcher_multipleRegistrationsAllAllowed();

    // ====================================================================
    // Error codes — all elevation error codes exist
    // ====================================================================
    void errorCodes_elevationRequiredExists();
    void errorCodes_elevationFailedExists();
    void errorCodes_elevationDeniedExists();
    void errorCodes_elevationTimeoutExists();
    void errorCodes_helperConnectionFailedExists();
    void errorCodes_helperCrashedExists();
    void errorCodes_taskNotAllowedExists();

    // ====================================================================
    // IPC protocol — cancellation round-trip
    // ====================================================================
    void ipc_cancelRequestRoundTrip();
    void ipc_taskRequestPreservesPayload();
};

// ============================================================================
// 5.1 — Legacy Removal Verification
// ============================================================================

void TestElevationHardening::legacyElevationManager_delegatesToIsElevated() {
    // Verify the consolidated isElevated() returns consistently
    bool first = sak::ElevationManager::isElevated();
    bool second = sak::ElevationManager::isElevated();
    QCOMPARE(first, second);
}

// ============================================================================
// Tier 1 — Standard Features
// ============================================================================

static void verifyTierIs(sak::FeatureId id, sak::ElevationTier expected_tier, const char* name) {
    const auto* entry = sak::findFeatureElevation(id);
    QVERIFY2(entry != nullptr, qPrintable(QString("Feature '%1' not found in table").arg(name)));
    QCOMPARE(entry->tier, expected_tier);
}

void TestElevationHardening::tier1_advancedSearchIsStandard() {
    verifyTierIs(sak::FeatureId::AdvancedSearch, sak::ElevationTier::Standard, "AdvancedSearch");
}

void TestElevationHardening::tier1_fileOrganizerIsStandard() {
    verifyTierIs(sak::FeatureId::FileOrganizer, sak::ElevationTier::Standard, "FileOrganizer");
}

void TestElevationHardening::tier1_duplicateFinderIsStandard() {
    verifyTierIs(sak::FeatureId::DuplicateFinder, sak::ElevationTier::Standard, "DuplicateFinder");
}

void TestElevationHardening::tier1_emailInspectorIsStandard() {
    verifyTierIs(sak::FeatureId::EmailInspector, sak::ElevationTier::Standard, "EmailInspector");
}

void TestElevationHardening::tier1_generateSystemReportIsStandard() {
    verifyTierIs(sak::FeatureId::GenerateSystemReport,
                 sak::ElevationTier::Standard,
                 "GenerateSystemReport");
}

void TestElevationHardening::tier1_screenshotSettingsIsStandard() {
    verifyTierIs(sak::FeatureId::ScreenshotSettings,
                 sak::ElevationTier::Standard,
                 "ScreenshotSettings");
}

void TestElevationHardening::tier1_optimizePowerSettingsIsStandard() {
    verifyTierIs(sak::FeatureId::OptimizePowerSettings,
                 sak::ElevationTier::Standard,
                 "OptimizePowerSettings");
}

void TestElevationHardening::tier1_backupCurrentUserIsStandard() {
    verifyTierIs(sak::FeatureId::BackupCurrentUser,
                 sak::ElevationTier::Standard,
                 "BackupCurrentUser");
}

void TestElevationHardening::tier1_restoreToUserPathsIsStandard() {
    verifyTierIs(sak::FeatureId::RestoreToUserPaths,
                 sak::ElevationTier::Standard,
                 "RestoreToUserPaths");
}

void TestElevationHardening::tier1_appScanningIsStandard() {
    verifyTierIs(sak::FeatureId::AppScanning, sak::ElevationTier::Standard, "AppScanning");
}

void TestElevationHardening::tier1_chocolateyBrowseIsStandard() {
    verifyTierIs(sak::FeatureId::ChocolateyBrowse,
                 sak::ElevationTier::Standard,
                 "ChocolateyBrowse");
}

void TestElevationHardening::tier1_networkDiagnosticsIsStandard() {
    verifyTierIs(sak::FeatureId::NetworkDiagnostics,
                 sak::ElevationTier::Standard,
                 "NetworkDiagnostics");
}

void TestElevationHardening::tier1_wifiAnalyzerIsStandard() {
    verifyTierIs(sak::FeatureId::WifiAnalyzer, sak::ElevationTier::Standard, "WifiAnalyzer");
}

void TestElevationHardening::tier1_activeConnectionsIsStandard() {
    verifyTierIs(sak::FeatureId::ActiveConnections,
                 sak::ElevationTier::Standard,
                 "ActiveConnections");
}

void TestElevationHardening::tier1_firewallAuditReadIsStandard() {
    verifyTierIs(sak::FeatureId::FirewallAuditRead,
                 sak::ElevationTier::Standard,
                 "FirewallAuditRead");
}

void TestElevationHardening::tier1_portScannerIsStandard() {
    verifyTierIs(sak::FeatureId::PortScanner, sak::ElevationTier::Standard, "PortScanner");
}

void TestElevationHardening::tier1_bandwidthTestIsStandard() {
    verifyTierIs(sak::FeatureId::BandwidthTest, sak::ElevationTier::Standard, "BandwidthTest");
}

void TestElevationHardening::tier1_internetSpeedTestIsStandard() {
    verifyTierIs(sak::FeatureId::InternetSpeedTest,
                 sak::ElevationTier::Standard,
                 "InternetSpeedTest");
}

void TestElevationHardening::tier1_networkShareBrowserIsStandard() {
    verifyTierIs(sak::FeatureId::NetworkShareBrowser,
                 sak::ElevationTier::Standard,
                 "NetworkShareBrowser");
}

void TestElevationHardening::tier1_dnsLookupIsStandard() {
    verifyTierIs(sak::FeatureId::DnsLookup, sak::ElevationTier::Standard, "DnsLookup");
}

void TestElevationHardening::tier1_hardwareInventoryIsStandard() {
    verifyTierIs(sak::FeatureId::HardwareInventory,
                 sak::ElevationTier::Standard,
                 "HardwareInventory");
}

void TestElevationHardening::tier1_stressTestIsStandard() {
    verifyTierIs(sak::FeatureId::StressTest, sak::ElevationTier::Standard, "StressTest");
}

// ============================================================================
// Tier 2 — Elevated Features
// ============================================================================

void TestElevationHardening::tier2_backupBitlockerKeysIsElevated() {
    verifyTierIs(sak::FeatureId::BackupBitlockerKeys,
                 sak::ElevationTier::Elevated,
                 "BackupBitlockerKeys");
}

void TestElevationHardening::tier2_verifySystemFilesIsElevated() {
    verifyTierIs(sak::FeatureId::VerifySystemFiles,
                 sak::ElevationTier::Elevated,
                 "VerifySystemFiles");
}

void TestElevationHardening::tier2_checkDiskErrorsIsElevated() {
    verifyTierIs(sak::FeatureId::CheckDiskErrors, sak::ElevationTier::Elevated, "CheckDiskErrors");
}

void TestElevationHardening::tier2_resetNetworkStackIsElevated() {
    verifyTierIs(sak::FeatureId::ResetNetworkStack,
                 sak::ElevationTier::Elevated,
                 "ResetNetworkStack");
}

void TestElevationHardening::tier2_flashUsbDriveIsElevated() {
    verifyTierIs(sak::FeatureId::FlashUsbDrive, sak::ElevationTier::Elevated, "FlashUsbDrive");
}

void TestElevationHardening::tier2_uupIsoConversionIsElevated() {
    verifyTierIs(sak::FeatureId::UupIsoConversion,
                 sak::ElevationTier::Elevated,
                 "UupIsoConversion");
}

void TestElevationHardening::tier2_backupCrossUserIsElevated() {
    verifyTierIs(sak::FeatureId::BackupCrossUser, sak::ElevationTier::Elevated, "BackupCrossUser");
}

void TestElevationHardening::tier2_restoreWithAclsIsElevated() {
    verifyTierIs(sak::FeatureId::RestoreWithAcls, sak::ElevationTier::Elevated, "RestoreWithAcls");
}

void TestElevationHardening::tier2_chocolateyInstallIsElevated() {
    verifyTierIs(sak::FeatureId::ChocolateyInstall,
                 sak::ElevationTier::Elevated,
                 "ChocolateyInstall");
}

void TestElevationHardening::tier2_chocolateyUninstallIsElevated() {
    verifyTierIs(sak::FeatureId::ChocolateyUninstall,
                 sak::ElevationTier::Elevated,
                 "ChocolateyUninstall");
}

void TestElevationHardening::tier2_advancedUninstallIsElevated() {
    verifyTierIs(sak::FeatureId::AdvancedUninstall,
                 sak::ElevationTier::Elevated,
                 "AdvancedUninstall");
}

void TestElevationHardening::tier2_firewallAuditModifyIsElevated() {
    verifyTierIs(sak::FeatureId::FirewallAuditModify,
                 sak::ElevationTier::Elevated,
                 "FirewallAuditModify");
}

void TestElevationHardening::tier2_dnsCacheFlushIsElevated() {
    verifyTierIs(sak::FeatureId::DnsCacheFlush, sak::ElevationTier::Elevated, "DnsCacheFlush");
}

void TestElevationHardening::tier2_modifyAdapterConfigIsElevated() {
    verifyTierIs(sak::FeatureId::ModifyAdapterConfig,
                 sak::ElevationTier::Elevated,
                 "ModifyAdapterConfig");
}

void TestElevationHardening::tier2_cpuThermalDataIsElevated() {
    verifyTierIs(sak::FeatureId::CpuThermalData, sak::ElevationTier::Elevated, "CpuThermalData");
}

void TestElevationHardening::tier2_permissionTakeOwnershipIsElevated() {
    verifyTierIs(sak::FeatureId::PermissionTakeOwnership,
                 sak::ElevationTier::Elevated,
                 "PermissionTakeOwnership");
}

// ============================================================================
// Tier 2 — Gate function classification
// ============================================================================

void TestElevationHardening::gate_enumContainsAllThreeResults() {
    auto already = sak::ElevationGateResult::AlreadyElevated;
    auto restart = sak::ElevationGateResult::RestartRequested;
    auto declined = sak::ElevationGateResult::Declined;
    QVERIFY(already != restart);
    QVERIFY(restart != declined);
    QVERIFY(already != declined);
}

void TestElevationHardening::gate_alreadyElevatedSkipsDialog() {
    if (!sak::ElevationManager::isElevated()) {
        QSKIP("Test requires elevated process to verify bypass path");
    }
    auto result = sak::showElevationGate(nullptr, "Test Feature", "Test reason");
    QCOMPARE(result, sak::ElevationGateResult::AlreadyElevated);
}

// ============================================================================
// Tier 3 — Mixed features
// ============================================================================

void TestElevationHardening::tier3_permissionStripInheritIsMixed() {
    verifyTierIs(sak::FeatureId::PermissionStripInherit,
                 sak::ElevationTier::Mixed,
                 "PermissionStripInherit");
}

void TestElevationHardening::tier3_permissionSetAclIsMixed() {
    verifyTierIs(sak::FeatureId::PermissionSetAcl, sak::ElevationTier::Mixed, "PermissionSetAcl");
}

void TestElevationHardening::tier3_mixedFeaturesNeedElevation() {
    QVERIFY(sak::featureNeedsElevation(sak::FeatureId::PermissionStripInherit));
    QVERIFY(sak::featureNeedsElevation(sak::FeatureId::PermissionSetAcl));
}

// ============================================================================
// Feature table completeness
// ============================================================================

void TestElevationHardening::featureTable_allElevatedHaveNonEmptyReasons() {
    for (const auto& entry : sak::kFeatureElevationTable) {
        if (entry.tier == sak::ElevationTier::Elevated || entry.tier == sak::ElevationTier::Mixed) {
            QVERIFY2(!entry.reason.empty(),
                     qPrintable(QString("Feature '%1' is %2 but has empty reason")
                                    .arg(QString::fromUtf8(entry.name.data(),
                                                           static_cast<int>(entry.name.size())))
                                    .arg(entry.tier == sak::ElevationTier::Elevated ? "Elevated"
                                                                                    : "Mixed")));
        }
    }
}

void TestElevationHardening::featureTable_noStandardFeatureHasReason() {
    for (const auto& entry : sak::kFeatureElevationTable) {
        if (entry.tier == sak::ElevationTier::Standard) {
            QVERIFY2(entry.reason.empty(),
                     qPrintable(QString("Standard feature '%1' should have empty reason")
                                    .arg(QString::fromUtf8(entry.name.data(),
                                                           static_cast<int>(entry.name.size())))));
        }
    }
}

void TestElevationHardening::featureTable_countMatchesExpected() {
    constexpr size_t kMinExpectedFeatures = 40;
    const bool has_enough = sak::kFeatureCount >= kMinExpectedFeatures;
    QVERIFY(has_enough);

    size_t standard_count = 0;
    size_t elevated_count = 0;
    size_t mixed_count = 0;
    for (const auto& entry : sak::kFeatureElevationTable) {
        switch (entry.tier) {
        case sak::ElevationTier::Standard:
            ++standard_count;
            break;
        case sak::ElevationTier::Elevated:
            ++elevated_count;
            break;
        case sak::ElevationTier::Mixed:
            ++mixed_count;
            break;
        }
    }
    QVERIFY2(standard_count > 0, "Must have Standard features");
    QVERIFY2(elevated_count > 0, "Must have Elevated features");
    QVERIFY2(mixed_count > 0, "Must have Mixed features");
    QCOMPARE(standard_count + elevated_count + mixed_count, sak::kFeatureCount);
}

// ============================================================================
// Dispatcher — allowlist hardening
// ============================================================================

void TestElevationHardening::dispatcher_rejectsEmptyTaskId() {
    sak::ElevatedTaskDispatcher dispatcher;
    QVERIFY(!dispatcher.isAllowed(""));
}

void TestElevationHardening::dispatcher_rejectsUnregisteredTaskId() {
    sak::ElevatedTaskDispatcher dispatcher;
    dispatcher.registerHandler(
        "KnownTask",
        [](const QJsonObject&, sak::ProgressCallback, sak::CancelCheck) -> sak::TaskHandlerResult {
            return sak::TaskHandlerResult{true, QJsonObject{}, {}};
        });
    QVERIFY(!dispatcher.isAllowed("UnknownMaliciousTask"));
    QVERIFY(!dispatcher.isAllowed("KNOWNTASK"));  // case-sensitive
}

void TestElevationHardening::dispatcher_multipleRegistrationsAllAllowed() {
    sak::ElevatedTaskDispatcher dispatcher;
    const QStringList task_ids = {"TakeOwnership",
                                  "StripPermissions",
                                  "SetStandardPermissions",
                                  "BackupFile",
                                  "ReadThermalData"};
    for (const auto& id : task_ids) {
        dispatcher.registerHandler(id,
                                   [](const QJsonObject&,
                                      sak::ProgressCallback,
                                      sak::CancelCheck) -> sak::TaskHandlerResult {
                                       return sak::TaskHandlerResult{true, QJsonObject{}, {}};
                                   });
    }
    for (const auto& id : task_ids) {
        QVERIFY2(dispatcher.isAllowed(id),
                 qPrintable(QString("Task '%1' should be allowed").arg(id)));
    }
    QVERIFY(!dispatcher.isAllowed("NotRegistered"));
}

// ============================================================================
// Error codes — all elevation error codes exist
// ============================================================================

void TestElevationHardening::errorCodes_elevationRequiredExists() {
    auto code = sak::error_code::elevation_required;
    QVERIFY(!std::string(sak::to_string(code)).empty());
}

void TestElevationHardening::errorCodes_elevationFailedExists() {
    auto code = sak::error_code::elevation_failed;
    QVERIFY(!std::string(sak::to_string(code)).empty());
}

void TestElevationHardening::errorCodes_elevationDeniedExists() {
    auto code = sak::error_code::elevation_denied;
    QVERIFY(!std::string(sak::to_string(code)).empty());
}

void TestElevationHardening::errorCodes_elevationTimeoutExists() {
    auto code = sak::error_code::elevation_timeout;
    QVERIFY(!std::string(sak::to_string(code)).empty());
}

void TestElevationHardening::errorCodes_helperConnectionFailedExists() {
    auto code = sak::error_code::helper_connection_failed;
    QVERIFY(!std::string(sak::to_string(code)).empty());
}

void TestElevationHardening::errorCodes_helperCrashedExists() {
    auto code = sak::error_code::helper_crashed;
    QVERIFY(!std::string(sak::to_string(code)).empty());
}

void TestElevationHardening::errorCodes_taskNotAllowedExists() {
    auto code = sak::error_code::task_not_allowed;
    QVERIFY(!std::string(sak::to_string(code)).empty());
}

// ============================================================================
// IPC protocol — cancellation round-trip
// ============================================================================

void TestElevationHardening::ipc_cancelRequestRoundTrip() {
    QByteArray frame = sak::buildCancelRequest("TestTask");
    QVERIFY(!frame.isEmpty());
    QVERIFY(frame.size() >= sak::kPipeHeaderSize);

    auto type =
        static_cast<sak::PipeMessageType>(static_cast<uint8_t>(frame.at(sak::kPipeHeaderSize - 1)));
    QCOMPARE(type, sak::PipeMessageType::CancelRequest);

    QByteArray payload_bytes = frame.mid(sak::kPipeHeaderSize);
    QJsonDocument doc = QJsonDocument::fromJson(payload_bytes);
    QVERIFY(doc.isObject());
    QCOMPARE(doc.object()["task"].toString(), QString("TestTask"));
}

void TestElevationHardening::ipc_taskRequestPreservesPayload() {
    QJsonObject payload;
    payload["path"] = QString::fromUtf8("C:\\Windows\\System32\\test.dll");
    payload["recursive"] = true;

    QByteArray frame = sak::buildTaskRequest("TakeOwnership", payload);
    QVERIFY(!frame.isEmpty());

    QByteArray payload_bytes = frame.mid(sak::kPipeHeaderSize);
    QJsonDocument doc = QJsonDocument::fromJson(payload_bytes);
    QVERIFY(doc.isObject());

    QJsonObject parsed = doc.object();
    QCOMPARE(parsed["task"].toString(), QString("TakeOwnership"));
    QCOMPARE(parsed["payload"].toObject()["path"].toString(),
             QString("C:\\Windows\\System32\\test.dll"));
    QCOMPARE(parsed["payload"].toObject()["recursive"].toBool(), true);
}

QTEST_MAIN(TestElevationHardening)
#include "test_elevation_hardening.moc"
