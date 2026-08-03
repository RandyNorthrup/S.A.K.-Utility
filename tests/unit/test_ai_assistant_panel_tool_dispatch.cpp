// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ai_assistant_panel_tool_dispatch.cpp
/// @brief Certification test for P10-04: built-in AI tool handlers that block
/// (download/package/offline/provider-gateway) must run off the GUI thread so
/// the GUI stays responsive and Stop can cancel. Drives the real AiAssistantPanel
/// tool loop with an injected blocking handler and inspects the private async
/// state via a friend seam.

#include "sak/advanced_uninstall_types.h"
#include "sak/ai/ai_async_tool_runner.h"
#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_command_tool_planner.h"
#include "sak/ai/ai_execution_broker.h"
#include "sak/ai/ai_orchestrator.h"
#include "sak/ai/ai_skill.h"
#include "sak/ai/ai_skill_store.h"
#include "sak/ai/ai_tool_dispatcher.h"
#include "sak/ai/ai_tool_health_ledger.h"
#include "sak/ai/ai_tool_loop_detector.h"
#include "sak/ai/ai_tool_policy.h"
#include "sak/ai/openai_response_types.h"
#include "sak/ai_assistant_panel.h"
#include "sak/app_mutating_actions.h"
#include "sak/flash_coordinator.h"
#include "sak/leftover_scan_provenance.h"
#include "sak/partition_apply_worker.h"
#include "sak/partition_operation_planner.h"
#include "sak/storage_inventory_worker.h"
#include "sak/uninstall_worker.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>
#include <QUuid>

#include <atomic>
#include <cstring>
#include <memory>

#if defined(SAK_WIN_REGISTRY_TESTS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sak {

// Deterministic stand-in for the delegate sub-agent's model client. Records that
// it was invoked (on the async worker) and returns a complete result, so the
// delegate_subagent cert never touches the network.
class FakeSubagentModelClient : public ai::IAiModelClient {
public:
    explicit FakeSubagentModelClient(std::atomic<bool>* invoked) : m_invoked(invoked) {}

    Response invoke(const Request& /*request*/, const ai::CancellationToken& /*token*/) override {
        if (m_invoked != nullptr) {
            m_invoked->store(true);
        }
        Response response;
        response.success = true;
        response.text =
            QStringLiteral("{\"status\":\"complete\",\"summary\":\"delegated task done\"}");
        response.usage.total_tokens = 10;
        return response;
    }

private:
    std::atomic<bool>* m_invoked{nullptr};
};

// Friend of AiAssistantPanel: reaches the private tool dispatcher, run token, and
// async-in-flight state so the certification can drive and observe the real loop.
class AiAssistantPanelToolDispatchTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        // Keep the panel's stores out of the real user data directory.
        QStandardPaths::setTestModeEnabled(true);
    }

#if defined(SAK_WIN_REGISTRY_TESTS)
    // The panel's Install/Uninstall button drives BrowserExtensionInstaller in-process.
    // Point it at a throwaway HKCU key + temp dir (never the real Chrome policy) via the
    // friend seam, then confirm a click installs and a second click uninstalls, reflected
    // in the button label.
    void browserExtensionButtonInstallsAndUninstalls() {
        AiAssistantPanel panel;
        const std::wstring base = L"Software\\SAK_Test\\PanelExt";
        RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
        auto cleanup = qScopeGuard([&] { RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str()); });

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString crx = dir.filePath(QStringLiteral("sak_browser_control.crx"));
        {
            QFile f(crx);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("CRX");
        }
        sak::win32mcp::ExtensionInstallConfig cfg;
        cfg.data_dir = dir.path();
        cfg.crx_path = crx;
        cfg.host_exe_path = dir.filePath(QStringLiteral("sak_win32_mcp.exe"));
        cfg.forcelist_key_path = QStringLiteral("Software\\SAK_Test\\PanelExt\\Forcelist");
        cfg.native_host_key_path =
            QStringLiteral("Software\\SAK_Test\\PanelExt\\NativeHost\\com.sak.browsercontrol");
        panel.m_browserExtensionConfig = cfg;

        panel.refreshBrowserExtensionStatus();
        QVERIFY(panel.m_browserExtensionButton != nullptr);
        QCOMPARE(panel.m_browserExtensionButton->text(), QStringLiteral("Install extension"));

        panel.onBrowserExtensionButtonClicked();  // install
        QCOMPARE(panel.m_browserExtensionButton->text(), QStringLiteral("Uninstall extension"));

        panel.onBrowserExtensionButtonClicked();  // uninstall
        QCOMPARE(panel.m_browserExtensionButton->text(), QStringLiteral("Install extension"));
    }
#endif

    // A download_file call whose handler blocks must run off the GUI thread: the
    // handler enters on a non-GUI thread, the GUI event loop keeps running, and
    // the async-in-flight flag is set. Releasing the handler drives the completion
    // back onto the GUI thread and advances the tool turn.
    void blockingBuiltInRunsOffGuiThreadAndCompletes() {
        AiAssistantPanel panel;
        std::atomic<bool> entered{false};
        std::atomic<Qt::HANDLE> handler_thread{QThread::currentThreadId()};
        QSemaphore gate;

        installBlockingDownloadHandler(panel, &entered, &handler_thread, &gate);
        allowLocalToolsAndValidRun(panel);

        QSignalSpy finished_spy(panel.m_asyncToolRunner.get(), &ai::AiAsyncToolRunner::finished);
        panel.beginToolTurn(downloadResponse());

        // GUI thread was not blocked: the event loop still runs and the handler
        // enters on a different thread.
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 5000);
        QVERIFY(handler_thread.load() != QThread::currentThreadId());
        QVERIFY(panel.m_asyncToolInFlight);
        QVERIFY(panel.m_asyncToolRunner->isRunning());
        QVERIFY(panel.m_toolTurn.active());

        // Let the handler finish: completion marshals back to the GUI thread,
        // clears the in-flight flag and records the completed tool.
        gate.release();
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_asyncToolInFlight, 5000);
        QCOMPARE(finished_spy.count(), 1);
        QCOMPARE(panel.m_runState.completed_tools, 1);
    }

    // Stop while a blocking handler is mid-flight must cancel: the run token is
    // cancelled and the runner is detached, so the late result is dropped -- the
    // tool turn does not advance and no completion is delivered.
    void stopCancelsInFlightBlockingTool() {
        AiAssistantPanel panel;
        std::atomic<bool> entered{false};
        std::atomic<Qt::HANDLE> handler_thread{QThread::currentThreadId()};
        QSemaphore gate;

        installBlockingDownloadHandler(panel, &entered, &handler_thread, &gate);
        allowLocalToolsAndValidRun(panel);

        QSignalSpy finished_spy(panel.m_asyncToolRunner.get(), &ai::AiAsyncToolRunner::finished);
        panel.beginToolTurn(downloadResponse());
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 5000);

        panel.onStopClicked();
        QVERIFY(panel.m_activeToolRunToken.isValid());
        QVERIFY(panel.m_activeToolRunToken.isCancellationRequested());
        QVERIFY(!panel.m_asyncToolInFlight);
        QVERIFY(!panel.m_toolTurn.active());

        // Release the (now detached) handler and confirm the stale result is
        // dropped: finished() is never delivered and no tool is recorded.
        gate.release();
        QTest::qWait(300);
        QCOMPARE(finished_spy.count(), 0);
        QCOMPARE(panel.m_runState.completed_tools, 0);
    }

    // Destroying the panel while an async built-in tool is still running must not
    // dead-lock or use-after-free: the destructor drains the worker (which
    // captured `this`) before members are torn down.
    void destroyingPanelWhileToolRunsDrainsCleanly() {
        auto panel = std::make_unique<AiAssistantPanel>();
        std::atomic<bool> entered{false};
        std::atomic<Qt::HANDLE> handler_thread{QThread::currentThreadId()};

        // Handler ignores any gate and just sleeps briefly, so the worker is
        // genuinely in-flight when the panel is destroyed.
        panel->m_toolDispatcher->registerHandler(
            QStringLiteral("download_file"),
            [&entered, &handler_thread](const QJsonObject&, const ai::AiToolPolicyDecision&) {
                handler_thread = QThread::currentThreadId();
                entered = true;
                QThread::msleep(200);
                return QJsonObject{{QStringLiteral("success"), true}};
            });
        allowLocalToolsAndValidRun(*panel);
        panel->beginToolTurn(downloadResponse());
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 5000);
        QVERIFY(panel->m_asyncToolInFlight);

        // Destroy mid-flight; must return without hanging or crashing.
        panel.reset();
        QVERIFY(true);
    }

    // Offline install_bundle with a wrong-typed 'packed_only' (a JSON number, which
    // QJsonValue::toBool() would silently read as false) is REJECTED up front, so a
    // stringified/mistyped flag can never quietly disable the air-gap and let the
    // install fetch from the network.
    void offlineInstallRejectsWrongTypedPackedOnly() {
        AiAssistantPanel panel;
        const QJsonObject result = panel.offlineRunOperation(
            QJsonObject{{QStringLiteral("manifest_path"), QStringLiteral("m.json")},
                        {QStringLiteral("packed_only"), 1}},
            QStringLiteral("install_bundle"));
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(QStringLiteral("packed_only")));
    }

    // A malformed package_id (a path-like 'fire/fox') is rejected rather than
    // silently repaired into a DIFFERENT valid package ('firefox'), which would
    // redirect the install to the wrong product.
    void offlineDownloadRejectsMalformedPackageId() {
        AiAssistantPanel panel;
        QJsonArray packages;
        packages.append(QJsonObject{{QStringLiteral("package_id"), QStringLiteral("fire/fox")}});
        const QJsonObject result = panel.offlineRunOperation(
            QJsonObject{{QStringLiteral("packages"), packages}}, QStringLiteral("direct_download"));
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(QStringLiteral("invalid package_id")));
    }

    // A valid recovery-resume snapshot enables resume at its recorded index; a
    // tampered snapshot (unknown schema, or a non-integer / negative / out-of-range
    // start index) is REFUSED so it cannot blind-replay or skip safety phases -- the
    // run then restarts fresh under the normal per-phase human gates.
    void workflowResumeSnapshotFailsClosedOnTamper() {
        QJsonArray history;
        history.append(QJsonObject{{QStringLiteral("phase_id"), QStringLiteral("p0")}});
        history.append(QJsonObject{{QStringLiteral("phase_id"), QStringLiteral("p1")}});
        const QString schema = QStringLiteral("sak.ai.workflow_recovery_resume.v1");
        const auto snapshot = [&](const QJsonValue& index, const QString& snapshot_schema) {
            return QJsonObject{{QStringLiteral("schema"), snapshot_schema},
                               {QStringLiteral("phase_history"), history},
                               {QStringLiteral("resume_start_phase_index"), index}};
        };

        ai::AiOrchestrationOptions ok_opts;
        AiAssistantPanel::applyWorkflowResumeState(&ok_opts, snapshot(2, schema));
        QVERIFY(ok_opts.resume_enabled);
        QCOMPARE(ok_opts.resume_start_phase_index, 2);
        QCOMPARE(ok_opts.resume_prior_phases.size(), 2);

        for (const QJsonValue& bad :
             {QJsonValue(5), QJsonValue(-1), QJsonValue(QStringLiteral("1"))}) {
            ai::AiOrchestrationOptions opts;
            AiAssistantPanel::applyWorkflowResumeState(&opts, snapshot(bad, schema));
            QVERIFY(!opts.resume_enabled);
        }

        ai::AiOrchestrationOptions wrong_schema;
        AiAssistantPanel::applyWorkflowResumeState(&wrong_schema,
                                                   snapshot(1, QStringLiteral("sak.ai.other.v9")));
        QVERIFY(!wrong_schema.resume_enabled);
    }

    // sak_app_action "list" returns the seeded built-in action catalog with risk
    // flags, so the assistant can enumerate the app's OWN technician actions headless.
    void appActionListReturnsSeededCatalogWithRiskFlags() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")},
                        {QStringLiteral("action_id"), QString()},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("operation")).toString(), QStringLiteral("list"));
        QVERIFY(result.value(QStringLiteral("action_count")).toInt() >= 7);

        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found_verify = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QStringLiteral("action.verify_system_files")) {
                found_verify = true;
                QVERIFY(action.value(QStringLiteral("requires_admin")).toBool());
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
            }
        }
        QVERIFY(found_verify);
    }

    // Running a mutating app action in a chat/research (no-execution) session is
    // refused by the per-action gate before the controller is ever touched.
    void appActionRunBlockedInChatResearchSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)

        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("action.verify_system_files")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});

        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // An unknown app action id is reported cleanly, never dispatched.
    void appActionRunRejectsUnknownId() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("action.does_not_exist")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("app_action_not_found"));
    }

    // W1a: the read-only technician ops are listed alongside the QuickActions with
    // read_only=true and mutating=false, so the per-action human gate is skipped.
    void readOnlyOpsAreListedAsReadOnly() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")},
                        {QStringLiteral("action_id"), QString()},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        // 7 built-in QuickActions + read-only + mutating ops (floor; grows as ops are added).
        QVERIFY(result.value(QStringLiteral("action_count")).toInt() >= 61);

        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        QSet<QString> read_only_ids;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("read_only")).toBool()) {
                QVERIFY(!action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                read_only_ids.insert(action.value(QStringLiteral("id")).toString());
            }
        }
        QVERIFY(read_only_ids.contains(QStringLiteral("partition.list_inventory")));
        QVERIFY(read_only_ids.contains(QStringLiteral("partition.preview_operation")));
        QVERIFY(read_only_ids.contains(QStringLiteral("network.list_adapters")));
        QVERIFY(read_only_ids.contains(QStringLiteral("network.dns_query")));
        QVERIFY(read_only_ids.contains(QStringLiteral("network.list_connections")));
        QVERIFY(read_only_ids.contains(QStringLiteral("network.audit_firewall")));
        QVERIFY(read_only_ids.contains(QStringLiteral("network.wifi_scan")));
        QVERIFY(read_only_ids.contains(QStringLiteral("network.list_wifi_profiles")));
        QVERIFY(read_only_ids.contains(QStringLiteral("network.generate_wifi_setup_script")));
        QVERIFY(read_only_ids.contains(QStringLiteral("network.ping")));
        QVERIFY(read_only_ids.contains(QStringLiteral("network.traceroute")));
        QVERIFY(read_only_ids.contains(QStringLiteral("network.mtr")));
        QVERIFY(read_only_ids.contains(QStringLiteral("network.port_scan")));
        QVERIFY(read_only_ids.contains(QStringLiteral("network.list_shares")));
        QVERIFY(read_only_ids.contains(QStringLiteral("security.list_installed_programs")));
        QVERIFY(read_only_ids.contains(QStringLiteral("software.scan_leftovers")));
        QVERIFY(read_only_ids.contains(QStringLiteral("security.scan_vulnerabilities")));
        QVERIFY(read_only_ids.contains(QStringLiteral("imaging.identify_image")));
        QVERIFY(read_only_ids.contains(QStringLiteral("imaging.analyze_iso")));
        QVERIFY(read_only_ids.contains(QStringLiteral("imaging.scan_recoverable")));
        QVERIFY(read_only_ids.contains(QStringLiteral("imaging.list_drives")));
        QVERIFY(read_only_ids.contains(QStringLiteral("search.find_in_files")));
        QVERIFY(read_only_ids.contains(QStringLiteral("organizer.find_duplicates")));
        QVERIFY(read_only_ids.contains(QStringLiteral("organizer.preview_organize")));
        QVERIFY(read_only_ids.contains(QStringLiteral("files.list_archive")));
        QVERIFY(read_only_ids.contains(QStringLiteral("files.scan_directory")));
        QVERIFY(read_only_ids.contains(QStringLiteral("files.hash_file")));
        QVERIFY(read_only_ids.contains(QStringLiteral("backup.preview_app_data")));
        QVERIFY(read_only_ids.contains(QStringLiteral("diagnostics.hardware_scan")));
        QVERIFY(read_only_ids.contains(QStringLiteral("diagnostics.smart_scan")));
        QVERIFY(read_only_ids.contains(QStringLiteral("diagnostics.list_restore_points")));
        QVERIFY(read_only_ids.contains(QStringLiteral("diagnostics.read_temperatures")));
        QVERIFY(read_only_ids.contains(QStringLiteral("system.list_users")));
        QVERIFY(read_only_ids.contains(QStringLiteral("email.read_mbox")));
        QVERIFY(read_only_ids.contains(QStringLiteral("email.search_mbox")));
        QVERIFY(read_only_ids.contains(QStringLiteral("email.read_pst")));
        QVERIFY(read_only_ids.contains(QStringLiteral("email.search_pst")));
        QVERIFY(read_only_ids.contains(QStringLiteral("email.recover_deleted")));
        QVERIFY(read_only_ids.contains(QStringLiteral("diagnostics.run_benchmark")));
        QVERIFY(read_only_ids.contains(QStringLiteral("email.list_profiles")));
    }

    // W1a: identify_image drives the app's FileImageSource detection on a real
    // file -- deterministic, reads no system or network state.
    void identifyImageDetectsGzip() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("sample.img.gz"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const QByteArray gz = QByteArray::fromHex("1f8b0800000000000003") +
                                  QByteArray(64, '\0');
            QCOMPARE(file.write(gz), static_cast<qint64>(gz.size()));
        }

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.identify_image")},
            {QStringLiteral("arguments"), QStringLiteral("{\"path\":\"%1\"}").arg(path)}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("format")).toString(), QStringLiteral("gzip"));
        QVERIFY(data.value(QStringLiteral("is_compressed")).toBool());
        QVERIFY(data.value(QStringLiteral("extension_recognized")).toBool());
        QCOMPARE(data.value(QStringLiteral("detection")).toString(), QStringLiteral("extension"));
        QCOMPARE(static_cast<qint64>(data.value(QStringLiteral("size_bytes")).toDouble()),
                 static_cast<qint64>(74));
    }

    // imaging.analyze_iso: build a minimal but REAL ISO 9660 image (system area + a Primary
    // Volume Descriptor at LBA 16 carrying a volume label) and assert the app's IsoAnalyzer reads
    // it back through the op. Deterministic, reads no system/network state.
    void analyzeIsoDetectsMinimalVolume() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("mini.iso"));
        {
            QByteArray iso(18 * 2048, '\0');  // through LBA 17 so PVD + boot-record reads are safe
            const int pvd = 16 * 2048;
            iso[pvd + 0] = 0x01;              // Primary Volume Descriptor type
            std::memcpy(iso.data() + pvd + 1, "CD001", 5);
            iso[pvd + 6] = 0x01;              // version
            const char* label = "SAK_TEST_ISO";
            std::memset(iso.data() + pvd + 40, ' ', 32);  // 32-byte volume-id field, space-padded
            std::memcpy(iso.data() + pvd + 40, label, std::strlen(label));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(iso), static_cast<qint64>(iso.size()));
        }

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.analyze_iso")},
            {QStringLiteral("arguments"), QStringLiteral("{\"path\":\"%1\"}").arg(path)}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(data.value(QStringLiteral("is_valid")).toBool());
        QCOMPARE(data.value(QStringLiteral("volume_label")).toString(),
                 QStringLiteral("SAK_TEST_ISO"));
        QCOMPARE(data.value(QStringLiteral("filesystem")).toString(), QStringLiteral("ISO 9660"));
    }

    // imaging.analyze_iso: a readable file that is NOT ISO media is an honest is_valid=false
    // SUCCESS (the read worked; the content simply is not an ISO) -- never a failed-read
    // masquerade.
    void analyzeIsoNonIsoFileReportsInvalid() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("not_an.iso"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray(40 * 1024, 'X')), static_cast<qint64>(40 * 1024));
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.analyze_iso")},
            {QStringLiteral("arguments"), QStringLiteral("{\"path\":\"%1\"}").arg(path)}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("data"))
                     .toObject()
                     .value(QStringLiteral("is_valid"))
                     .toBool(),
                 false);
    }

    // imaging.analyze_iso: a missing path fails cleanly (IsoAnalyzer's exists() precondition is
    // guaranteed by the op, never violated).
    void analyzeIsoMissingFileFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("imaging.analyze_iso")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"path\":\"C:/nonexistent/no_such.iso\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
    }

    // imaging.analyze_iso: refuse a UNC/network path -- reading it would pull over SMB and could
    // leak credentials (same guard as find_in_files/read_mbox).
    void analyzeIsoRefusesUncPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("imaging.analyze_iso")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"path\":\"\\\\\\\\server\\\\share\\\\x.iso\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("network"), Qt::CaseInsensitive));
    }

    // diagnostics.run_benchmark: the op-layer routes on 'target'. An unknown target (e.g. the
    // deliberately-unexposed "disk" suite, which would write a test file) is rejected cleanly by
    // the thunk itself -- there is no schema pre-validation, so this certs MY routing/validation
    // branch, not a framework reject. The successful cpu/memory runs are timing-sensitive and
    // covered by the dedicated benchmark-worker unit tests (not re-run here).
    void runBenchmarkRejectsUnknownTarget() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("diagnostics.run_benchmark")},
                        {QStringLiteral("arguments"), QStringLiteral("{\"target\":\"disk\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        const QString message = result.value(QStringLiteral("message")).toString();
        QVERIFY(message.contains(QStringLiteral("cpu"), Qt::CaseInsensitive));
        QVERIFY(message.contains(QStringLiteral("memory"), Qt::CaseInsensitive));
    }

    // diagnostics.run_benchmark: the descriptor is registered read-only with a 'target' enum
    // schema (cpu/memory only) and no admin/mutating flags -- so it is allowed in every access
    // mode and never triggers the human gate.
    void runBenchmarkDescriptorIsReadOnly() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject listing = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")},
                        {QStringLiteral("action_id"), QString()},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        bool found = false;
        for (const QJsonValue& value : listing.value(QStringLiteral("actions")).toArray()) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() !=
                QStringLiteral("diagnostics.run_benchmark")) {
                continue;
            }
            found = true;
            QVERIFY(action.value(QStringLiteral("read_only")).toBool());
            QVERIFY(!action.value(QStringLiteral("mutating")).toBool());
            QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
            QVERIFY(!action.value(QStringLiteral("requires_admin")).toBool());
            const QJsonObject schema = action.value(QStringLiteral("params")).toObject();
            const QJsonArray enum_vals = schema.value(QStringLiteral("properties"))
                                             .toObject()
                                             .value(QStringLiteral("target"))
                                             .toObject()
                                             .value(QStringLiteral("enum"))
                                             .toArray();
            QCOMPARE(enum_vals.size(), 2);
        }
        QVERIFY(found);
    }

    // email.list_profiles: env-dependent enumeration (the real host's mail clients), so assert
    // only shape-level invariants that hold whether or not any client is installed -- an honest
    // success with a bounded profiles array and reported_count <= profile_count. Metadata only:
    // no mailbox contents, no secret export.
    void listEmailProfilesReportsShape() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.list_profiles")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(data.contains(QStringLiteral("profile_count")));
        // The test host reads its (possibly empty) mail config cleanly, so discovery is reliable
        // -- proves the honesty channel reports a complete read, not a masked failure.
        QVERIFY(data.value(QStringLiteral("discovery_complete")).toBool());
        const int profile_count = data.value(QStringLiteral("profile_count")).toInt(-1);
        QVERIFY(profile_count >= 0);
        const QJsonArray profiles = data.value(QStringLiteral("profiles")).toArray();
        QVERIFY(profiles.size() <= profile_count);
        QVERIFY(data.value(QStringLiteral("reported_count")).toInt() == profiles.size());
        for (const QJsonValue& value : profiles) {
            const QJsonObject profile = value.toObject();
            QVERIFY(profile.contains(QStringLiteral("client_type")));
            QVERIFY(profile.contains(QStringLiteral("data_files")));
        }
    }

    // W1a: a read-only op runs even in a chat/research session -- the per-action
    // gate is skipped because it changes nothing (never policy_blocked).
    void readOnlyOpRunsInChatResearchSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("blank.img"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray(512, '\0')), static_cast<qint64>(512));
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.identify_image")},
            {QStringLiteral("arguments"), QStringLiteral("{\"path\":\"%1\"}").arg(path)}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QVERIFY(!result.contains(QStringLiteral("failure_class")));
    }

    // W1a: identify_image with no path fails cleanly (never dereferences).
    void identifyImageMissingPathFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("imaging.identify_image")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("path")));
    }

    // identify_image now rejects a network/UNC path (it previously had NO UNC guard, so a bare
    // exists()/isFile() on a UNC path would have leaked the NTLM hash).
    void identifyImageRefusesNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args = QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{QStringLiteral("path"), QStringLiteral("\\\\host\\share\\x.iso")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("imaging.identify_image")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // The reparse-point guard (endemic sweep): a model-supplied path that is a symlink/junction is
    // refused BEFORE any following stat, so a link whose target is a UNC share cannot leak the NTLM
    // hash. A Windows .lnk shortcut (creatable unprivileged) is reported by QFileInfo::isSymLink(),
    // so it exercises the same guard as a real NTFS symlink. Covers a read-only op (read_mbox) and
    // a mutating op (compress_zip source) through the shared refuseUnsafePath / pathIsReparsePoint.
    void readMboxRefusesSymlinkPath() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString real = dir.filePath(QStringLiteral("real.mbox"));
        writeSampleMbox(real);
        const QString link = dir.filePath(QStringLiteral("link.mbox.lnk"));
        if (!QFile::link(real, link) || !QFileInfo(link).isSymLink()) {
            QSKIP("Could not create a symlink/shortcut on this filesystem");
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("path"), link}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.read_mbox")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("symlink"), Qt::CaseInsensitive));
    }

    void compressZipRefusesSymlinkSource() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString real = dir.filePath(QStringLiteral("real.txt"));
        writeTextFile(real, QByteArrayLiteral("x"));
        const QString link = dir.filePath(QStringLiteral("link.txt.lnk"));
        if (!QFile::link(real, link) || !QFileInfo(link).isSymLink()) {
            QSKIP("Could not create a symlink/shortcut on this filesystem");
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = runArchiveOp(
            panel,
            QStringLiteral("files.compress_zip"),
            QJsonObject{{QStringLiteral("sources"), QJsonArray{link}},
                        {QStringLiteral("output_path"), dir.filePath(QStringLiteral("o.zip"))}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("symlink"), Qt::CaseInsensitive));
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // a guard, not the gate
    }

    // W1b: search.find_in_files drives the real AdvancedSearchController + worker
    // (async, on its own thread) through the AsyncActionInvocation bridge, end to
    // end and deterministically: a temp tree with a known token yields exactly the
    // expected matches, proving completion is delivered onto the worker thread and
    // drained by the local loop.
    void searchFindsMatchesInTempDir() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto write = [&](const QString& name, const QByteArray& content) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(content), static_cast<qint64>(content.size()));
        };
        write(QStringLiteral("a.txt"), "hello NEEDLE world\nsecond line\n");
        write(QStringLiteral("b.txt"), "no match here\nanother NEEDLE token\n");
        write(QStringLiteral("c.txt"), "totally unrelated content\n");

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args =
            QStringLiteral("{\"root_path\":\"%1\",\"pattern\":\"NEEDLE\"}").arg(dir.path());
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("search.find_in_files")},
                        {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("total_matches")).toInt(), 2);
        const QJsonArray matches = data.value(QStringLiteral("matches")).toArray();
        QCOMPARE(matches.size(), 2);
        for (const auto& value : matches) {
            QVERIFY(value.toObject()
                        .value(QStringLiteral("line_content"))
                        .toString()
                        .contains(QStringLiteral("NEEDLE")));
        }
    }

    // W1b: an ungated read-only search must refuse UNC/network roots, so prompt
    // injection cannot steer it into an SMB/NTLM handshake with an attacker host.
    void searchRejectsNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        // Windows treats ANY two leading separators as a UNC/device root: cover
        // homogeneous (\\, //) and mixed (\/, /\) forms -- all must be refused.
        const QStringList roots = {QStringLiteral("\\\\attacker.example.com\\share"),
                                   QStringLiteral("\\/attacker.example.com\\share"),
                                   QStringLiteral("/\\attacker.example.com/share"),
                                   QStringLiteral("//attacker.example.com/share")};
        for (const QString& root : roots) {
            const QString args = QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("root_path"), root},
                                          {QStringLiteral("pattern"), QStringLiteral("x")}})
                    .toJson(QJsonDocument::Compact));
            const QJsonObject result = panel.runAppActionTool(
                QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                            {QStringLiteral("action_id"), QStringLiteral("search.find_in_files")},
                            {QStringLiteral("arguments"), args}});
            QVERIFY2(!result.value(QStringLiteral("success")).toBool(), qPrintable(root));
            QVERIFY2(result.value(QStringLiteral("message"))
                         .toString()
                         .contains(QStringLiteral("network")),
                     qPrintable(root));
        }
    }

    // W1b: a search missing a required argument fails cleanly (never starts a worker).
    void searchMissingArgsFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("search.find_in_files")},
                        {QStringLiteral("arguments"), QStringLiteral("{\"pattern\":\"x\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("root_path")));
    }

    // organizer.find_duplicates drives the real DuplicateFinderWorker (a QThread) through the
    // AsyncActionInvocation bridge, end to end + deterministically: a temp dir with two byte-
    // identical files plus one unique file yields exactly one duplicate group of two files.
    void findDuplicatesDetectsIdenticalFiles() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto write = [&](const QString& name, const QByteArray& content) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(content), static_cast<qint64>(content.size()));
        };
        write(QStringLiteral("one.bin"), "IDENTICAL DUPLICATE PAYLOAD 12345");
        write(QStringLiteral("copy.bin"), "IDENTICAL DUPLICATE PAYLOAD 12345");
        write(QStringLiteral("unique.bin"), "a totally different payload");

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("directories"), QJsonArray{dir.path()}}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("organizer.find_duplicates")},
                        {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("total_groups")).toInt(), 1);
        QCOMPARE(data.value(QStringLiteral("total_duplicate_files")).toInt(), 1);
        const QJsonArray groups = data.value(QStringLiteral("groups")).toArray();
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.at(0).toObject().value(QStringLiteral("file_count")).toInt(), 2);
    }

    // organizer.find_duplicates is read-only: it runs (never policy_blocked) even in a
    // chat/research session. An empty temp dir yields an honest zero-duplicate success.
    void findDuplicatesRunsUngatedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("directories"), QJsonArray{dir.path()}}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("organizer.find_duplicates")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("data"))
                     .toObject()
                     .value(QStringLiteral("total_groups"))
                     .toInt(),
                 0);
    }

    // organizer.find_duplicates refuses UNC/network/device roots (the scan HASHES file contents, so
    // a network root would also pull data over the wire) -- same guard as search.find_in_files.
    void findDuplicatesRejectsNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QStringList roots = {QStringLiteral("\\\\attacker.example.com\\share"),
                                   QStringLiteral("//attacker.example.com/share")};
        for (const QString& root : roots) {
            const QString args = QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("directories"), QJsonArray{root}}})
                    .toJson(QJsonDocument::Compact));
            const QJsonObject result = panel.runAppActionTool(QJsonObject{
                {QStringLiteral("operation"), QStringLiteral("run")},
                {QStringLiteral("action_id"), QStringLiteral("organizer.find_duplicates")},
                {QStringLiteral("arguments"), args}});
            QVERIFY2(!result.value(QStringLiteral("success")).toBool(), qPrintable(root));
            QVERIFY2(result.value(QStringLiteral("message"))
                         .toString()
                         .contains(QStringLiteral("network")),
                     qPrintable(root));
        }
    }

    // organizer.find_duplicates with no directories fails cleanly (never starts a worker).
    void findDuplicatesMissingDirsFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("organizer.find_duplicates")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("directories")));
    }

    // organizer.find_duplicates FAILS CLOSED on a nonexistent directory rather than letting the
    // worker silently skip it and return a dishonest "0 duplicates" success (the fail-open honesty
    // rule). Deterministic: a path under a temp dir that was never created cannot exist.
    void findDuplicatesRejectsMissingDirectory() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString missing = dir.filePath(QStringLiteral("no_such_subdir_zzz"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("directories"), QJsonArray{missing}}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("organizer.find_duplicates")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("existing directory")));
    }

    // organizer.preview_organize drives the SAME OrganizerWorker in preview_mode through the
    // AsyncActionInvocation bridge and end to end: three files across two categories yield an
    // accurate per-category plan, and -- the whole point of a dry run -- NOT ONE file is moved and
    // no category subfolder is created.
    void previewOrganizePlansWithoutMoving() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto write = [&](const QString& name) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("x"), static_cast<qint64>(1));
        };
        write(QStringLiteral("a.jpg"));
        write(QStringLiteral("b.png"));
        write(QStringLiteral("c.txt"));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject mapping{{QStringLiteral("Images"),
                                   QJsonArray{QStringLiteral("jpg"), QStringLiteral("png")}},
                                  {QStringLiteral("Docs"), QJsonArray{QStringLiteral("txt")}}};
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("target_directory"), dir.path()},
                                      {QStringLiteral("category_mapping"), mapping}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("organizer.preview_organize")},
                        {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("files_to_move")).toInt(), 3);
        QCOMPARE(data.value(QStringLiteral("category_count")).toInt(), 2);
        QCOMPARE(data.value(QStringLiteral("collisions")).toInt(), 0);
        QVERIFY(!data.value(QStringLiteral("plan_truncated")).toBool());  // 3 files, well under cap
        const QJsonObject categories = data.value(QStringLiteral("categories")).toObject();
        QCOMPARE(categories.value(QStringLiteral("Images")).toInt(), 2);
        QCOMPARE(categories.value(QStringLiteral("Docs")).toInt(), 1);

        // A dry run must not touch the filesystem: every source file stays put and no category
        // subfolder was created.
        QVERIFY(QFileInfo(dir.filePath(QStringLiteral("a.jpg"))).isFile());
        QVERIFY(QFileInfo(dir.filePath(QStringLiteral("b.png"))).isFile());
        QVERIFY(QFileInfo(dir.filePath(QStringLiteral("c.txt"))).isFile());
        QVERIFY(!QFileInfo(dir.filePath(QStringLiteral("Images"))).exists());
        QVERIFY(!QFileInfo(dir.filePath(QStringLiteral("Docs"))).exists());
    }

    // organizer.preview_organize is read-only: it runs (never policy_blocked) even in a
    // chat/research session, since it moves nothing.
    void previewOrganizeRunsUngatedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject mapping{{QStringLiteral("Docs"), QJsonArray{QStringLiteral("txt")}}};
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("target_directory"), dir.path()},
                                      {QStringLiteral("category_mapping"), mapping}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("organizer.preview_organize")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("data"))
                     .toObject()
                     .value(QStringLiteral("files_to_move"))
                     .toInt(),
                 0);
    }

    // organizer.preview_organize refuses UNC/network/device targets before any stat (a symlink or
    // literal UNC target would leak the NTLM hash on the directory scan) -- same guard as the
    // apply.
    void previewOrganizeRejectsNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject mapping{{QStringLiteral("Docs"), QJsonArray{QStringLiteral("txt")}}};
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("target_directory"),
                                       QStringLiteral("\\\\attacker.example.com\\share")},
                                      {QStringLiteral("category_mapping"), mapping}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("organizer.preview_organize")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // software.scan_leftovers with no program_name fails cleanly (never resolves/scans).
    void scanLeftoversMissingNameFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("software.scan_leftovers")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("program_name")));
    }

    // software.scan_leftovers is read-only: even in a chat/research session it RUNS (never
    // policy_blocked) and, for a program that does not exist, returns an honest resolution failure
    // rather than a masked success. Deterministic: a random GUID-like name is never an installed
    // program's exact display name.
    void scanLeftoversUnknownProgramRunsUngatedAndFailsHonestly() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("program_name"),
                                       QStringLiteral("NoSuchProgram_5f3c1a9e-dead-beef")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("software.scan_leftovers")},
                        {QStringLiteral("arguments"), args}});
        // Ran (not policy_blocked) and honestly reports the program was not found.
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        const QString message = result.value(QStringLiteral("message")).toString();
        QVERIFY2(!message.contains(QStringLiteral("policy")), qPrintable(message));
        QVERIFY2(message.contains(QStringLiteral("No installed program")) ||
                     message.contains(QStringLiteral("installed-programs registry")),
                 qPrintable(message));
    }

    // software.scan_leftovers advanced=true drives an ADVANCED-level scan of a REAL, uniquely-named
    // installed program (resolve refuses ambiguous display names) and emits the honesty fields the
    // fail-open fix added. Asserts what is deterministically observable: the scan ran (success),
    // the Advanced level was actually selected (scan_level=="advanced" -- this FAILS if the
    // advanced param wiring is reverted, so it is the real regression guard), and the completeness
    // fields are present with the right shape (failed_system_phases is an array, scan_complete is a
    // bool).
    //
    // NOT asserted: that a FAILED phase yields a non-empty failed_system_phases +
    // scan_complete=false
    // -- that mapping cannot be triggered headless (sc/schtasks/netsh and the Run-key reads all
    // succeed on a healthy host), and scan_complete/failed_system_phases are both derived from one
    // internal variable so cross-checking them here would be a tautology. The failure mapping's
    // correctness rests on code review + the engine's per-phase ok flags; the coverage gap is
    // tracked in memory/assistant-dominion-deferred.md. QSKIP where nothing enumerable/unique.
    void scanLeftoversAdvancedRunsAtAdvancedLevel() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject listed = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("security.list_installed_programs")},
            {QStringLiteral("arguments"), QStringLiteral("{}")}});
        if (!listed.value(QStringLiteral("success")).toBool()) {
            QSKIP("could not enumerate installed programs on this host");
        }
        const QJsonArray programs = listed.value(QStringLiteral("data"))
                                        .toObject()
                                        .value(QStringLiteral("programs"))
                                        .toArray();
        QMap<QString, int> counts;
        for (const auto& value : programs) {
            counts[value.toObject().value(QStringLiteral("name")).toString()]++;
        }
        QString unique;
        for (const auto& value : programs) {
            const QString candidate = value.toObject().value(QStringLiteral("name")).toString();
            if (!candidate.isEmpty() && counts.value(candidate) == 1) {
                unique = candidate;
                break;
            }
        }
        if (unique.isEmpty()) {
            QSKIP("no uniquely-named installed program to scan");
        }

        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("program_name"), unique},
                                                        {QStringLiteral("advanced"), true}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("software.scan_leftovers")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY2(result.value(QStringLiteral("success")).toBool(),
                 qPrintable(result.value(QStringLiteral("message")).toString()));
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("scan_level")).toString(), QStringLiteral("advanced"));
        QVERIFY(data.value(QStringLiteral("failed_system_phases")).isArray());
        QVERIFY(data.value(QStringLiteral("scan_complete")).isBool());
    }

    // imaging.scan_recoverable with no image_path fails cleanly (never opens/scans).
    void scanRecoverableMissingImageFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("imaging.scan_recoverable")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("image_path")));
    }

    // imaging.scan_recoverable drives the real FileRecoveryEngine end to end + deterministically:
    // an image blob with an embedded JPEG (SOI ... EOI) between junk yields exactly one carved
    // candidate. It is read-only (runs in a chat/research session) and returns METADATA ONLY -- no
    // recovered file content leaves the process.
    void scanRecoverableCarvesEmbeddedJpeg() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString imgPath = dir.filePath(QStringLiteral("disk.img"));
        {
            QFile file(imgPath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QByteArray blob;
            blob.append(QByteArray(64, '\0'));                         // leading junk
            blob.append(QByteArray::fromHex("ffd8ffe000104a464946"));  // JPEG SOI + APP0 "JFIF"
            blob.append(QByteArray(128, 'x'));                         // body
            blob.append(QByteArray::fromHex("ffd9"));                  // JPEG EOI
            blob.append(QByteArray(64, '\0'));                         // trailing junk
            QCOMPARE(file.write(blob), static_cast<qint64>(blob.size()));
        }

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (read-only runs ungated)
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("image_path"), imgPath}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("imaging.scan_recoverable")},
                        {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(data.value(QStringLiteral("source_read_only")).toBool());
        // The whole small image is read, so coverage is complete (honest scan_complete flag).
        QVERIFY(data.value(QStringLiteral("scan_complete")).toBool());
        QVERIFY(data.value(QStringLiteral("candidate_count")).toInt() >= 1);
        const QJsonArray candidates = data.value(QStringLiteral("candidates")).toArray();
        QVERIFY(!candidates.isEmpty());
        const QJsonObject first = candidates.at(0).toObject();
        QCOMPARE(first.value(QStringLiteral("extension")).toString(), QStringLiteral("jpg"));
        // Metadata only: never a recovered-content field.
        QVERIFY(!first.contains(QStringLiteral("recovered_bytes")));
        QVERIFY(!first.contains(QStringLiteral("content")));
    }

    // imaging.scan_recoverable refuses UNC/network/device image paths before opening (a symlink or
    // literal UNC target would leak the NTLM hash on the read-only open).
    void scanRecoverableRejectsNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args = QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{QStringLiteral("image_path"),
                             QStringLiteral("\\\\attacker.example.com\\share\\disk.img")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("imaging.scan_recoverable")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // imaging.list_drives drives the app's OWN DriveScanner::enumerateDrivesOnce end to end. Every
    // real host has at least PhysicalDrive0, so a healthy run reports >=1 drive with the flag set a
    // technician needs (removable/read-only) and metadata only -- no drive data. Env-dependent: a
    // locked-down sandbox that cannot open a physical drive even for metadata reports an HONEST
    // failure (never a misleading "0 drives" success), which this test accepts via QSKIP.
    void listDrivesReportsPhysicalDrives() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (read-only runs ungated)
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("imaging.list_drives")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});

        if (!result.value(QStringLiteral("success")).toBool()) {
            // Honest empty-read failure: message names the enumeration failure, not a silent empty.
            QVERIFY(result.value(QStringLiteral("message"))
                        .toString()
                        .contains(QStringLiteral("enumerate")));
            QSKIP("host does not permit physical-drive metadata enumeration");
        }
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(data.value(QStringLiteral("drive_count")).toInt() >= 1);
        QVERIFY(data.value(QStringLiteral("reported_count")).toInt() >= 1);
        QVERIFY(data.value(QStringLiteral("enumeration_complete")).isBool());
        QCOMPARE(data.value(QStringLiteral("filtered_removable_only")).toBool(), false);
        const int removable = data.value(QStringLiteral("removable_count")).toInt();
        QVERIFY(removable >= 0);
        QVERIFY(removable <= data.value(QStringLiteral("drive_count")).toInt());
        const QJsonArray drives = data.value(QStringLiteral("drives")).toArray();
        QVERIFY(!drives.isEmpty());
        const QJsonObject first = drives.at(0).toObject();
        QVERIFY(first.value(QStringLiteral("device_path"))
                    .toString()
                    .contains(QStringLiteral("PhysicalDrive")));
        QVERIFY(first.contains(QStringLiteral("size_bytes")));
        QVERIFY(first.contains(QStringLiteral("is_removable")));
        QVERIFY(first.contains(QStringLiteral("is_read_only")));
        QVERIFY(first.contains(QStringLiteral("bus_type")));
        QVERIFY(first.value(QStringLiteral("mount_points")).isArray());
        // Metadata only: no drive-content field ever leaves the op.
        QVERIFY(!first.contains(QStringLiteral("data")));
        QVERIFY(!first.contains(QStringLiteral("bytes")));
    }

    // removable_only really filters (non-vacuous): if the filter were a no-op the removable listing
    // would report drive_count entries, so reported_count == removable_count fails whenever a fixed
    // (non-removable) system disk exists -- true on every normal host. Totals stay accurate under
    // the filter, and every listed drive is removable.
    void listDrivesRemovableOnlyReallyFilters() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);

        const auto runList = [&panel](bool removable_only) {
            const QString args = QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("removable_only"), removable_only}})
                    .toJson(QJsonDocument::Compact));
            return panel.runAppActionTool(
                QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                            {QStringLiteral("action_id"), QStringLiteral("imaging.list_drives")},
                            {QStringLiteral("arguments"), args}});
        };

        const QJsonObject all = runList(false);
        if (!all.value(QStringLiteral("success")).toBool()) {
            QSKIP("host does not permit physical-drive metadata enumeration");
        }
        const QJsonObject allData = all.value(QStringLiteral("data")).toObject();
        const int drive_count = allData.value(QStringLiteral("drive_count")).toInt();
        const int removable_count = allData.value(QStringLiteral("removable_count")).toInt();
        // Unfiltered listing reports the full set (drives are always well under the cap).
        QCOMPARE(allData.value(QStringLiteral("reported_count")).toInt(), drive_count);

        const QJsonObject rem = runList(true);
        QVERIFY(rem.value(QStringLiteral("success")).toBool());
        const QJsonObject remData = rem.value(QStringLiteral("data")).toObject();
        QVERIFY(remData.value(QStringLiteral("filtered_removable_only")).toBool());
        // Totals are unchanged by the filter; only the surfaced list narrows to removable drives.
        QCOMPARE(remData.value(QStringLiteral("drive_count")).toInt(), drive_count);
        QCOMPARE(remData.value(QStringLiteral("removable_count")).toInt(), removable_count);
        QCOMPARE(remData.value(QStringLiteral("reported_count")).toInt(), removable_count);
        const QJsonArray remDrives = remData.value(QStringLiteral("drives")).toArray();
        for (const QJsonValue& value : remDrives) {
            QVERIFY(value.toObject().value(QStringLiteral("is_removable")).toBool());
        }
    }

    // files.hash_file drives the app's OWN file_hasher end to end + deterministically: the file
    // content "abc" has the well-known SHA-256 ba7816bf...15ad. Read-only, returns the hex digest
    // + size only.
    void hashFileComputesKnownSha256() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString filePath = dir.filePath(QStringLiteral("data.bin"));
        {
            QFile file(filePath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("abc", 3), static_cast<qint64>(3));
        }

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (read-only runs ungated)
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("path"), filePath}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("files.hash_file")},
                        {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("algorithm")).toString(), QStringLiteral("sha256"));
        QCOMPARE(data.value(QStringLiteral("hash")).toString(),
                 QStringLiteral(
                     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QCOMPARE(data.value(QStringLiteral("size_bytes")).toDouble(), 3.0);
    }

    // The algorithm arg selects MD5; MD5("abc") = 900150983cd24fb0d6963f7d28e17f72.
    void hashFileMd5Selectable() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString filePath = dir.filePath(QStringLiteral("data.bin"));
        {
            QFile file(filePath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("abc", 3), static_cast<qint64>(3));
        }

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"), filePath},
                                      {QStringLiteral("algorithm"), QStringLiteral("md5")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("files.hash_file")},
                        {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("algorithm")).toString(), QStringLiteral("md5"));
        QCOMPARE(data.value(QStringLiteral("hash")).toString(),
                 QStringLiteral("900150983cd24fb0d6963f7d28e17f72"));
    }

    // Missing path fails cleanly (never hashes).
    void hashFileMissingPathFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("files.hash_file")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("path")));
    }

    // An unknown algorithm is rejected before any file open.
    void hashFileRejectsBadAlgorithm() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString filePath = dir.filePath(QStringLiteral("data.bin"));
        {
            QFile file(filePath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("abc", 3), static_cast<qint64>(3));
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"), filePath},
                                      {QStringLiteral("algorithm"), QStringLiteral("sha1")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("files.hash_file")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("algorithm")));
    }

    // files.hash_file refuses UNC/network/device paths before opening (a symlink or literal UNC
    // target would leak the NTLM hash on the read-only open) -- the shared refuseUnsafePath guard.
    void hashFileRejectsNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args = QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{QStringLiteral("path"),
                             QStringLiteral("\\\\attacker.example.com\\share\\secret.bin")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("files.hash_file")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // R1 sweep: the shared path guards now reject a path whose ANCESTOR directory is a reparse
    // point (symlink/junction), not just the leaf -- an ancestor junction to a UNC target would
    // otherwise leak the NTLM hash on the following stat. Certified through refuseUnsafePath
    // (imaging.scan_recoverable) since every file op shares pathReparseUnsafe. Deterministic on
    // Windows via an unprivileged directory junction (mklink /J).
    //
    // Scope: this proves the guard REFUSES the ancestor-junction path (the functional wiring). It
    // cannot observe the security-critical property that NO stat traversed the ancestor (that needs
    // a UNC target + network observation, infeasible headless), so a LOCAL junction is used. The
    // no-leak property rests on pathReparseUnsafe running the ancestor walk BEFORE the leaf stat
    // (ancestor-first operand order), enforced + documented in app_action_guards.h -- this test
    // certifies the observable half.
    void refuseUnsafePathRejectsAncestorJunction() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString realTarget = dir.filePath(QStringLiteral("real"));
        QVERIFY(QDir().mkpath(realTarget));
        {
            QFile file(realTarget + QStringLiteral("/disk.img"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("x"), static_cast<qint64>(1));
        }

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const auto runScan = [&](const QString& imagePath) {
            const QString args = QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("image_path"), imagePath}})
                    .toJson(QJsonDocument::Compact));
            return panel.runAppActionTool(QJsonObject{
                {QStringLiteral("operation"), QStringLiteral("run")},
                {QStringLiteral("action_id"), QStringLiteral("imaging.scan_recoverable")},
                {QStringLiteral("arguments"), args}});
        };

        // Control: the real path (no reparse ancestor) is accepted -- confirms the temp tree itself
        // is clean, so a refusal below is attributable to the junction, not the environment.
        const QJsonObject control = runScan(realTarget + QStringLiteral("/disk.img"));
        QVERIFY2(control.value(QStringLiteral("success")).toBool(),
                 qPrintable(control.value(QStringLiteral("message")).toString()));

        // Create a directory junction (unprivileged) and scan an image THROUGH it.
        const QString junction = dir.filePath(QStringLiteral("jlink"));
        QProcess mklink;
        mklink.start(QStringLiteral("cmd"),
                     {QStringLiteral("/c"),
                      QStringLiteral("mklink"),
                      QStringLiteral("/J"),
                      QDir::toNativeSeparators(junction),
                      QDir::toNativeSeparators(realTarget)});
        if (!mklink.waitForFinished(10'000) || mklink.exitCode() != 0 ||
            !QFileInfo(junction).isJunction()) {
            QSKIP("could not create a directory junction on this host");
        }

        const QJsonObject viaJunction = runScan(junction + QStringLiteral("/disk.img"));
        QVERIFY(!viaJunction.value(QStringLiteral("success")).toBool());
        const QString message = viaJunction.value(QStringLiteral("message")).toString();
        QVERIFY2(message.contains(QStringLiteral("junction")) ||
                     message.contains(QStringLiteral("symlink")) ||
                     message.contains(QStringLiteral("path")),
                 qPrintable(message));
    }

    // R2 sweep: a symlink FILE entry discovered INSIDE a scanned tree must be skipped by all three
    // walk ops (find_duplicates hashes, find_in_files opens, scan_directory sizes) via a
    // non-following check, so a planted symlink to a UNC target is never resolved (the following
    // stat/open would leak the credential hash). Deterministic: a temp tree with two byte-identical
    // real files plus a symlink to one of them. Each op must see exactly the TWO real files and
    // EXCLUDE the symlink -- proving the skip, not mere absence (the symlink content is identical,
    // so an unskipped walk would fold it into the results).
    //
    // COVERAGE NOTE: this is the ONLY test that exercises the walk-time symlink skip for
    // find_duplicates and find_in_files, and it needs a real NTFS FILE symlink (mklink, requires
    // SeCreateSymbolicLinkPrivilege / Developer Mode). It QSKIPs where unavailable, same posture as
    // the ancestor-junction cert. The unprivileged junction cert below CANNOT substitute for those
    // two engines: a junction is a DIRECTORY, so DuplicateFinderWorker drops it at
    // is_regular_file() and AdvancedSearchWorker (QDir::Files, no FollowSymlinks) never lists or
    // descends it -- neither ever reaches the file-symlink skip. So on a non-privileged host the
    // find_duplicates/ find_in_files skip is UNCOVERED (a structural Windows limitation, tracked in
    // memory/assistant-dominion-deferred.md); run the cert suite elevated or under Dev Mode for
    // full R2 coverage.
    // Assert the three walk ops all EXCLUDE the planted symlink from a tree of two byte-identical
    // real files: find_duplicates sees ONE group of TWO (not three), find_in_files sees TWO NEEDLE
    // matches (not three), scan_directory counts TWO files and never lists link.txt. Extracted from
    // the slot to keep each function within the complexity budget.
    static void verifySymlinkExcludedFromWalkOps(AiAssistantPanel& panel, const QString& dirPath) {
        const auto run = [&](const QString& actionId, const QJsonObject& arguments) {
            const QString args =
                QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
            return panel.runAppActionTool(
                QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                            {QStringLiteral("action_id"), actionId},
                            {QStringLiteral("arguments"), args}});
        };

        const QJsonObject dup =
            run(QStringLiteral("organizer.find_duplicates"),
                QJsonObject{{QStringLiteral("directories"), QJsonArray{dirPath}}});
        QVERIFY2(dup.value(QStringLiteral("success")).toBool(),
                 qPrintable(dup.value(QStringLiteral("message")).toString()));
        const QJsonObject dupData = dup.value(QStringLiteral("data")).toObject();
        QCOMPARE(dupData.value(QStringLiteral("total_groups")).toInt(), 1);
        QCOMPARE(dupData.value(QStringLiteral("groups"))
                     .toArray()
                     .at(0)
                     .toObject()
                     .value(QStringLiteral("file_count"))
                     .toInt(),
                 2);

        const QJsonObject search =
            run(QStringLiteral("search.find_in_files"),
                QJsonObject{{QStringLiteral("root_path"), dirPath},
                            {QStringLiteral("pattern"), QStringLiteral("NEEDLE")}});
        QVERIFY2(search.value(QStringLiteral("success")).toBool(),
                 qPrintable(search.value(QStringLiteral("message")).toString()));
        QCOMPARE(search.value(QStringLiteral("data"))
                     .toObject()
                     .value(QStringLiteral("total_matches"))
                     .toInt(),
                 2);

        const QJsonObject scan = run(QStringLiteral("files.scan_directory"),
                                     QJsonObject{{QStringLiteral("root_path"), dirPath}});
        QVERIFY2(scan.value(QStringLiteral("success")).toBool(),
                 qPrintable(scan.value(QStringLiteral("message")).toString()));
        const QJsonObject scanData = scan.value(QStringLiteral("data")).toObject();
        QCOMPARE(scanData.value(QStringLiteral("files_found")).toInt(), 2);
        for (const auto& value : scanData.value(QStringLiteral("entries")).toArray()) {
            QVERIFY(!value.toObject()
                         .value(QStringLiteral("path"))
                         .toString()
                         .contains(QStringLiteral("link.txt")));
        }
    }

    void symlinkFileEntriesSkippedAcrossWalkOps() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray payload = "R2 NEEDLE canary identical payload for the symlink-skip cert";
        const auto write = [&](const QString& name) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(payload), static_cast<qint64>(payload.size()));
        };
        write(QStringLiteral("real.txt"));
        write(QStringLiteral("real2.txt"));

        // Create an NTFS file symlink real.txt <- link.txt (identical content via the target).
        const QString link = dir.filePath(QStringLiteral("link.txt"));
        QProcess mklink;
        mklink.start(QStringLiteral("cmd"),
                     {QStringLiteral("/c"),
                      QStringLiteral("mklink"),
                      QDir::toNativeSeparators(link),
                      QDir::toNativeSeparators(dir.filePath(QStringLiteral("real.txt")))});
        if (!mklink.waitForFinished(10'000) || mklink.exitCode() != 0 ||
            !QFileInfo(link).isSymLink()) {
            QSKIP("could not create an NTFS file symlink on this host (needs privilege/Dev Mode)");
        }

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        verifySymlinkExcludedFromWalkOps(panel, dir.path());
    }

    // R2, unprivileged half: a DIRECTORY JUNCTION entry inside a scanned tree is skipped by the
    // file_scanner walk (scan_directory). Runs where the file-symlink cert above must QSKIP: mklink
    // /J needs no privilege. The junction is caught by isReparsePointEntry's GetFileAttributesW
    // reparse-attribute read, NOT by directory_entry::is_symlink() -- which on MSVC returns FALSE
    // for a junction (it maps to file_type::junction, not file_type::symlink). That gap is exactly
    // why the skip matters here: with is_symlink()==false, canDescendInto does not block the
    // junction, so WITHOUT the skip the walk would DESCEND it -- counting it as a directory AND
    // reaching target/inside.txt (directories_found==2, files_found==2). The skip drops it before
    // both emission and descent, so directories_found==1 (only real 'keep') and files_found==1
    // (only keep/a.txt) is the observable proof.
    void scanDirectorySkipsReparseDirEntry() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTemporaryDir target;  // junction target OUTSIDE root, so its file never double-counts
        QVERIFY(target.isValid());
        writeTextFile(dir.filePath(QStringLiteral("keep/a.txt")), QByteArrayLiteral("hello"));
        writeTextFile(target.filePath(QStringLiteral("inside.txt")),
                      QByteArrayLiteral("only reachable if the junction were descended"));

        const QString junction = dir.filePath(QStringLiteral("jlink"));
        QProcess mklink;
        mklink.start(QStringLiteral("cmd"),
                     {QStringLiteral("/c"),
                      QStringLiteral("mklink"),
                      QStringLiteral("/J"),
                      QDir::toNativeSeparators(junction),
                      QDir::toNativeSeparators(target.path())});
        if (!mklink.waitForFinished(10'000) || mklink.exitCode() != 0 ||
            !QFileInfo(junction).isJunction()) {
            QSKIP("could not create a directory junction on this host");
        }

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result =
            runArchiveOp(panel,
                         QStringLiteral("files.scan_directory"),
                         QJsonObject{{QStringLiteral("root_path"), dir.path()}});
        QVERIFY2(result.value(QStringLiteral("success")).toBool(),
                 qPrintable(result.value(QStringLiteral("message")).toString()));
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("directories_found")).toInt(), 1);
        QCOMPARE(data.value(QStringLiteral("files_found")).toInt(), 1);
        for (const auto& value : data.value(QStringLiteral("entries")).toArray()) {
            QVERIFY(!value.toObject()
                         .value(QStringLiteral("path"))
                         .toString()
                         .contains(QStringLiteral("jlink")));
        }
    }

    // W1c: email.read_mbox drives the real MboxParser (synchronous read API) end to
    // end and deterministically -- a temp mailbox with two messages yields exactly
    // those two, with parsed headers.
    void readMboxListsMessages() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("sample.mbox"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const QByteArray mbox =
                "From alice@example.com Mon Jan  1 00:00:00 2024\n"
                "From: alice@example.com\n"
                "To: bob@example.com\n"
                "Subject: First message\n"
                "Date: Mon, 1 Jan 2024 00:00:00 +0000\n"
                "\n"
                "Body one.\n"
                "\n"
                "From carol@example.com Mon Jan  1 00:01:00 2024\n"
                "From: carol@example.com\n"
                "Subject: Second message\n"
                "\n"
                "Body two.\n";
            QCOMPARE(file.write(mbox), static_cast<qint64>(mbox.size()));
        }

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("path"), path}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.read_mbox")},
                        {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("message_count")).toInt(), 2);
        const QJsonArray messages = data.value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages.at(0).toObject().value(QStringLiteral("subject")).toString(),
                 QStringLiteral("First message"));
        QCOMPARE(messages.at(0).toObject().value(QStringLiteral("from")).toString(),
                 QStringLiteral("alice@example.com"));
    }

    // W1c: read_mbox on a non-MBOX file fails cleanly (never crashes).
    void readMboxRejectsNonMbox() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("notes.txt"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("just some text, not a mailbox\n"), static_cast<qint64>(30));
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("path"), path}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.read_mbox")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
    }

    // email.read_pst helper: run the op with a JSON args object (robust path escaping).
    static QJsonObject runReadPst(AiAssistantPanel& panel, const QJsonObject& arguments) {
        const QString args =
            QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
        return panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.read_pst")},
                        {QStringLiteral("arguments"), args}});
    }

    // email.read_pst: a missing file fails cleanly (guarantees PstParser's exists precondition).
    void readPstMissingFileFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = runReadPst(
            panel, QJsonObject{{QStringLiteral("path"), QStringLiteral("C:/nope/no_such.pst")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
    }

    // email.read_pst: refuse a UNC/network path -- reading it would pull over SMB and could leak
    // credentials (same guard as read_mbox / find_in_files).
    void readPstRefusesUncPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = runReadPst(
            panel,
            QJsonObject{{QStringLiteral("path"), QStringLiteral("\\\\server\\share\\mail.pst")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("network"), Qt::CaseInsensitive));
    }

    // email.read_pst: a readable NON-PST file makes PstParser::open leave isOpen()==false, which
    // the op maps to an HONEST failure -- never a fake "0 folders" empty-success (fail-closed).
    void readPstNonPstFileReportsFailure() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("not_a.pst"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray(8192, 'Z')), static_cast<qint64>(8192));
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = runReadPst(panel, QJsonObject{{QStringLiteral("path"), path}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("PST"), Qt::CaseInsensitive));
    }

    // email.read_pst: read-only -> UNGATED in a Chat & Research session (never policy-blocked),
    // whether or not the parse succeeds.
    void readPstRunsUngatedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("x.pst"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray(8192, 'Z')), static_cast<qint64>(8192));
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = runReadPst(panel, QJsonObject{{QStringLiteral("path"), path}});
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
    }

    // email.recover_deleted helper: run the op with a JSON args object (robust path escaping).
    static QJsonObject runRecoverDeleted(AiAssistantPanel& panel, const QJsonObject& arguments) {
        const QString args =
            QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
        return panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.recover_deleted")},
                        {QStringLiteral("arguments"), args}});
    }

    // email.recover_deleted: a missing file fails cleanly (shares read_pst's validatePstReadPath).
    void recoverDeletedMissingFileFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = runRecoverDeleted(
            panel, QJsonObject{{QStringLiteral("path"), QStringLiteral("C:/nope/no_such.pst")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
    }

    // email.recover_deleted: refuse a UNC/network path (reading it would pull over SMB and could
    // leak an NTLM handshake) -- the shared PST path guard.
    void recoverDeletedRefusesUncPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = runRecoverDeleted(
            panel,
            QJsonObject{{QStringLiteral("path"), QStringLiteral("\\\\server\\share\\mail.pst")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("network"), Qt::CaseInsensitive));
    }

    // email.recover_deleted: a readable NON-PST file leaves PstParser::open isOpen()==false, which
    // the op maps to an HONEST failure -- never a fake "0 recoverable" empty-success (fail-closed).
    void recoverDeletedNonPstFileReportsFailure() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("not_a.pst"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray(8192, 'Z')), static_cast<qint64>(8192));
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = runRecoverDeleted(panel,
                                                     QJsonObject{{QStringLiteral("path"), path}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("PST"), Qt::CaseInsensitive));
    }

    // email.recover_deleted: read-only -> UNGATED in a Chat & Research session (never
    // policy-blocked), whether or not the parse succeeds -- it only READS message metadata.
    void recoverDeletedRunsUngatedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("x.pst"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray(8192, 'Z')), static_cast<qint64>(8192));
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = runRecoverDeleted(panel,
                                                     QJsonObject{{QStringLiteral("path"), path}});
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
    }

    // ------------------------------------------------------------------
    // W2a: email.export_mbox -- the first MUTATING app action. The critical new
    // proofs are (1) it is listed as mutating (so the gate WILL fire), (2) a
    // chat/research session refuses it, (3) when gated + approved it actually runs
    // the real export and writes files, and (4)/(5) its guards fail closed.
    // ------------------------------------------------------------------

    // Writes the same deterministic two-message mailbox the read_mbox test uses.
    static void writeSampleMbox(const QString& path) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray mbox =
            "From alice@example.com Mon Jan  1 00:00:00 2024\n"
            "From: alice@example.com\n"
            "To: bob@example.com\n"
            "Subject: First message\n"
            "Date: Mon, 1 Jan 2024 00:00:00 +0000\n"
            "\n"
            "Body one.\n"
            "\n"
            "From carol@example.com Mon Jan  1 00:01:00 2024\n"
            "From: carol@example.com\n"
            "Subject: Second message\n"
            "\n"
            "Body two.\n";
        QCOMPARE(file.write(mbox), static_cast<qint64>(mbox.size()));
    }

    // email.search_mbox drives the app's OWN EmailSearchWorker over an open MboxParser end to end +
    // deterministically: the sample MBOX has a subject "Second message", so a search for "Second"
    // yields exactly one hit with that subject + a context snippet. Read-only; returns a snippet,
    // never the full body.
    void searchMboxFindsSubjectMatch() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("box.mbox"));
        writeSampleMbox(path);

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (read-only runs ungated)
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                      {QStringLiteral("query"), QStringLiteral("Second")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.search_mbox")},
                        {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("total_hits")).toInt(), 1);
        const QJsonArray hits = data.value(QStringLiteral("hits")).toArray();
        QCOMPARE(hits.size(), 1);
        const QJsonObject hit = hits.at(0).toObject();
        QCOMPARE(hit.value(QStringLiteral("subject")).toString(), QStringLiteral("Second message"));
        QVERIFY(hit.contains(QStringLiteral("context")));
        QVERIFY(!hit.contains(QStringLiteral("body")));  // snippet only, never the full body
    }

    // A completed search with no matches is an honest SUCCESS with zero hits (only a read FAILURE
    // is an error) -- never a misleading failure.
    void searchMboxNoMatchIsHonestZero() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("box.mbox"));
        writeSampleMbox(path);
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                      {QStringLiteral("query"), QStringLiteral("zzznomatchzzz")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.search_mbox")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("total_hits")).toInt(), 0);
        QVERIFY(data.value(QStringLiteral("hits")).toArray().isEmpty());
    }

    // A missing query fails cleanly (never searches).
    void searchMboxMissingQueryFails() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("box.mbox"));
        writeSampleMbox(path);
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("path"), path}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.search_mbox")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("query")));
    }

    // A UNC/network path is refused before opening (the shared refuseUnsafePath guard).
    void searchMboxRejectsNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"),
                                       QStringLiteral("\\\\attacker.example.com\\share\\in.mbox")},
                                      {QStringLiteral("query"), QStringLiteral("hi")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.search_mbox")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // email.search_pst guards + honesty (shape-certed, like read_pst: no hand-buildable PST
    // fixture; the real search logic is covered by the MBOX match cert -- same EmailSearchWorker +
    // serializer
    // + emit contract). A missing query fails cleanly on an otherwise-valid file path.
    void searchPstMissingQueryFails() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("box.pst"));
        writeTextFile(path, QByteArrayLiteral("not a real pst"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("path"), path}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.search_pst")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("query")));
    }

    // A UNC/network path is refused before opening (shared refuseUnsafePath via
    // validatePstReadPath).
    void searchPstRejectsNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"),
                                       QStringLiteral("\\\\attacker.example.com\\share\\in.pst")},
                                      {QStringLiteral("query"), QStringLiteral("hi")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.search_pst")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // An invalid/garbage PST fails honestly at open -- never a misleading empty-hit success.
    void searchPstBogusFileFails() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("box.pst"));
        writeTextFile(path, QByteArrayLiteral("this is not a valid PST file body"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                      {QStringLiteral("query"), QStringLiteral("hello")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.search_pst")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message")).toString().contains(QStringLiteral("PST")));
    }

    // network.generate_wifi_setup_script: READ-ONLY (runs ungated in a chat session) -- builds a
    // WLAN .cmd via the shared sak::buildWifiSetupScriptWindows. Real output: the script adds a
    // profile + connects, and (for WPA) embeds the password. Nothing is executed or written.
    void generateWifiScriptBuildsWpaScript() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (read-only runs ungated)
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("ssid"), QStringLiteral("MyNet")},
                                      {QStringLiteral("password"), QStringLiteral("secret123")},
                                      {QStringLiteral("security"), QStringLiteral("wpa2")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.generate_wifi_setup_script")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        const QString script = data.value(QStringLiteral("script")).toString();
        // netsh is invoked by its absolute System32 path (anti-hijack).
        QVERIFY(script.contains(QStringLiteral("netsh.exe\" wlan add profile")));
        QVERIFY(script.contains(QStringLiteral("wlan connect name=\"MyNet\"")));
        QVERIFY(data.value(QStringLiteral("embeds_password")).toBool());
    }

    // An OPEN network with a password: the profile omits keyMaterial, so embeds_password must be
    // false and the note must not claim a password is in the script (honest disclosure).
    void generateWifiScriptOpenNetworkReportsNoEmbeddedPassword() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("ssid"), QStringLiteral("CafeGuest")},
                                      {QStringLiteral("password"), QStringLiteral("ignored")},
                                      {QStringLiteral("security"), QStringLiteral("open")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.generate_wifi_setup_script")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(!data.value(QStringLiteral("embeds_password")).toBool());
        QVERIFY(
            !data.value(QStringLiteral("note")).toString().contains(QStringLiteral("password")));
    }

    // An SSID with a double quote cannot be safely embedded -> refused (no script).
    void generateWifiScriptRefusesUnsafeSsid() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("ssid"), QStringLiteral("Net\"work")},
                                      {QStringLiteral("password"), QStringLiteral("pw")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.generate_wifi_setup_script")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("cannot be safely scripted")));
    }

    // A missing SSID fails cleanly.
    void generateWifiScriptRequiresSsid() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.generate_wifi_setup_script")},
            {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("ssid")));
    }

    // ------------------------------------------------------------------
    // email.convert_ost -- MUTATING (not destructive): converts an OST/PST store to a directory of
    // files via the app's OWN OstConversionWorker. Shape + guard certed (no hand-buildable OST
    // fixture, like export_pst): the real writer paths mirror the already-shipped export writers.
    // Proofs: listed mutating, chat refuses it, direct IMAP upload refused, guards fail closed.
    // ------------------------------------------------------------------

    void convertOstListedAsMutating() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("email.convert_ost")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("requires_admin")).toBool());
            }
        }
        QVERIFY(found);
    }

    void convertOstBlockedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("in.pst"));
        writeTextFile(path, QByteArrayLiteral("not a pst"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                      {QStringLiteral("output_directory"),
                                       dir.filePath(QStringLiteral("out"))},
                                      {QStringLiteral("format"), QStringLiteral("eml")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.convert_ost")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
        QVERIFY(!QDir(dir.filePath(QStringLiteral("out"))).exists());
    }

    // Direct IMAP upload (a network-egress "format") is refused -- the headless tool only writes
    // files.
    void convertOstRefusesImapFormat() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("in.pst"));
        writeTextFile(path, QByteArrayLiteral("not a pst"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                      {QStringLiteral("output_directory"),
                                       dir.filePath(QStringLiteral("out"))},
                                      {QStringLiteral("format"), QStringLiteral("imap_upload")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.convert_ost")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("IMAP")));
        QVERIFY(
            !QDir(dir.filePath(QStringLiteral("out"))).exists());  // never opened/wrote anything
    }

    // PST/MSG/DBX are refused up front with a precise reason (their writers are not
    // spec-conformant), never attempted and left to fail mid-run (B7-12).
    void convertOstRefusesUnsupportedFormat() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("in.pst"));
        writeTextFile(path, QByteArrayLiteral("not a pst"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        for (const QString& fmt :
             {QStringLiteral("pst"), QStringLiteral("msg"), QStringLiteral("dbx")}) {
            const QString out_sub = QStringLiteral("out_") + fmt;
            const QString args = QString::fromUtf8(
                QJsonDocument(
                    QJsonObject{{QStringLiteral("path"), path},
                                {QStringLiteral("output_directory"), dir.filePath(out_sub)},
                                {QStringLiteral("format"), fmt}})
                    .toJson(QJsonDocument::Compact));
            const QJsonObject result = panel.runAppActionTool(
                QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                            {QStringLiteral("action_id"), QStringLiteral("email.convert_ost")},
                            {QStringLiteral("arguments"), args}});
            QVERIFY(!result.value(QStringLiteral("success")).toBool());
            QVERIFY(result.value(QStringLiteral("message"))
                        .toString()
                        .contains(QStringLiteral("not supported")));
            // Refused before opening: nothing was ever created in the output path.
            QVERIFY(!QDir(dir.filePath(out_sub)).exists());
        }
    }

    // A UNC/network source is refused before opening.
    void convertOstRejectsNetworkPath() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"),
                                       QStringLiteral("\\\\attacker.example.com\\share\\in.pst")},
                                      {QStringLiteral("output_directory"),
                                       dir.filePath(QStringLiteral("out"))},
                                      {QStringLiteral("format"), QStringLiteral("eml")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.convert_ost")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // A non-empty output_directory is refused so nothing pre-existing can be overwritten.
    void convertOstRefusesNonEmptyOutputDir() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("in.pst"));
        writeTextFile(path, QByteArrayLiteral("not a pst"));
        const QString out_dir = dir.filePath(QStringLiteral("dest"));
        QVERIFY(QDir().mkpath(out_dir));
        writeTextFile(out_dir + QStringLiteral("/keepme.txt"), QByteArrayLiteral("precious"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                      {QStringLiteral("output_directory"), out_dir},
                                      {QStringLiteral("format"), QStringLiteral("eml")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.convert_ost")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("directory")));
        // The pre-existing file is untouched.
        QFile check(out_dir + QStringLiteral("/keepme.txt"));
        QVERIFY(check.open(QIODevice::ReadOnly));
        QCOMPARE(check.readAll(), QByteArray("precious"));
    }

    // A regular file that is not a valid OST/PST fails honestly at open (never a false success).
    void convertOstGarbageFileFailsHonestly() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("in.pst"));
        writeTextFile(path, QByteArrayLiteral("definitely not a pst store"));
        const QString out_dir = dir.filePath(QStringLiteral("out"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                      {QStringLiteral("output_directory"), out_dir},
                                      {QStringLiteral("format"), QStringLiteral("eml")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.convert_ost")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("items_converted")).toInt(), 0);
    }

    // ------------------------------------------------------------------
    // network.connect_wifi -- MUTATING + requires_admin: installs a WLAN profile + connects via
    // netsh. Certs are validation-only (listed flags, chat refusal, and input refusals that return
    // BEFORE any netsh/temp-file), so the suite never actually changes the machine's WiFi state.
    // ------------------------------------------------------------------

    void connectWifiListedAsMutatingAdmin() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("network.connect_wifi")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(action.value(QStringLiteral("requires_admin")).toBool());
            }
        }
        QVERIFY(found);
    }

    // A chat/research session refuses the mutating op (policy_blocked) before any netsh runs.
    void connectWifiBlockedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("ssid"), QStringLiteral("SomeNet")},
                                      {QStringLiteral("password"), QStringLiteral("pw")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.connect_wifi")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // A missing SSID fails cleanly, before any netsh/temp-file.
    void connectWifiRequiresSsid() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.connect_wifi")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("ssid")));
    }

    // An SSID with a double quote is refused (fail-closed) before any netsh runs.
    void connectWifiRefusesUnsafeSsid() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("ssid"), QStringLiteral("Net\"work")},
                                      {QStringLiteral("password"), QStringLiteral("pw")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.connect_wifi")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(!data.value(QStringLiteral("profile_added")).toBool());
    }

    // software.clean_leftovers -- MUTATING + destructive + CATASTROPHIC + requires_admin: deletes
    // files/folders/registry/services/tasks/firewall rules via CleanupWorker. Every cert here feeds
    // a DENYLISTED item, so the fail-closed guard refuses the whole batch BEFORE CleanupWorker is
    // built -- the suite never deletes a real system object.
    // ------------------------------------------------------------------

    void cleanLeftoversListedCatastrophic() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("software.clean_leftovers")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
                QVERIFY(action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(action.value(QStringLiteral("catastrophic")).toBool());
                QVERIFY(action.value(QStringLiteral("requires_admin")).toBool());
            }
        }
        QVERIFY(found);
    }

    // A chat/research session refuses the mutating op (policy_blocked) before any deletion.
    void cleanLeftoversBlockedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QString args = QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{QStringLiteral("items"),
                             QJsonArray{QJsonObject{
                                 {QStringLiteral("type"), QStringLiteral("folder")},
                                 {QStringLiteral("path"), QStringLiteral("C:\\Temp\\Acme")}}}}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("software.clean_leftovers")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // A missing/empty items array fails cleanly, before any deletion.
    void cleanLeftoversRequiresItems() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("software.clean_leftovers")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("items")));
    }

    // Run clean_leftovers Unattended (forced confirm auto-approved) with a single denylisted item
    // and assert it is REFUSED with a non-empty "refused" list -- proving the guard fired and the
    // worker never ran. @p item is one leftover-item object.
    QJsonObject runCleanRefusedItem(AiAssistantPanel& panel, const QJsonObject& item) {
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("items"), QJsonArray{item}}})
                .toJson(QJsonDocument::Compact));
        return panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("software.clean_leftovers")},
                        {QStringLiteral("arguments"), args}});
    }

    void cleanLeftoversRefusesOsCriticalTargets() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        const QVector<QJsonObject> denied = {
            // OS-critical registry subtree.
            QJsonObject{{QStringLiteral("type"), QStringLiteral("registry_key")},
                        {QStringLiteral("path"),
                         QStringLiteral("HKLM\\SYSTEM\\CurrentControlSet\\Services\\RpcSs")}},
            // Critical Windows service.
            QJsonObject{{QStringLiteral("type"), QStringLiteral("service")},
                        {QStringLiteral("path"), QStringLiteral("RpcSs")}},
            // Firewall wildcard that deletes ALL rules.
            QJsonObject{{QStringLiteral("type"), QStringLiteral("firewall_rule")},
                        {QStringLiteral("path"), QStringLiteral("all")}},
            // Windows system directory.
            QJsonObject{{QStringLiteral("type"), QStringLiteral("folder")},
                        {QStringLiteral("path"), QStringLiteral("C:\\Windows\\System32")}},
            // The \Microsoft\Windows OS task tree.
            QJsonObject{{QStringLiteral("type"), QStringLiteral("scheduled_task")},
                        {QStringLiteral("path"), QStringLiteral("\\Microsoft\\Windows\\Foo")}}};

        for (const QJsonObject& item : denied) {
            const QJsonObject result = runCleanRefusedItem(panel, item);
            QVERIFY(!result.value(QStringLiteral("success")).toBool());
            const QJsonObject data = result.value(QStringLiteral("data")).toObject();
            QVERIFY(!data.value(QStringLiteral("refused")).toArray().isEmpty());
        }
    }

    // All-or-nothing: a batch mixing a VALID vendor item with one denylisted item is refused whole,
    // so the valid item is never deleted (a poisoned batch cannot smuggle a real deletion through).
    void cleanLeftoversAllOrNothingBatch() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonArray items{QJsonObject{{QStringLiteral("type"), QStringLiteral("registry_key")},
                                           {QStringLiteral("path"),
                                            QStringLiteral("HKLM\\SOFTWARE\\AcmeCorp\\App")}},
                               QJsonObject{{QStringLiteral("type"), QStringLiteral("service")},
                                           {QStringLiteral("path"), QStringLiteral("Dnscache")}}};
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("items"), items}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("software.clean_leftovers")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(!data.value(QStringLiteral("refused")).toArray().isEmpty());
    }

    // A catastrophic batch too large to review item-by-item is REFUSED outright rather than
    // confirmed behind a truncated JSON preview a human cannot vet (>100 items). The confirm never
    // shows and the worker never runs.
    void cleanLeftoversRefusesOversizeBatchForReview() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        QJsonArray items;
        for (int i = 0; i < 150; ++i) {
            items.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("file")},
                {QStringLiteral("path"),
                 QStringLiteral("C:\\Program Files\\AcmeCorp\\App\\stale%1.tmp").arg(i)}});
        }
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("items"), items}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("software.clean_leftovers")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("batch_too_large_to_review"));
        // Refused BEFORE the human confirm -- the approval hook never fired.
        QVERIFY(titles.isEmpty());
    }

    // W2a(1): the action is registered mutating, NOT read_only -- so appActionRunGate
    // enforces the human-gate/restore path rather than skipping it.
    void exportMboxListedAsMutating() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("email.export_mbox")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("requires_admin")).toBool());
            }
        }
        QVERIFY(found);
    }

    // W2a(2): a chat/research session refuses the mutating op (policy_blocked). This
    // is the core guarantee -- a mutating app action cannot run read-only.
    void exportMboxBlockedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("in.mbox"));
        writeSampleMbox(path);

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QString args = QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{QStringLiteral("path"), path},
                            {QStringLiteral("output_path"), dir.filePath(QStringLiteral("out"))}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.export_mbox")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
        // Nothing was written: the output directory must not even exist.
        QVERIFY(!QDir(dir.filePath(QStringLiteral("out"))).exists());
    }

    // W2a(3): in Unattended, the gate offers a restore point (the hook proceeds) and
    // the REAL export runs end to end -- two .eml files land on disk. Proves the gate
    // was actually traversed (restore-point prompt recorded), not bypassed.
    void exportMboxRunsWhenGatedInUnattended() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("in.mbox"));
        writeSampleMbox(path);
        const QString out_dir = dir.filePath(QStringLiteral("exported"));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                      {QStringLiteral("output_path"), out_dir},
                                      {QStringLiteral("format"), QStringLiteral("eml")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.export_mbox")},
                        {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("items_exported")).toInt(), 2);
        // The gate was traversed, not skipped.
        QVERIFY(countTitles(titles, QStringLiteral("Create Restore Point")) >= 1);
        // Two message files actually written to disk.
        const QStringList eml = QDir(out_dir).entryList(QStringList{QStringLiteral("*.eml")},
                                                        QDir::Files);
        QCOMPARE(eml.size(), 2);
    }

    // W2a(4): both source and destination reject UNC/network/device paths, so prompt
    // injection cannot steer the export into an SMB handshake or a write to a share.
    void exportMboxRejectsNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        const QString unc = QStringLiteral("\\\\attacker.example.com\\share\\x.mbox");
        const QString unc_out = QStringLiteral("//attacker.example.com/share/out");
        struct Case {
            QString path;
            QString output_path;
        };
        const QVector<Case> cases = {{unc, QStringLiteral("C:/tmp/out")},
                                     {QStringLiteral("C:/tmp/in.mbox"), unc_out}};
        for (const Case& c : cases) {
            const QString args = QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("path"), c.path},
                                          {QStringLiteral("output_path"), c.output_path}})
                    .toJson(QJsonDocument::Compact));
            const QJsonObject result = panel.runAppActionTool(
                QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                            {QStringLiteral("action_id"), QStringLiteral("email.export_mbox")},
                            {QStringLiteral("arguments"), args}});
            QVERIFY(!result.value(QStringLiteral("success")).toBool());
            QVERIFY(result.value(QStringLiteral("message"))
                        .toString()
                        .contains(QStringLiteral("network")));
        }
    }

    // W2a(5): missing required args fail cleanly (never opens a parser or writes).
    void exportMboxMissingArgsFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.export_mbox")},
                        {QStringLiteral("arguments"), QStringLiteral("{\"path\":\"x.mbox\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("output_path")));
    }

    // W2a(6): the export refuses a NON-EMPTY output directory, so it can never
    // overwrite a pre-existing file (the html/pdf writers do not exists-check). This
    // is what keeps the op honestly non-destructive. Proves the existing file is
    // left byte-for-byte intact and nothing was written.
    void exportMboxRefusesNonEmptyOutputDir() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("in.mbox"));
        writeSampleMbox(path);
        const QString out_dir = dir.filePath(QStringLiteral("dest"));
        QVERIFY(QDir().mkpath(out_dir));
        const QString existing = out_dir + QStringLiteral("/keepme.txt");
        {
            QFile file(existing);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("precious\n"), static_cast<qint64>(9));
        }

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                                        {QStringLiteral("output_path"), out_dir}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.export_mbox")},
                        {QStringLiteral("arguments"), args}});

        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("directory")));
        // Pre-existing file untouched, and no message files were written.
        QFile check(existing);
        QVERIFY(check.open(QIODevice::ReadOnly));
        QCOMPARE(check.readAll(), QByteArray("precious\n"));
        const QStringList produced = QDir(out_dir).entryList(
            QStringList{QStringLiteral("*.eml"), QStringLiteral("*.html"), QStringLiteral("*.pdf")},
            QDir::Files);
        QCOMPARE(produced.size(), 0);
    }

    // ------------------------------------------------------------------
    // email.save_mbox_attachments -- MUTATING (not destructive): extracts one message's
    // attachments into an EXISTING dir via the app's OWN AttachmentBatchSave. Proofs:
    // (1) listed mutating so the gate fires, (2) a chat session refuses it, (3) gated +
    // approved it really writes the decoded attachment to disk, (4) honesty + guards.
    // ------------------------------------------------------------------

    // A one-message mailbox whose message carries one base64 attachment "note.txt" =
    // "hello world" (11 bytes). Exercises the real MIME + base64 decode path in MboxParser.
    static void writeMboxWithAttachment(const QString& path) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray mbox =
            "From alice@example.com Mon Jan  1 00:00:00 2024\n"
            "From: alice@example.com\n"
            "Subject: Has attachment\n"
            "MIME-Version: 1.0\n"
            "Content-Type: multipart/mixed; boundary=\"BOUND\"\n"
            "\n"
            "--BOUND\n"
            "Content-Type: text/plain\n"
            "\n"
            "Body.\n"
            "--BOUND\n"
            "Content-Type: application/octet-stream; name=\"note.txt\"\n"
            "Content-Disposition: attachment; filename=\"note.txt\"\n"
            "Content-Transfer-Encoding: base64\n"
            "\n"
            "aGVsbG8gd29ybGQ=\n"
            "--BOUND--\n";
        QCOMPARE(file.write(mbox), static_cast<qint64>(mbox.size()));
    }

    void saveMboxAttachmentsListedAsMutating() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("email.save_mbox_attachments")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("requires_admin")).toBool());
            }
        }
        QVERIFY(found);
    }

    // A chat/research session refuses the mutating op (policy_blocked); nothing is written.
    void saveMboxAttachmentsBlockedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("in.mbox"));
        writeMboxWithAttachment(path);
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                                        {QStringLiteral("message_index"), 0},
                                                        {QStringLiteral("output_dir"), dir.path()}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("email.save_mbox_attachments")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("note.txt"))));
    }

    // Gated + approved in Unattended, the op runs the REAL saver: the base64 attachment is
    // decoded and written to disk with the exact bytes. Proves end-to-end execution.
    void saveMboxAttachmentsRunsWhenGated() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("in.mbox"));
        writeMboxWithAttachment(path);
        const QString out_dir = dir.filePath(QStringLiteral("saved"));
        QVERIFY(QDir().mkpath(out_dir));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                                        {QStringLiteral("message_index"), 0},
                                                        {QStringLiteral("output_dir"), out_dir}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("email.save_mbox_attachments")},
            {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("saved_count")).toInt(), 1);
        QCOMPARE(data.value(QStringLiteral("failed_count")).toInt(), 0);
        QCOMPARE(data.value(QStringLiteral("attachment_total")).toInt(), 1);
        const QString saved = out_dir + QStringLiteral("/note.txt");
        QVERIFY(QFile::exists(saved));
        QFile check(saved);
        QVERIFY(check.open(QIODevice::ReadOnly));
        QCOMPARE(check.readAll(), QByteArray("hello world"));
    }

    // A message with a nested multipart/related (inline image "pic.png" = "ABC") followed by a
    // top-level PDF attachment "report.pdf" = "DEF". readMessageDetail enumerates attachments
    // RECURSIVELY but readAttachmentData/extractAttachmentBytes does NOT -- pairing them by index
    // would write the PDF's bytes under the image's name and report the PDF unreadable. The op uses
    // readAllAttachments (one recursive pass, name + bytes aligned), so BOTH land with the right
    // bytes under the right name.
    static void writeMboxNestedMultipart(const QString& path) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray mbox =
            "From alice@example.com Mon Jan  1 00:00:00 2024\n"
            "From: alice@example.com\n"
            "Subject: Nested\n"
            "MIME-Version: 1.0\n"
            "Content-Type: multipart/mixed; boundary=\"OUT\"\n"
            "\n"
            "--OUT\n"
            "Content-Type: multipart/related; boundary=\"IN\"\n"
            "\n"
            "--IN\n"
            "Content-Type: text/html\n"
            "\n"
            "<html>hi</html>\n"
            "--IN\n"
            "Content-Type: image/png\n"
            "Content-Disposition: inline; filename=\"pic.png\"\n"
            "Content-Transfer-Encoding: base64\n"
            "\n"
            "QUJD\n"
            "--IN--\n"
            "--OUT\n"
            "Content-Type: application/pdf\n"
            "Content-Disposition: attachment; filename=\"report.pdf\"\n"
            "Content-Transfer-Encoding: base64\n"
            "\n"
            "REVG\n"
            "--OUT--\n";
        QCOMPARE(file.write(mbox), static_cast<qint64>(mbox.size()));
    }

    void saveMboxAttachmentsHandlesNestedMultipart() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("nested.mbox"));
        writeMboxNestedMultipart(path);
        const QString out_dir = dir.filePath(QStringLiteral("out"));
        QVERIFY(QDir().mkpath(out_dir));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                                        {QStringLiteral("message_index"), 0},
                                                        {QStringLiteral("output_dir"), out_dir}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("email.save_mbox_attachments")},
            {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("attachment_total")).toInt(), 2);
        QCOMPARE(data.value(QStringLiteral("saved_count")).toInt(), 2);
        QCOMPARE(data.value(QStringLiteral("failed_count")).toInt(), 0);
        // Each file has its OWN bytes -- not swapped and not misreported.
        QFile pic(out_dir + QStringLiteral("/pic.png"));
        QVERIFY(pic.open(QIODevice::ReadOnly));
        QCOMPARE(pic.readAll(), QByteArray("ABC"));
        QFile pdf(out_dir + QStringLiteral("/report.pdf"));
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        QCOMPARE(pdf.readAll(), QByteArray("DEF"));
    }

    // A message with no attachments is an honest FAILURE (nothing to save), never a false success.
    void saveMboxAttachmentsNoAttachmentsFails() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("plain.mbox"));
        writeSampleMbox(path);  // its messages carry no attachments
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                                        {QStringLiteral("message_index"), 0},
                                                        {QStringLiteral("output_dir"), dir.path()}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("email.save_mbox_attachments")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("no attachments")));
    }

    // A UNC/network source is refused before opening (shared guard).
    void saveMboxAttachmentsRejectsNetworkPath() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"),
                                       QStringLiteral("\\\\attacker.example.com\\share\\in.mbox")},
                                      {QStringLiteral("message_index"), 0},
                                      {QStringLiteral("output_dir"), dir.path()}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("email.save_mbox_attachments")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // A non-existent output_dir fails closed (the saver never creates directories).
    void saveMboxAttachmentsRejectsMissingOutputDir() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("in.mbox"));
        writeMboxWithAttachment(path);
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{QStringLiteral("path"), path},
                            {QStringLiteral("message_index"), 0},
                            {QStringLiteral("output_dir"), dir.filePath(QStringLiteral("nope"))}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("email.save_mbox_attachments")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("existing directory")));
    }

    // ------------------------------------------------------------------
    // email.save_pst_attachments -- MUTATING (not destructive) PST/OST pair to
    // save_mbox_attachments. Shape + guard certed (no hand-buildable PST fixture, like read_pst /
    // search_pst; the real saver + honesty are covered by the MBOX save certs -- same
    // AttachmentBatchSave + AttachmentSaveTally + buildAttachmentSaveResult path). Runs the app's
    // OWN PstParser readAttachments/readAttachmentData index-aligned pair.
    // ------------------------------------------------------------------

    void savePstAttachmentsListedAsMutating() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("email.save_pst_attachments")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("requires_admin")).toBool());
            }
        }
        QVERIFY(found);
    }

    // A chat/research session refuses the mutating op (policy_blocked) before any execution.
    void savePstAttachmentsBlockedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QString args = QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{QStringLiteral("path"), dir.filePath(QStringLiteral("x.pst"))},
                            {QStringLiteral("message_node_id"), 2'097'188},
                            {QStringLiteral("output_dir"), dir.path()}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.save_pst_attachments")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // A missing message_node_id fails cleanly (never opens the file).
    void savePstAttachmentsMissingNodeIdFails() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{QStringLiteral("path"), dir.filePath(QStringLiteral("x.pst"))},
                            {QStringLiteral("output_dir"), dir.path()}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.save_pst_attachments")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("message_node_id")));
    }

    // A UNC/network source is refused before opening.
    void savePstAttachmentsRejectsNetworkPath() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"),
                                       QStringLiteral("\\\\attacker.example.com\\share\\in.pst")},
                                      {QStringLiteral("message_node_id"), 2'097'188},
                                      {QStringLiteral("output_dir"), dir.path()}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.save_pst_attachments")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // A non-existent output_dir fails closed (a regular non-PST file passes the isFile check, so
    // the validator reaches the output_dir check before any parser open).
    void savePstAttachmentsRejectsMissingOutputDir() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("blob.pst"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("not a pst"), static_cast<qint64>(9));
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{QStringLiteral("path"), path},
                            {QStringLiteral("message_node_id"), 2'097'188},
                            {QStringLiteral("output_dir"), dir.filePath(QStringLiteral("nope"))}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.save_pst_attachments")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("existing directory")));
    }

    // A regular file that is not a valid PST/OST fails honestly at open (never a false success).
    void savePstAttachmentsGarbageFileOpenFails() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("blob.pst"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("not a pst"), static_cast<qint64>(9));
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                      {QStringLiteral("message_node_id"), 2'097'188},
                                      {QStringLiteral("output_dir"), dir.path()}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.save_pst_attachments")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("valid PST/OST")));
    }

    // ------------------------------------------------------------------
    // imaging.restore_recoverable -- MUTATING (not destructive) pair to imaging.scan_recoverable.
    // Recovers carved files out of an offline image FILE (opened read-only, hashed before+after)
    // into a directory via the app's OWN FileRecoveryEngine::restoreCandidates. Proofs: listed
    // mutating, chat refuses it, a real range is recovered byte-exact, a traversal-laced extension
    // is neutralized (output stays in the destination), and the guards fail closed.
    // ------------------------------------------------------------------

    // Build image file: 16 zero bytes, then a 32-byte payload at offset 16, then 16 zero bytes.
    static void writeRecoveryImage(const QString& path, const QByteArray& payload) {
        QCOMPARE(payload.size(), 32);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(16, '\0')), static_cast<qint64>(16));
        QCOMPARE(file.write(payload), static_cast<qint64>(32));
        QCOMPARE(file.write(QByteArray(16, '\0')), static_cast<qint64>(16));
    }

    void restoreRecoverableListedAsMutating() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("imaging.restore_recoverable")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("requires_admin")).toBool());
            }
        }
        QVERIFY(found);
    }

    void restoreRecoverableBlockedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject cand{{QStringLiteral("offset_bytes"), 16},
                               {QStringLiteral("size_bytes"), 32}};
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("image_path"),
                                       dir.filePath(QStringLiteral("disk.img"))},
                                      {QStringLiteral("destination_directory"), dir.path()},
                                      {QStringLiteral("candidates"), QJsonArray{cand}}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.restore_recoverable")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // Gated + approved, the op reads the image read-only and writes the recovered 32-byte range to
    // the destination with the derived name recovered_<16-hex-offset>.<ext>, byte-exact.
    void restoreRecoverableRecoversBytes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString image = dir.filePath(QStringLiteral("disk.img"));
        const QByteArray payload = QByteArrayLiteral("RECOVERED-PAYLOAD-0123456789ABCD");
        writeRecoveryImage(image, payload);
        const QString dest = dir.filePath(QStringLiteral("out"));
        QVERIFY(QDir().mkpath(dest));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject cand{{QStringLiteral("offset_bytes"), 16},
                               {QStringLiteral("size_bytes"), 32},
                               {QStringLiteral("extension"), QStringLiteral("bin")}};
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("image_path"), image},
                                      {QStringLiteral("destination_directory"), dest},
                                      {QStringLiteral("candidates"), QJsonArray{cand}}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.restore_recoverable")},
            {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("restored_count")).toInt(), 1);
        QVERIFY(data.value(QStringLiteral("source_opened_read_only")).toBool());
        QVERIFY(data.value(QStringLiteral("source_not_mutated")).toBool());
        QFile out(dest + QStringLiteral("/recovered_0000000000000010.bin"));
        QVERIFY(out.open(QIODevice::ReadOnly));
        QCOMPARE(out.readAll(), payload);
    }

    // A traversal-laced extension is sanitized to letters/digits, so the recovered file stays
    // inside the destination and no file escapes it.
    void restoreRecoverableNeutralizesTraversalExtension() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString image = dir.filePath(QStringLiteral("disk.img"));
        writeRecoveryImage(image, QByteArrayLiteral("RECOVERED-PAYLOAD-0123456789ABCD"));
        const QString dest = dir.filePath(QStringLiteral("out"));
        QVERIFY(QDir().mkpath(dest));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject cand{{QStringLiteral("offset_bytes"), 16},
                               {QStringLiteral("size_bytes"), 32},
                               {QStringLiteral("extension"), QStringLiteral("../../../evil")}};
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("image_path"), image},
                                      {QStringLiteral("destination_directory"), dest},
                                      {QStringLiteral("candidates"), QJsonArray{cand}}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.restore_recoverable")},
            {QStringLiteral("arguments"), args}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        // Sanitized to "evil"; the file is inside dest and nothing escaped above it.
        QVERIFY(QFile::exists(dest + QStringLiteral("/recovered_0000000000000010.evil")));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("evil"))));
        QVERIFY(!QFile::exists(dir.path() + QStringLiteral("/../evil")));
    }

    // A candidate whose byte range runs past the end of the image is refused (never an OOB read).
    void restoreRecoverableRejectsOutOfRangeCandidate() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString image = dir.filePath(QStringLiteral("disk.img"));
        writeRecoveryImage(image,
                           QByteArrayLiteral("RECOVERED-PAYLOAD-0123456789ABCD"));  // 64 bytes
        const QString dest = dir.filePath(QStringLiteral("out"));
        QVERIFY(QDir().mkpath(dest));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject cand{{QStringLiteral("offset_bytes"), 40},
                               {QStringLiteral("size_bytes"), 1000}};  // 40+1000 > 64
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("image_path"), image},
                                      {QStringLiteral("destination_directory"), dest},
                                      {QStringLiteral("candidates"), QJsonArray{cand}}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.restore_recoverable")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("outside")));
    }

    // Candidates whose sizes SUM past the image size are refused (anti disk-fill amplification):
    // two 40-byte ranges (each individually in-bounds of the 64-byte image) sum to 80 > 64.
    void restoreRecoverableRejectsAggregateOverflow() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString image = dir.filePath(QStringLiteral("disk.img"));
        writeRecoveryImage(image,
                           QByteArrayLiteral("RECOVERED-PAYLOAD-0123456789ABCD"));  // 64 bytes
        const QString dest = dir.filePath(QStringLiteral("out"));
        QVERIFY(QDir().mkpath(dest));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonArray cands{
            QJsonObject{{QStringLiteral("offset_bytes"), 0}, {QStringLiteral("size_bytes"), 40}},
            QJsonObject{{QStringLiteral("offset_bytes"), 10}, {QStringLiteral("size_bytes"), 40}}};
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("image_path"), image},
                                      {QStringLiteral("destination_directory"), dest},
                                      {QStringLiteral("candidates"), cands}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.restore_recoverable")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("combined candidate size")));
        // Nothing was written.
        QVERIFY(QDir(dest).entryList(QDir::Files).isEmpty());
    }

    // An empty candidate list fails cleanly.
    void restoreRecoverableNoCandidatesFails() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString image = dir.filePath(QStringLiteral("disk.img"));
        writeRecoveryImage(image, QByteArrayLiteral("RECOVERED-PAYLOAD-0123456789ABCD"));
        const QString dest = dir.filePath(QStringLiteral("out"));
        QVERIFY(QDir().mkpath(dest));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("image_path"), image},
                                      {QStringLiteral("destination_directory"), dest},
                                      {QStringLiteral("candidates"), QJsonArray{}}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.restore_recoverable")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("candidate")));
    }

    // A UNC/network image path is refused before opening.
    void restoreRecoverableRejectsNetworkPath() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject cand{{QStringLiteral("offset_bytes"), 0},
                               {QStringLiteral("size_bytes"), 4}};
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("image_path"),
                                       QStringLiteral("\\\\attacker.example.com\\share\\disk.img")},
                                      {QStringLiteral("destination_directory"), dir.path()},
                                      {QStringLiteral("candidates"), QJsonArray{cand}}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.restore_recoverable")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // A non-existent destination_directory fails closed with a schema-matching field name.
    void restoreRecoverableRejectsMissingDest() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString image = dir.filePath(QStringLiteral("disk.img"));
        writeRecoveryImage(image, QByteArrayLiteral("RECOVERED-PAYLOAD-0123456789ABCD"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject cand{{QStringLiteral("offset_bytes"), 16},
                               {QStringLiteral("size_bytes"), 32}};
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("image_path"), image},
                                      {QStringLiteral("destination_directory"),
                                       dir.filePath(QStringLiteral("nope"))},
                                      {QStringLiteral("candidates"), QJsonArray{cand}}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("imaging.restore_recoverable")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        const QString msg = result.value(QStringLiteral("message")).toString();
        QVERIFY(msg.contains(QStringLiteral("existing directory")));
        QVERIFY(
            msg.contains(QStringLiteral("destination_directory")));  // matches the schema arg name
    }

    // W-net-... email.export_pst: exports a PST/OST via EmailExportWorker::exportItems, mirroring
    // export_mbox. Listed with the SAME risk flags (mutating, not read_only/destructive/admin).
    void exportPstListedAsMutating() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("email.export_pst")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("requires_admin")).toBool());
            }
        }
        QVERIFY(found);
    }

    // email.export_pst: refused in a chat/research session (mutating; never opens the parser or
    // writes -- the output directory is not even created).
    void exportPstBlockedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QString args = QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{QStringLiteral("path"), dir.filePath(QStringLiteral("in.pst"))},
                            {QStringLiteral("output_path"), dir.filePath(QStringLiteral("out"))}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.export_pst")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
        QVERIFY(!QDir(dir.filePath(QStringLiteral("out"))).exists());
    }

    // email.export_pst: both source and destination reject UNC/network/device paths (shared guard).
    void exportPstRejectsNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString unc = QStringLiteral("\\\\attacker.example.com\\share\\x.pst");
        const QString unc_out = QStringLiteral("//attacker.example.com/share/out");
        struct Case {
            QString path;
            QString output_path;
        };
        const QVector<Case> cases = {{unc, QStringLiteral("C:/tmp/out")},
                                     {QStringLiteral("C:/tmp/in.pst"), unc_out}};
        for (const Case& c : cases) {
            const QString args = QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("path"), c.path},
                                          {QStringLiteral("output_path"), c.output_path}})
                    .toJson(QJsonDocument::Compact));
            const QJsonObject result = panel.runAppActionTool(
                QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                            {QStringLiteral("action_id"), QStringLiteral("email.export_pst")},
                            {QStringLiteral("arguments"), args}});
            QVERIFY(!result.value(QStringLiteral("success")).toBool());
            QVERIFY(result.value(QStringLiteral("message"))
                        .toString()
                        .contains(QStringLiteral("network")));
        }
    }

    // email.export_pst: missing required args fail cleanly (never opens a parser or writes).
    void exportPstMissingArgsFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.export_pst")},
                        {QStringLiteral("arguments"), QStringLiteral("{\"path\":\"x.pst\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("output_path")));
    }

    // email.export_pst: a readable file that is NOT a PST/OST is refused honestly via isOpen()==
    // false. Deterministic end-to-end: valid args (local paths, new output dir) pass validation and
    // reach PstParser::open, which fails on the non-PST content -- no crash, no files written. (A
    // valid PST is not hand-buildable in a unit test, so the success path is covered by the email
    // panel's own PST tests; here we prove the honest-failure path.)
    void exportPstRejectsNonPstFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("not.pst"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("this is definitely not a PST store\n") > 0);
        }
        const QString out_dir = dir.filePath(QStringLiteral("pst_out"));  // new/empty

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("path"), path},
                                                        {QStringLiteral("output_path"), out_dir}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("email.export_pst")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("Not a valid PST/OST")));
    }

    // ------------------------------------------------------------------
    // files.compress_zip / files.extract_zip -- MUTATING (not destructive) archive
    // ops. Prove listing/gating parity with the export ops, a real compress->extract
    // round-trip when gated, and that the op-layer guards (UNC, .zip name, no-clobber
    // output, new/empty destination) fail closed. The engine's zip-slip/bomb hardening
    // is its own concern; these cert the dispatch + guards + honest result channel.
    // ------------------------------------------------------------------

    static void writeTextFile(const QString& path, const QByteArray& content) {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(content), static_cast<qint64>(content.size()));
    }

    static QJsonObject runArchiveOp(AiAssistantPanel& panel,
                                    const QString& action_id,
                                    const QJsonObject& arguments) {
        const QString args =
            QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
        return panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), action_id},
                        {QStringLiteral("arguments"), args}});
    }

    static void verifyArchiveOpMutating(const QJsonArray& actions, const QString& id) {
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() == id) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("requires_admin")).toBool());
            }
        }
        QVERIFY(found);
    }

    void archiveOpsListedAsMutating() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        verifyArchiveOpMutating(actions, QStringLiteral("files.compress_zip"));
        verifyArchiveOpMutating(actions, QStringLiteral("files.extract_zip"));
    }

    // Real end-to-end when gated in Unattended: compress a folder into a new .zip, then extract it
    // into a new dir and confirm the file content round-trips. Also proves the gate was traversed.
    void compressExtractRoundTripWhenGated() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString src = dir.filePath(QStringLiteral("src"));
        writeTextFile(src + QStringLiteral("/a.txt"), QByteArrayLiteral("hello A"));
        writeTextFile(src + QStringLiteral("/nested/b.txt"), QByteArrayLiteral("hello B"));
        const QString zip_path = dir.filePath(QStringLiteral("out.zip"));
        const QString dest = dir.filePath(QStringLiteral("unpacked"));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        const QJsonObject compressed =
            runArchiveOp(panel,
                         QStringLiteral("files.compress_zip"),
                         QJsonObject{{QStringLiteral("sources"), QJsonArray{src}},
                                     {QStringLiteral("output_path"), zip_path}});
        QVERIFY(compressed.value(QStringLiteral("success")).toBool());
        QVERIFY(QFileInfo::exists(zip_path));
        QVERIFY(countTitles(titles, QStringLiteral("Create Restore Point")) >= 1);

        const QJsonObject extracted =
            runArchiveOp(panel,
                         QStringLiteral("files.extract_zip"),
                         QJsonObject{{QStringLiteral("archive_path"), zip_path},
                                     {QStringLiteral("destination_dir"), dest}});
        QVERIFY(extracted.value(QStringLiteral("success")).toBool());
        QFile a(dest + QStringLiteral("/src/a.txt"));
        QVERIFY(a.open(QIODevice::ReadOnly));
        QCOMPARE(a.readAll(), QByteArrayLiteral("hello A"));
        QFile b(dest + QStringLiteral("/src/nested/b.txt"));
        QVERIFY(b.open(QIODevice::ReadOnly));
        QCOMPARE(b.readAll(), QByteArrayLiteral("hello B"));
    }

    void compressZipBlockedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeTextFile(dir.filePath(QStringLiteral("a.txt")), QByteArrayLiteral("x"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject result = runArchiveOp(
            panel,
            QStringLiteral("files.compress_zip"),
            QJsonObject{{QStringLiteral("sources"),
                         QJsonArray{dir.filePath(QStringLiteral("a.txt"))}},
                        {QStringLiteral("output_path"), dir.filePath(QStringLiteral("o.zip"))}});
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    void extractZipBlockedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject result = runArchiveOp(
            panel,
            QStringLiteral("files.extract_zip"),
            QJsonObject{{QStringLiteral("archive_path"), QStringLiteral("C:/tmp/x.zip")},
                        {QStringLiteral("destination_dir"), QStringLiteral("C:/tmp/out")}});
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // compress_zip guards all fail closed (never write a clobbering or off-box archive). Gated in
    // Unattended so a guard failure is what stops each case, not the policy gate.
    void compressZipGuardsFailClosed() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString good_src = dir.filePath(QStringLiteral("a.txt"));
        writeTextFile(good_src, QByteArrayLiteral("x"));
        const QString existing_zip = dir.filePath(QStringLiteral("exists.zip"));
        writeTextFile(existing_zip, QByteArrayLiteral("PK"));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        const QVector<QJsonObject> bad = {
            // UNC source
            QJsonObject{{QStringLiteral("sources"),
                         QJsonArray{QStringLiteral("\\\\host\\share\\a.txt")}},
                        {QStringLiteral("output_path"), dir.filePath(QStringLiteral("o.zip"))}},
            // UNC output
            QJsonObject{{QStringLiteral("sources"), QJsonArray{good_src}},
                        {QStringLiteral("output_path"), QStringLiteral("//host/share/o.zip")}},
            // non-.zip output
            QJsonObject{{QStringLiteral("sources"), QJsonArray{good_src}},
                        {QStringLiteral("output_path"), dir.filePath(QStringLiteral("o.tar"))}},
            // output already exists (no clobber)
            QJsonObject{{QStringLiteral("sources"), QJsonArray{good_src}},
                        {QStringLiteral("output_path"), existing_zip}},
            // missing source
            QJsonObject{{QStringLiteral("sources"),
                         QJsonArray{dir.filePath(QStringLiteral("nope.txt"))}},
                        {QStringLiteral("output_path"), dir.filePath(QStringLiteral("o.zip"))}},
            // missing args (no sources)
            QJsonObject{{QStringLiteral("output_path"), dir.filePath(QStringLiteral("o.zip"))}}};
        for (const QJsonObject& args : bad) {
            const QJsonObject result =
                runArchiveOp(panel, QStringLiteral("files.compress_zip"), args);
            QVERIFY(!result.value(QStringLiteral("success")).toBool());
            QVERIFY(!result.contains(QStringLiteral("failure_class")));  // a guard, not the gate
        }
        // The pre-existing archive was never clobbered/removed by a failed run.
        QCOMPARE(QFile(existing_zip).size(), static_cast<qint64>(2));
    }

    // extract_zip guards fail closed (never read over SMB, never clobber a populated directory).
    void extractZipGuardsFailClosed() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString real_zip = dir.filePath(QStringLiteral("real.zip"));
        writeTextFile(dir.filePath(QStringLiteral("src/a.txt")), QByteArrayLiteral("x"));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        // Make one genuine archive so the "non-empty destination" case reaches the dir guard.
        runArchiveOp(panel,
                     QStringLiteral("files.compress_zip"),
                     QJsonObject{{QStringLiteral("sources"),
                                  QJsonArray{dir.filePath(QStringLiteral("src"))}},
                                 {QStringLiteral("output_path"), real_zip}});
        const QString nonempty = dir.filePath(QStringLiteral("populated"));
        writeTextFile(nonempty + QStringLiteral("/keep.txt"), QByteArrayLiteral("keep"));

        const QVector<QJsonObject> bad = {
            // UNC archive
            QJsonObject{{QStringLiteral("archive_path"), QStringLiteral("\\\\host\\share\\a.zip")},
                        {QStringLiteral("destination_dir"), dir.filePath(QStringLiteral("d1"))}},
            // UNC destination
            QJsonObject{{QStringLiteral("archive_path"), real_zip},
                        {QStringLiteral("destination_dir"), QStringLiteral("//host/share/out")}},
            // nonexistent archive
            QJsonObject{{QStringLiteral("archive_path"), dir.filePath(QStringLiteral("nope.zip"))},
                        {QStringLiteral("destination_dir"), dir.filePath(QStringLiteral("d2"))}},
            // non-empty destination (would clobber)
            QJsonObject{{QStringLiteral("archive_path"), real_zip},
                        {QStringLiteral("destination_dir"), nonempty}},
            // missing args
            QJsonObject{{QStringLiteral("archive_path"), real_zip}}};
        for (const QJsonObject& args : bad) {
            const QJsonObject result =
                runArchiveOp(panel, QStringLiteral("files.extract_zip"), args);
            QVERIFY(!result.value(QStringLiteral("success")).toBool());
            QVERIFY(!result.contains(QStringLiteral("failure_class")));  // a guard, not the gate
        }
        // The populated directory's pre-existing file survived every refused extract.
        QVERIFY(QFileInfo::exists(nonempty + QStringLiteral("/keep.txt")));
    }

    // extract_zip's validation errors must name ITS OWN schema fields (archive_path/
    // destination_dir), not the shared validator's export field names -- otherwise a model cannot
    // correct the right argument (the schema is additionalProperties:false).
    void extractZipErrorNamesItsOwnFields() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString real_zip = dir.filePath(QStringLiteral("real.zip"));
        writeTextFile(dir.filePath(QStringLiteral("src/a.txt")), QByteArrayLiteral("x"));
        const QString nonempty = dir.filePath(QStringLiteral("populated"));
        writeTextFile(nonempty + QStringLiteral("/keep.txt"), QByteArrayLiteral("keep"));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        runArchiveOp(panel,
                     QStringLiteral("files.compress_zip"),
                     QJsonObject{{QStringLiteral("sources"),
                                  QJsonArray{dir.filePath(QStringLiteral("src"))}},
                                 {QStringLiteral("output_path"), real_zip}});

        // Missing destination_dir -> error names archive_path + destination_dir, never output_path.
        const QJsonObject missing =
            runArchiveOp(panel,
                         QStringLiteral("files.extract_zip"),
                         QJsonObject{{QStringLiteral("archive_path"), real_zip}});
        const QString missing_msg = missing.value(QStringLiteral("message")).toString();
        QVERIFY(missing_msg.contains(QStringLiteral("destination_dir")));
        QVERIFY(!missing_msg.contains(QStringLiteral("output_path")));

        // Non-empty destination -> the new/empty-dir error names destination_dir, not output_path.
        const QJsonObject nonempty_result =
            runArchiveOp(panel,
                         QStringLiteral("files.extract_zip"),
                         QJsonObject{{QStringLiteral("archive_path"), real_zip},
                                     {QStringLiteral("destination_dir"), nonempty}});
        const QString nonempty_msg = nonempty_result.value(QStringLiteral("message")).toString();
        QVERIFY(nonempty_msg.contains(QStringLiteral("destination_dir")));
        QVERIFY(!nonempty_msg.contains(QStringLiteral("output_path")));
    }

    // files.list_archive: read-only inspection. Compress a known tree, then list the archive and
    // confirm the entries + counts round-trip. Read-only -> UNGATED even in a Chat session.
    void listArchiveListsEntriesUngated() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString src = dir.filePath(QStringLiteral("src"));
        writeTextFile(src + QStringLiteral("/a.txt"), QByteArrayLiteral("hello A"));
        writeTextFile(src + QStringLiteral("/nested/b.txt"), QByteArrayLiteral("hello B"));
        const QString zip = dir.filePath(QStringLiteral("out.zip"));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject compressed =
            runArchiveOp(panel,
                         QStringLiteral("files.compress_zip"),
                         QJsonObject{{QStringLiteral("sources"), QJsonArray{src}},
                                     {QStringLiteral("output_path"), zip}});
        QVERIFY(compressed.value(QStringLiteral("success")).toBool());

        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject listed = runArchiveOp(panel,
                                                QStringLiteral("files.list_archive"),
                                                QJsonObject{{QStringLiteral("archive_path"), zip}});
        QVERIFY(!listed.contains(QStringLiteral("failure_class")));  // read-only, never gated
        QVERIFY(listed.value(QStringLiteral("success")).toBool());
        const QJsonObject data = listed.value(QStringLiteral("data")).toObject();
        QVERIFY(data.value(QStringLiteral("total_entries")).toInt() >= 2);
        bool have_a = false;
        bool have_b = false;
        for (const auto& value : data.value(QStringLiteral("entries")).toArray()) {
            const QString p = value.toObject().value(QStringLiteral("path")).toString();
            have_a = have_a || p.contains(QStringLiteral("a.txt"));
            have_b = have_b || p.contains(QStringLiteral("b.txt"));
        }
        QVERIFY(have_a);
        QVERIFY(have_b);
    }

    void listArchiveMissingArgFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result =
            runArchiveOp(panel, QStringLiteral("files.list_archive"), QJsonObject{});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("archive_path")));
    }

    // A non-zip file makes listEntries fail (isReadable false), which the op maps to an HONEST
    // failure -- never a fake "0 entries" empty listing.
    void listArchiveNonArchiveFileReportsFailure() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("not.zip"));
        writeTextFile(path, QByteArrayLiteral("this is definitely not a zip archive"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result =
            runArchiveOp(panel,
                         QStringLiteral("files.list_archive"),
                         QJsonObject{{QStringLiteral("archive_path"), path}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("list"), Qt::CaseInsensitive));
    }

    // files.scan_directory: read-only tree walk + disk-usage totals. Deterministic temp tree ->
    // exact file/dir counts + total bytes, entries include the files, ungated in a Chat session.
    void scanDirectoryListsTreeUngated() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeTextFile(dir.filePath(QStringLiteral("a/b.txt")), QByteArrayLiteral("hello"));
        writeTextFile(dir.filePath(QStringLiteral("a/c/d.txt")), QByteArrayLiteral("worldworld"));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject result =
            runArchiveOp(panel,
                         QStringLiteral("files.scan_directory"),
                         QJsonObject{{QStringLiteral("root_path"), dir.path()}});
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // read-only, never gated
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("files_found")).toInt(), 2);
        QCOMPARE(data.value(QStringLiteral("directories_found")).toInt(), 2);
        QCOMPARE(data.value(QStringLiteral("total_size_bytes")).toDouble(), 15.0);
        // A clean temp tree finishes fully with no unreadable subdirs.
        QVERIFY(data.value(QStringLiteral("scan_complete")).toBool());
        QCOMPARE(data.value(QStringLiteral("errors_encountered")).toInt(), 0);
        bool have_b = false;
        bool have_d = false;
        for (const auto& value : data.value(QStringLiteral("entries")).toArray()) {
            const QString p = value.toObject().value(QStringLiteral("path")).toString();
            have_b = have_b || p.contains(QStringLiteral("b.txt"));
            have_d = have_d || p.contains(QStringLiteral("d.txt"));
        }
        QVERIFY(have_b);
        QVERIFY(have_d);
    }

    void scanDirectoryMissingArgFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result =
            runArchiveOp(panel, QStringLiteral("files.scan_directory"), QJsonObject{});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("root_path")));
    }

    void scanDirectoryRejectsNonDirectory() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("x.txt"));
        writeTextFile(file, QByteArrayLiteral("x"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = runArchiveOp(panel,
                                                QStringLiteral("files.scan_directory"),
                                                QJsonObject{{QStringLiteral("root_path"), file}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("directory")));
    }

    // min_file_size must actually filter (it was a silent no-op: the engine only honors it when
    // calculate_sizes is on, which this op keeps off; the op now applies it in the callback).
    void scanDirectoryHonorsMinFileSize() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeTextFile(dir.filePath(QStringLiteral("small.txt")), QByteArray(3, 'x'));
        writeTextFile(dir.filePath(QStringLiteral("big.txt")), QByteArray(100, 'y'));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result =
            runArchiveOp(panel,
                         QStringLiteral("files.scan_directory"),
                         QJsonObject{{QStringLiteral("root_path"), dir.path()},
                                     {QStringLiteral("min_file_size"), 50}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("files_found")).toInt(), 1);
        QCOMPARE(data.value(QStringLiteral("total_size_bytes")).toDouble(), 100.0);
        bool have_small = false;
        bool have_big = false;
        for (const auto& value : data.value(QStringLiteral("entries")).toArray()) {
            const QString p = value.toObject().value(QStringLiteral("path")).toString();
            have_small = have_small || p.contains(QStringLiteral("small.txt"));
            have_big = have_big || p.contains(QStringLiteral("big.txt"));
        }
        QVERIFY(have_big);
        QVERIFY(!have_small);
    }

    // backup.preview_app_data with no app_name fails cleanly (never scans).
    void previewAppDataMissingNameFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result =
            runArchiveOp(panel, QStringLiteral("backup.preview_app_data"), QJsonObject{});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("app_name")));
    }

    // backup.preview_app_data is read-only: even in a chat/research session it RUNS (never
    // policy_blocked). A random GUID-like app name matches no standard data directory, so it
    // returns an honest empty preview (found=false, zero size, scan_complete=true) -- not a masked
    // failure.
    void previewAppDataUnknownAppRunsUngatedAndReportsEmpty() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("app_name"),
                                       QStringLiteral("NoSuchApp_5f3c1a9e_dead_beef")}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("backup.preview_app_data")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY2(result.value(QStringLiteral("success")).toBool(),
                 qPrintable(result.value(QStringLiteral("message")).toString()));
        const QString message = result.value(QStringLiteral("message")).toString();
        QVERIFY2(!message.contains(QStringLiteral("policy")), qPrintable(message));
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(!data.value(QStringLiteral("found")).toBool());
        QCOMPARE(data.value(QStringLiteral("directory_count")).toInt(), 0);
        QCOMPARE(data.value(QStringLiteral("total_size_bytes")).toDouble(), 0.0);
        QVERIFY(data.value(QStringLiteral("scan_complete")).toBool());
    }

    // backup.preview_app_data discovers a real directory under a standard data location (Documents)
    // and sizes it via the hardened file_scanner. Creates a uniquely-named subdir so scanForAppData
    // finds it by name, writes a known payload, and asserts the reported total matches. Cleaned up
    // regardless of assertion outcome; QSKIP where Documents is not writable.
    void previewAppDataSizesDiscoveredDir() {
        const QString documents =
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (documents.isEmpty()) {
            QSKIP("no writable Documents location on this host");
        }
        const QString token = QStringLiteral("SakPreviewCanary") +
                              QUuid::createUuid().toString(QUuid::Id128).left(12);
        const QString appDir = QDir(documents).filePath(token);
        if (!QDir().mkpath(appDir + QStringLiteral("/sub"))) {
            QSKIP("could not create a probe directory under Documents");
        }
        const auto cleanup = qScopeGuard([&] { QDir(appDir).removeRecursively(); });

        const QByteArray payload(4096, 'z');
        {
            QFile file(appDir + QStringLiteral("/sub/data.bin"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(payload), static_cast<qint64>(payload.size()));
        }

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("app_name"), token}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("backup.preview_app_data")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY2(result.value(QStringLiteral("success")).toBool(),
                 qPrintable(result.value(QStringLiteral("message")).toString()));
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(data.value(QStringLiteral("found")).toBool());
        QCOMPARE(data.value(QStringLiteral("sized_count")).toInt(), 1);
        QCOMPARE(data.value(QStringLiteral("total_size_bytes")).toDouble(),
                 static_cast<double>(payload.size()));
        QVERIFY(data.value(QStringLiteral("file_count")).toDouble() >= 1.0);
        QVERIFY(data.value(QStringLiteral("scan_complete")).toBool());
        bool listed = false;
        for (const auto& value : data.value(QStringLiteral("paths")).toArray()) {
            listed = listed || value.toString().contains(token);
        }
        QVERIFY(listed);
    }

    // backup.preview_app_data refuses a PATH-SHAPED app_name. app_name is a plain application name,
    // never a path; a UNC/absolute/traversal value must be rejected BEFORE any discovery, so a
    // prompt-injected model cannot turn it into an outbound stat (a UNC value would otherwise leak
    // the NTLM hash). Covers UNC (both slash forms), a drive path, and a traversal.
    void previewAppDataRejectsPathLikeName() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QStringList names = {QStringLiteral("\\\\attacker.example.com\\share"),
                                   QStringLiteral("//attacker.example.com/share"),
                                   QStringLiteral("C:\\Windows"),
                                   QStringLiteral("../../secret")};
        for (const QString& name : names) {
            const QString args =
                QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("app_name"), name}})
                                      .toJson(QJsonDocument::Compact));
            const QJsonObject result = panel.runAppActionTool(QJsonObject{
                {QStringLiteral("operation"), QStringLiteral("run")},
                {QStringLiteral("action_id"), QStringLiteral("backup.preview_app_data")},
                {QStringLiteral("arguments"), args}});
            QVERIFY2(!result.value(QStringLiteral("success")).toBool(), qPrintable(name));
            QVERIFY2(result.value(QStringLiteral("message"))
                         .toString()
                         .contains(QStringLiteral("not a path")),
                     qPrintable(name));
        }
    }

    // Non-ASCII filenames must survive to the model intact (the reported path was built via the
    // NARROW generic_string() -> ANSI code page -> QString::fromStdString-as-UTF-8, which corrupted
    // accented/CJK names; it now converts via the wide path).
    void scanDirectoryPreservesNonAsciiNames() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString name = QString::fromUtf8("Caf\xC3\xA9.txt");  // "Cafe" with an acute e
        writeTextFile(dir.filePath(name), QByteArrayLiteral("x"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result =
            runArchiveOp(panel,
                         QStringLiteral("files.scan_directory"),
                         QJsonObject{{QStringLiteral("root_path"), dir.path()}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        bool found = false;
        for (const auto& value : result.value(QStringLiteral("data"))
                                     .toObject()
                                     .value(QStringLiteral("entries"))
                                     .toArray()) {
            found = found || value.toObject()
                                 .value(QStringLiteral("path"))
                                 .toString()
                                 .contains(QString::fromUtf8("Caf\xC3\xA9"));
        }
        QVERIFY(found);
    }

    // ------------------------------------------------------------------
    // W2b: organizer.organize_directory -- a DESTRUCTIVE mutating op (relocates
    // existing user files). Proves the destructive flag + restore-point path and
    // that the op refuses the data-loss "overwrite" collision strategy.
    // ------------------------------------------------------------------

    static QString organizeArgs(const QString& target,
                                const QJsonObject& mapping,
                                const QString& collision = QString()) {
        QJsonObject obj{{QStringLiteral("target_directory"), target},
                        {QStringLiteral("category_mapping"), mapping}};
        if (!collision.isEmpty()) {
            obj.insert(QStringLiteral("collision_strategy"), collision);
        }
        return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }

    // W2b(1): registered mutating AND destructive, so the gate offers a restore point
    // / forces a confirm rather than skipping.
    void organizeListedAsMutatingDestructive() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("organizer.organize_directory")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
                QVERIFY(!action.value(QStringLiteral("requires_admin")).toBool());
            }
        }
        QVERIFY(found);
    }

    // W2b(2): blocked in a chat/research session; the source file is not moved.
    void organizeBlockedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("note.txt"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject mapping{{QStringLiteral("Docs"), QJsonArray{QStringLiteral("txt")}}};
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("organizer.organize_directory")},
            {QStringLiteral("arguments"), organizeArgs(dir.path(), mapping)}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
        QVERIFY(QFile::exists(file));  // untouched
        QVERIFY(!QDir(dir.filePath(QStringLiteral("Docs"))).exists());
    }

    // W2b(3): in Unattended, the gate offers a restore point and the REAL organize
    // runs -- two files land in their category subfolders, movedCount is exact.
    void organizeMovesFilesWhenGated() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto write = [&](const QString& name) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("data");
        };
        write(QStringLiteral("report.txt"));
        write(QStringLiteral("photo.jpg"));
        write(QStringLiteral("misc.zzz"));  // no category -> stays put

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        const QJsonObject mapping{{QStringLiteral("Docs"), QJsonArray{QStringLiteral("txt")}},
                                  {QStringLiteral("Images"), QJsonArray{QStringLiteral("jpg")}}};
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("organizer.organize_directory")},
            {QStringLiteral("arguments"), organizeArgs(dir.path(), mapping)}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("data"))
                     .toObject()
                     .value(QStringLiteral("files_moved"))
                     .toInt(),
                 2);
        QVERIFY(countTitles(titles, QStringLiteral("Create Restore Point")) >= 1);
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("Docs/report.txt"))));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("Images/photo.jpg"))));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("misc.zzz"))));     // uncategorized stays
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("report.txt"))));  // moved out
    }

    // W2b(4): the data-loss "overwrite" collision strategy is refused outright.
    void organizeRejectsOverwriteStrategy() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject mapping{{QStringLiteral("Docs"), QJsonArray{QStringLiteral("txt")}}};
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("organizer.organize_directory")},
            {QStringLiteral("arguments"),
             organizeArgs(dir.path(), mapping, QStringLiteral("overwrite"))}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("overwrite")));
    }

    // W2b(5): UNC/device target is refused before any filesystem work.
    void organizeRejectsNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject mapping{{QStringLiteral("Docs"), QJsonArray{QStringLiteral("txt")}}};
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("organizer.organize_directory")},
            {QStringLiteral("arguments"),
             organizeArgs(QStringLiteral("\\\\attacker\\share"), mapping)}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // W2b(7): a category name that would escape the target directory (path
    // separator / drive / "..") is refused, so a prompt-injected mapping cannot
    // relocate files outside the target. A sibling file outside stays untouched.
    void organizeRejectsEscapingCategoryName() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = dir.filePath(QStringLiteral("box"));
        QVERIFY(QDir().mkpath(target));
        {
            QFile f(target + QStringLiteral("/doc.txt"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        const QStringList evil = {QStringLiteral(".."),
                                  QStringLiteral("..\\..\\loot"),
                                  QStringLiteral("sub/dir")};
        for (const QString& category : evil) {
            const QJsonObject mapping{{category, QJsonArray{QStringLiteral("txt")}}};
            const QJsonObject result = panel.runAppActionTool(QJsonObject{
                {QStringLiteral("operation"), QStringLiteral("run")},
                {QStringLiteral("action_id"), QStringLiteral("organizer.organize_directory")},
                {QStringLiteral("arguments"), organizeArgs(target, mapping)}});
            QVERIFY2(!result.value(QStringLiteral("success")).toBool(), qPrintable(category));
            QVERIFY2(result.value(QStringLiteral("message"))
                         .toString()
                         .contains(QStringLiteral("category name")),
                     qPrintable(category));
        }
        // The file never moved anywhere.
        QVERIFY(QFile::exists(target + QStringLiteral("/doc.txt")));
    }

    // W2b(6): a missing category_mapping fails cleanly (nothing is moved).
    void organizeMissingMappingFails() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("target_directory"), dir.path()}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("organizer.organize_directory")},
            {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("category_mapping")));
    }

    // ------------------------------------------------------------------
    // files.delete_to_recycle_bin -- a DESTRUCTIVE (but recoverable) mutating op
    // that moves one file/dir to the Windows Recycle Bin via the app's OWN
    // sendPathToRecycleBin. Mirrors the organize_directory gate + guard coverage.
    // ------------------------------------------------------------------

    // Registered mutating AND destructive, no elevation.
    void recycleListedAsMutatingDestructive() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("files.delete_to_recycle_bin")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
                QVERIFY(!action.value(QStringLiteral("requires_admin")).toBool());
            }
        }
        QVERIFY(found);
    }

    // Blocked in a chat/research session; the file is NOT deleted.
    void recycleBlockedInChatSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("keep.txt"));
        writeTextFile(file, QByteArrayLiteral("x"));
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject result = runArchiveOp(panel,
                                                QStringLiteral("files.delete_to_recycle_bin"),
                                                QJsonObject{{QStringLiteral("path"), file}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
        QVERIFY(QFile::exists(file));  // untouched
    }

    // In Unattended the gate offers a restore point and the REAL delete runs: the
    // file leaves its original location (moved to the Recycle Bin).
    void recycleDeletesFileWhenGated() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("trash-me.txt"));
        writeTextFile(file, QByteArrayLiteral("disposable"));
        QVERIFY(QFile::exists(file));

        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = runArchiveOp(panel,
                                                QStringLiteral("files.delete_to_recycle_bin"),
                                                QJsonObject{{QStringLiteral("path"), file}});

        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(data.value(QStringLiteral("recycled")).toBool());
        QCOMPARE(data.value(QStringLiteral("was_directory")).toBool(), false);
        QVERIFY(!QFile::exists(file));  // moved out of its location (to the bin)
    }

    // A wildcard path is refused BEFORE the shell op (SHFileOperation would expand it and delete
    // many files). Runs in Unattended so the thunk executes and its own guard rejects (not the
    // gate).
    void recycleRejectsWildcardPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = runArchiveOp(
            panel,
            QStringLiteral("files.delete_to_recycle_bin"),
            QJsonObject{{QStringLiteral("path"), QStringLiteral("C:\\Data\\Documents\\*")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("wildcard")));
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // a guard, not the gate
    }

    // A UNC/network path is refused before any shell op (the shared network guard).
    void recycleRejectsNetworkPath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = runArchiveOp(
            panel,
            QStringLiteral("files.delete_to_recycle_bin"),
            QJsonObject{{QStringLiteral("path"),
                         QStringLiteral("\\\\attacker.example.com\\share\\file.txt")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // An embedded NUL is refused: it would split sendPathToRecycleBin's double-null pFrom into a
    // MULTI-item delete list, deleting a second path the guards never validated.
    void recycleRejectsEmbeddedNull() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        QString evil = QStringLiteral("C:\\Data\\ok.txt");
        evil.append(QChar(QChar::Null));
        evil.append(QStringLiteral("C:\\Windows\\System32"));
        const QJsonObject result = runArchiveOp(panel,
                                                QStringLiteral("files.delete_to_recycle_bin"),
                                                QJsonObject{{QStringLiteral("path"), evil}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("null")));
    }

    // A drive/volume root is refused -- never recycle a whole volume. The literal root reports the
    // root guard; the trailing-dot/space forms ("C:\\...", "C:\\ .") canonicalize to the same
    // volume root at the Win32 layer, so they must ALSO be refused (they must never reach the shell
    // op as the root). Validating the OS-canonical path -- not the lexical string -- is what closes
    // that escape: whether canonicalization maps them to the root (root guard) or to empty
    // (no-such-file), the security property holds -- the volume root is not recycled.
    void recycleRejectsDriveRoot() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject literal =
            runArchiveOp(panel,
                         QStringLiteral("files.delete_to_recycle_bin"),
                         QJsonObject{{QStringLiteral("path"), QStringLiteral("C:\\")}});
        QVERIFY(!literal.value(QStringLiteral("success")).toBool());
        QVERIFY(
            literal.value(QStringLiteral("message")).toString().contains(QStringLiteral("root")));

        const QStringList rootEscapes = {QStringLiteral("C:\\..."), QStringLiteral("C:\\ .")};
        for (const QString& form : rootEscapes) {
            const QJsonObject result = runArchiveOp(panel,
                                                    QStringLiteral("files.delete_to_recycle_bin"),
                                                    QJsonObject{{QStringLiteral("path"), form}});
            QVERIFY2(!result.value(QStringLiteral("success")).toBool(), qPrintable(form));
        }
    }

    // A protected SHARED ROOT (Program Files / Users / the Windows tree) is refused even though it
    // is not a bare volume root: the generic recycle op routes the CANONICAL path through the same
    // file-deletion denylist the leftover-cleanup path uses, so it cannot target a shared system
    // location. Runs Unattended so the thunk's own guard (not the gate) rejects; the directory is
    // left untouched.
    void recycleRejectsProtectedSharedRoot() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString programFiles = qEnvironmentVariable("ProgramFiles",
                                                          QStringLiteral("C:\\Program Files"));
        const QJsonObject result =
            runArchiveOp(panel,
                         QStringLiteral("files.delete_to_recycle_bin"),
                         QJsonObject{{QStringLiteral("path"), programFiles}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("protected")));
        QVERIFY(QFile::exists(programFiles));  // untouched
    }

    // The model-supplied technician_override JSON flag ALONE must never bypass proof-of-scan. With
    // the out-of-band control (SAK_LEFTOVER_TECHNICIAN_OVERRIDE) unset, override=true is ignored
    // and an item no scan surfaced is still refused -- an injected flag cannot rubber-stamp a
    // fabricated deletion list. Runs Unattended (approval auto-hooked) so the thunk itself returns
    // the refusal.
    void cleanLeftoversOverrideFlagAloneCannotBypassProofOfScan() {
        qunsetenv("SAK_LEFTOVER_TECHNICIAN_OVERRIDE");
        sak::LeftoverScanProvenance::instance().clear();
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonArray items{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("file")},
                        {QStringLiteral("path"),
                         QStringLiteral("C:\\Program Files\\AcmeCorp\\App\\never-scanned.tmp")}}};
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("items"), items},
                                      {QStringLiteral("technician_override"), true}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("software.clean_leftovers")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(!data.value(QStringLiteral("unscanned")).toArray().isEmpty());
    }

    // ------------------------------------------------------------------
    // W2c: the CATASTROPHIC gate tier for app actions. A catastrophic action
    // (disk wipe / partition apply / image flash) must ALWAYS force an explicit
    // human confirm -- even in Unattended, where a merely-destructive action only
    // gets a restore-point offer. Exercised directly against appActionRunGate with
    // synthetic descriptors (no catastrophic op is registered yet).
    // ------------------------------------------------------------------

    static AppActionDescriptor riskyDescriptor(bool destructive, bool catastrophic) {
        AppActionDescriptor descriptor;
        descriptor.id = QStringLiteral("test.risky");
        descriptor.title = QStringLiteral("Test risky action");
        descriptor.category = QStringLiteral("test");
        descriptor.read_only = false;
        descriptor.mutating = true;
        descriptor.destructive = destructive;
        descriptor.requires_admin = false;
        descriptor.catastrophic = catastrophic;
        return descriptor;
    }

    // W2c(1): a catastrophic action in Unattended forces a confirm (not just a
    // restore-point offer); approving it lets the action proceed.
    void catastrophicAppActionForcesConfirmEvenInUnattended() {
        AiAssistantPanel panel;
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);  // approves "Approve AI Command"
        const std::optional<QJsonObject> blocked =
            panel.appActionRunGate(riskyDescriptor(/*destructive=*/true, /*catastrophic=*/true));
        QVERIFY(!blocked.has_value());  // allowed after the forced confirm
        QCOMPARE(countTitles(titles, QStringLiteral("Approve AI Command")), 1);
    }

    // W2c(2): a merely-destructive (non-catastrophic) action in Unattended keeps the
    // low-friction path -- a restore-point offer, no mandatory confirm.
    void destructiveNonCatastrophicUnattendedOffersRestoreNotConfirm() {
        AiAssistantPanel panel;
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const std::optional<QJsonObject> blocked =
            panel.appActionRunGate(riskyDescriptor(/*destructive=*/true, /*catastrophic=*/false));
        QVERIFY(!blocked.has_value());
        QCOMPARE(countTitles(titles, QStringLiteral("Approve AI Command")), 0);
        QCOMPARE(countTitles(titles, QStringLiteral("Create Restore Point")), 1);
    }

    // W2c(3): declining the forced confirm on a catastrophic action blocks it
    // (user_declined), even in Unattended.
    void catastrophicAppActionDeclinedBlocks() {
        AiAssistantPanel panel;
        setUnattended(panel);
        QStringList titles;
        // Decline every prompt (Cancel) so the forced confirm is refused.
        aiApprovalPromptTestHook() = [&titles](const QString& title, const QString&) -> int {
            titles.append(title);
            return 3;  // ApprovalPromptChoice::Cancel
        };
        const std::optional<QJsonObject> blocked =
            panel.appActionRunGate(riskyDescriptor(/*destructive=*/true, /*catastrophic=*/true));
        QVERIFY(blocked.has_value());
        QCOMPARE(blocked->value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("user_declined"));
        QVERIFY(countTitles(titles, QStringLiteral("Approve AI Command")) >= 1);
    }

    // W2c(4): the catastrophic flag is surfaced in the catalog so the model can see it.
    void catalogExposesCatastrophicFlag() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        QVERIFY(!actions.isEmpty());
        // Every catalog entry carries the (default-false) catastrophic field.
        for (const auto& value : actions) {
            QVERIFY(value.toObject().contains(QStringLiteral("catastrophic")));
        }
    }

    // ------------------------------------------------------------------
    // W2d: imaging.flash_image -- the first CATASTROPHIC op (overwrites a whole
    // physical disk). The flash EXECUTION cannot be unit-tested (needs a real
    // disk), so the SAFETY guard resolveFlashTarget is certified directly with
    // synthetic inventories, plus the descriptor flags + gating + arg guards.
    // ------------------------------------------------------------------

    static PartitionInventory inventoryWithDisk(uint32_t number,
                                                bool is_system,
                                                bool is_boot,
                                                bool is_read_only) {
        PartitionDiskInfo disk;
        disk.disk_number = number;
        disk.model = QStringLiteral("Test Disk");
        disk.bus_type = QStringLiteral("USB");
        disk.size_bytes = 8'000'000'000ULL;
        disk.is_removable = true;
        disk.is_system = is_system;
        disk.is_boot = is_boot;
        disk.is_read_only = is_read_only;
        PartitionInventory inventory;
        inventory.disks.append(disk);
        return inventory;
    }

    // W2d(1): the guard REFUSES the system disk, the boot disk, and a read-only disk,
    // and refuses a disk number that is not present -- fail closed.
    void resolveFlashTargetRefusesUnsafeDisks() {
        // System disk.
        QVERIFY(!resolveFlashTarget(inventoryWithDisk(0, true, false, false), 0).ok);
        // Boot disk.
        QVERIFY(!resolveFlashTarget(inventoryWithDisk(0, false, true, false), 0).ok);
        // Read-only disk.
        QVERIFY(!resolveFlashTarget(inventoryWithDisk(3, false, false, true), 3).ok);
        // Missing disk number.
        const FlashTargetResolution missing =
            resolveFlashTarget(inventoryWithDisk(1, false, false, false), 7);
        QVERIFY(!missing.ok);
        QVERIFY(missing.error.message.contains(QStringLiteral("No physical disk")));
        // Negative disk number.
        QVERIFY(!resolveFlashTarget(inventoryWithDisk(0, false, false, false), -1).ok);
    }

    // W2d(1b): fail CLOSED against the OS disk hiding behind degraded/absent Get-Disk
    // flags -- a warnings-tainted scan, a dynamic / Storage Spaces disk, or a disk
    // carrying an EFI/boot/system partition are all refused even when the disk-level
    // is_system/is_boot came back false.
    void resolveFlashTargetRefusesHiddenOsSignals() {
        // Degraded scan (warnings present) -> refuse regardless of the disk flags.
        {
            PartitionInventory inv = inventoryWithDisk(2, false, false, false);
            inv.warnings.append(QStringLiteral("enumeration degraded"));
            QVERIFY(!resolveFlashTarget(inv, 2).ok);
        }
        // Dynamic disk.
        {
            PartitionInventory inv = inventoryWithDisk(2, false, false, false);
            inv.disks[0].is_dynamic = true;
            QVERIFY(!resolveFlashTarget(inv, 2).ok);
        }
        // Storage Spaces disk.
        {
            PartitionInventory inv = inventoryWithDisk(2, false, false, false);
            inv.disks[0].is_storage_spaces = true;
            QVERIFY(!resolveFlashTarget(inv, 2).ok);
        }
        // Disk carries an EFI system partition (disk-level flags both false).
        {
            PartitionInventory inv = inventoryWithDisk(2, false, false, false);
            PartitionInfoEx efi;
            efi.is_efi = true;
            inv.disks[0].partitions.append(efi);
            QVERIFY(!resolveFlashTarget(inv, 2).ok);
        }
        // Disk carries a partition flagged as the boot partition.
        {
            PartitionInventory inv = inventoryWithDisk(2, false, false, false);
            PartitionInfoEx boot;
            boot.is_boot = true;
            inv.disks[0].partitions.append(boot);
            QVERIFY(!resolveFlashTarget(inv, 2).ok);
        }
    }

    // W2d(2): the guard ACCEPTS a present, non-system, non-boot, writable disk and
    // returns the exact device path + a human description.
    void resolveFlashTargetAcceptsSafeDisk() {
        const FlashTargetResolution resolved =
            resolveFlashTarget(inventoryWithDisk(2, false, false, false), 2);
        QVERIFY(resolved.ok);
        QCOMPARE(resolved.device_path, QStringLiteral("\\\\.\\PhysicalDrive2"));
        QVERIFY(resolved.description.contains(QStringLiteral("disk 2")));
    }

    // ------------------------------------------------------------------
    // W2f: partition.apply_operation. The APPLY execution is never unit-tested (needs
    // a real disk + admin diskpart), so the pure guard resolvePartitionApplyTarget is
    // certified directly with synthetic inventories, plus descriptor flags + gating +
    // arg guards.
    // ------------------------------------------------------------------

    // W2f(1): the guard refuses the same OS-disk denylist as flash (system/boot,
    // read-only, dynamic/Storage-Spaces, EFI-partition), plus missing/negative.
    void resolvePartitionApplyRefusesUnsafeDisks() {
        const QString hash = QStringLiteral("layout-abc");
        const auto inv = [&](uint32_t n, bool sys, bool boot, bool ro) {
            PartitionInventory i = inventoryWithDisk(n, sys, boot, ro);
            i.layout_hash = hash;
            return i;
        };
        QVERIFY(!resolvePartitionApplyTarget(inv(0, true, false, false), 0, hash).ok);  // system
        QVERIFY(!resolvePartitionApplyTarget(inv(0, false, true, false), 0, hash).ok);  // boot
        QVERIFY(!resolvePartitionApplyTarget(inv(3, false, false, true), 3, hash).ok);  // read-only
        QVERIFY(!resolvePartitionApplyTarget(inv(1, false, false, false), 7, hash).ok);  // missing
        QVERIFY(
            !resolvePartitionApplyTarget(inv(0, false, false, false), -1, hash).ok);     // negative
        {
            PartitionInventory i = inv(2, false, false, false);
            i.disks[0].is_dynamic = true;
            QVERIFY(!resolvePartitionApplyTarget(i, 2, hash).ok);
        }
        {
            PartitionInventory i = inv(2, false, false, false);
            i.disks[0].is_storage_spaces = true;
            QVERIFY(!resolvePartitionApplyTarget(i, 2, hash).ok);
        }
        {
            PartitionInventory i = inv(2, false, false, false);
            PartitionInfoEx efi;
            efi.is_efi = true;
            i.disks[0].partitions.append(efi);
            QVERIFY(!resolvePartitionApplyTarget(i, 2, hash).ok);
        }
    }

    // W2f(2): refuse a drifted layout (confirm hash != current) and a degraded scan.
    void resolvePartitionApplyRefusesDriftAndDegraded() {
        PartitionInventory current = inventoryWithDisk(2, false, false, false);
        current.layout_hash = QStringLiteral("current-hash");
        const PartitionApplyResolution drift =
            resolvePartitionApplyTarget(current, 2, QStringLiteral("stale-hash"));
        QVERIFY(!drift.ok);
        QVERIFY(drift.error.message.contains(QStringLiteral("changed since")));

        PartitionInventory degraded = inventoryWithDisk(2, false, false, false);
        degraded.layout_hash = QStringLiteral("h");
        degraded.warnings.append(QStringLiteral("enumeration degraded"));
        QVERIFY(!resolvePartitionApplyTarget(degraded, 2, QStringLiteral("h")).ok);
    }

    // W2f(3): accept a present, non-system, writable data disk when the layout hash
    // the model confirmed still matches the current inventory.
    void resolvePartitionApplyAcceptsSafeDisk() {
        PartitionInventory i = inventoryWithDisk(2, false, false, false);
        i.layout_hash = QStringLiteral("match");
        const PartitionApplyResolution r =
            resolvePartitionApplyTarget(i, 2, QStringLiteral("match"));
        QVERIFY(r.ok);
        QCOMPARE(r.device_path, QStringLiteral("\\\\.\\PhysicalDrive2"));
        QVERIFY(r.description.contains(QStringLiteral("disk 2")));
    }

    // W2f(4): registered destructive + CATASTROPHIC + requires_admin so the gate
    // forces a human confirm even in Unattended.
    void applyOperationListedCatastrophic() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("partition.apply_operation")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(action.value(QStringLiteral("catastrophic")).toBool());
                QVERIFY(action.value(QStringLiteral("requires_admin")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
            }
        }
        QVERIFY(found);
    }

    // W2f(4b): the exposed pure arg-validator fails CLOSED on wrong-typed / fractional byte
    // fields, a non-object payload, a present-but-non-bool dry_run, and a missing confirm hash --
    // so a mistyped destructive argument is refused before any disk work, never coerced to 0 /
    // truncated / a plan-to-apply escalation.
    void validatePartitionApplyArgsFailsClosed() {
        const auto base = []() {
            return QJsonObject{{QStringLiteral("disk_number"), 2},
                               {QStringLiteral("confirm_layout_hash"), QStringLiteral("h")}};
        };
        // A clean args object passes.
        QVERIFY(!sak::validatePartitionApplyArgs(base()).has_value());
        // Wrong-typed disk_number (string, not a JSON number).
        {
            QJsonObject a = base();
            a[QStringLiteral("disk_number")] = QStringLiteral("2");
            QVERIFY(sak::validatePartitionApplyArgs(a).has_value());
        }
        // Wrong-typed byte field (string) -> refused, never coerced to 0.
        {
            QJsonObject a = base();
            a[QStringLiteral("size_bytes")] = QStringLiteral("1024");
            QVERIFY(sak::validatePartitionApplyArgs(a).has_value());
        }
        // Fractional byte field -> refused, never truncated.
        {
            QJsonObject a = base();
            a[QStringLiteral("offset_bytes")] = 1024.5;
            QVERIFY(sak::validatePartitionApplyArgs(a).has_value());
        }
        // Non-object payload -> refused (never coerced to an empty {} that drops op fields).
        {
            QJsonObject a = base();
            a[QStringLiteral("payload")] = QStringLiteral("not-an-object");
            QVERIFY(sak::validatePartitionApplyArgs(a).has_value());
        }
        // Present-but-non-bool dry_run -> refused (never a silent false -> destructive apply).
        {
            QJsonObject a = base();
            a[QStringLiteral("dry_run")] = QStringLiteral("true");
            QVERIFY(sak::validatePartitionApplyArgs(a).has_value());
        }
        // Missing confirm_layout_hash -> refused (drift guard is mandatory).
        {
            const QJsonObject a{{QStringLiteral("disk_number"), 2}};
            QVERIFY(sak::validatePartitionApplyArgs(a).has_value());
        }
        // Well-formed integral byte field + real bool dry_run + object payload all pass.
        {
            QJsonObject a = base();
            a[QStringLiteral("size_bytes")] = 1048576.0;
            a[QStringLiteral("dry_run")] = true;
            a[QStringLiteral("payload")] =
                QJsonObject{{QStringLiteral("label"), QStringLiteral("X")}};
            QVERIFY(!sak::validatePartitionApplyArgs(a).has_value());
        }
    }

    // W2f(5): blocked in a chat/research session (never reaches the executor).
    void applyOperationBlockedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("partition.apply_operation")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"operation\":\"delete\",\"disk_number\":9,"
                                        "\"confirm_layout_hash\":\"h\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // W2f(6): once past the forced confirm, a missing confirm_layout_hash fails cleanly
    // (before any inventory scan) -- the drift guard cannot be skipped.
    void applyOperationMissingConfirmHashFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);  // approve the forced catastrophic confirm
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("partition.apply_operation")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"operation\":\"delete\",\"disk_number\":9}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("confirm_layout_hash")));
    }

    // W2f(7): an unsupported operation is refused before any disk work.
    void applyOperationRejectsUnsupportedType() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("partition.apply_operation")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"operation\":\"frobnicate\",\"disk_number\":9,"
                                        "\"confirm_layout_hash\":\"h\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("Unsupported")));
    }

    // W2g: a LIVE apply (dry_run false) is no longer blanket-refused -- it runs through
    // the same guard, which here rejects it (a bogus confirm_layout_hash trips the drift
    // guard before any disk is touched). The point: the guard is reached, not a blanket
    // "not yet available" refusal, and nothing is executed.
    void applyOperationLiveRunsThroughGuard() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);  // approve the forced catastrophic confirm
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("partition.apply_operation")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"operation\":\"delete\",\"disk_number\":9,"
                                        "\"confirm_layout_hash\":\"bogus\",\"dry_run\":false}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        const QString message = result.value(QStringLiteral("message")).toString();
        QVERIFY(!message.contains(QStringLiteral("not yet available")));
        QVERIFY(message.contains(QStringLiteral("Refusing to apply")));
    }

    // W2f(9): dry_run must be a real boolean -- a non-boolean value is rejected, never
    // coerced to false (which would silently select the live/destructive path).
    void applyOperationRejectsNonBooleanDryRun() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("partition.apply_operation")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"operation\":\"delete\",\"disk_number\":9,"
                                        "\"confirm_layout_hash\":\"h\",\"dry_run\":\"true\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("dry_run must be a boolean")));
    }

    // W2d(3): the descriptor is registered destructive + CATASTROPHIC + requires_admin
    // so the gate forces a human confirm even in Unattended.
    void flashImageListedCatastrophic() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("imaging.flash_image")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(action.value(QStringLiteral("catastrophic")).toBool());
                QVERIFY(action.value(QStringLiteral("requires_admin")).toBool());
            }
        }
        QVERIFY(found);
    }

    // W2d(4): blocked in a chat/research session (never reaches the disk enumeration).
    void flashImageBlockedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("image_path"), QStringLiteral("C:/x.img")},
                                      {QStringLiteral("disk_number"), 3}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("imaging.flash_image")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // W2d(5): a UNC/network image path is refused (guarded before any disk work).
    void flashImageRejectsNetworkImagePath() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);  // approve the forced catastrophic confirm
        const QString args =
            QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("image_path"),
                                                         QStringLiteral("\\\\host\\share\\x.img")},
                                                        {QStringLiteral("disk_number"), 3}})
                                  .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("imaging.flash_image")},
                        {QStringLiteral("arguments"), args}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("network")));
    }

    // W2d(6): missing args fail cleanly (no disk enumeration, no flash).
    void flashImageMissingArgsFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("imaging.flash_image")},
                        {QStringLiteral("arguments"), QStringLiteral("{\"disk_number\":3}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("image_path")));
    }

    // W2e: preview_operation with no 'operation' fails cleanly (never plans).
    void previewOperationMissingOperationFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("partition.preview_operation")},
            {QStringLiteral("arguments"), QStringLiteral("{\"disk_number\":0}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("operation")));
    }

    // W2e: an unsupported operation name is refused with the supported list, never
    // mapped to an enum by accident.
    void previewOperationRejectsUnsupportedType() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("partition.preview_operation")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"operation\":\"frobnicate\",\"disk_number\":0}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("Unsupported")));
    }

    // W2e: a valid operation type with no disk_number fails cleanly.
    void previewOperationMissingDiskNumberFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("partition.preview_operation")},
            {QStringLiteral("arguments"), QStringLiteral("{\"operation\":\"delete\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("disk_number")));
    }

    // W2e: preview drives the app's real PartitionOperationPlanner + safety
    // validator over a fresh no-elevation inventory. It is read-only and ungated,
    // so it runs even in a chat/research session, and a nonexistent target disk is
    // reported as BLOCKED (can_apply=false) -- a successful preview whose answer is
    // "not allowed", never a dispatch of any mutation.
    void previewOperationBlocksNonexistentDiskUngated() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("partition.preview_operation")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"operation\":\"delete\",\"disk_number\":99999,"
                            "\"partition_number\":1}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(!data.value(QStringLiteral("can_apply")).toBool());
        QVERIFY(data.value(QStringLiteral("blocker_count")).toInt() >= 1);
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("BLOCKED")));
    }

    // W2e: a partition-scoped op (format) is validated on the PARTITION path even
    // when partition_number is omitted -- so it is BLOCKED (partition/disk not
    // found), never falsely reported ALLOWED via the rule-less whole-disk path.
    // Deterministic on any machine: with disk 0 present the block is
    // "partition not found"; without it, "disk not found". Either way can_apply
    // must be false -- the old arg-inference bug reported can_apply=true here.
    void previewPartitionScopedOpWithoutPartitionIsBlocked() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("partition.preview_operation")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"operation\":\"format\",\"disk_number\":0}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(!data.value(QStringLiteral("can_apply")).toBool());
        QVERIFY(data.value(QStringLiteral("blocker_count")).toInt() >= 1);
    }

    // W2e: a negative size/offset is a malformed request, refused before any UB
    // floating-to-unsigned cast (never silently coerced to a huge value).
    void previewOperationRejectsNegativeByteCount() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("partition.preview_operation")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"operation\":\"create\",\"disk_number\":0,\"size_bytes\":-1}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("size_bytes")));
    }

    // W-uninstall: software.uninstall_uwp_app is listed mutating + destructive + requires_admin
    // and NOT catastrophic (a Store app is reinstallable; no disk wipe) and NOT read_only.
    void uninstallUwpListedMutatingDestructiveAdmin() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("software.uninstall_uwp_app")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(action.value(QStringLiteral("requires_admin")).toBool());
                QVERIFY(!action.value(QStringLiteral("catastrophic")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
            }
        }
        QVERIFY(found);
    }

    // W-uninstall: missing program_name fails cleanly (never enumerates or removes anything).
    void uninstallUwpMissingNameFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);  // proceeds past the destructive restore-point offer
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("software.uninstall_uwp_app")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("program_name")));
    }

    // W-uninstall: a mutating op is refused in a chat/research session (never enumerates).
    void uninstallUwpBlockedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("software.uninstall_uwp_app")},
            {QStringLiteral("arguments"), QStringLiteral("{\"program_name\":\"Whatever\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // W-uninstall: a name that matches no installed Store/UWP app fails cleanly after a REAL
    // enumeration (Get-AppxPackage). Deterministic: this bogus name is never installed, so the
    // resolve returns "not found" and no package is ever removed.
    void uninstallUwpNonexistentProgramFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("software.uninstall_uwp_app")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"program_name\":\"SAK No Such Store App ZZZ 9f3c\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("No installed")));
    }

    // W-uninstall (injection guard): isSafePackageFullName accepts a real Appx full name and
    // rejects any value that could break out of the single-quoted PowerShell -Package '...'
    // argument. The value passed to removal always comes from the system enumeration, but this
    // is the defense-in-depth barrier that certifies it.
    void packageFullNameValidatorRejectsInjection() {
        // Per-user (Get-AppxPackage) full name and provisioned (DISM PackageName, with a '~'
        // neutral ResourceId) are both accepted.
        QVERIFY(sak::isSafePackageFullName(
            QStringLiteral("Microsoft.WindowsCalculator_11.2.0.0_x64__8wekyb3d8bbwe")));
        QVERIFY(sak::isSafePackageFullName(
            QStringLiteral("Microsoft.BingWeather_4.53.51361.0_neutral_~_8wekyb3d8bbwe")));
        // Anything that could break out of the single-quoted -Package '<...>' argument is refused.
        QVERIFY(!sak::isSafePackageFullName(QString()));
        QVERIFY(!sak::isSafePackageFullName(QStringLiteral("app'; Remove-Item C:\\ -Recurse #")));
        QVERIFY(!sak::isSafePackageFullName(QStringLiteral("app name with spaces")));
        QVERIFY(!sak::isSafePackageFullName(QStringLiteral("app`whoami`")));
        QVERIFY(!sak::isSafePackageFullName(QStringLiteral("app$(calc)")));
        QVERIFY(!sak::isSafePackageFullName(QStringLiteral("safepart\ninjected")));  // trailing NL
    }

    // W-uninstall (Win32): software.uninstall_program is listed mutating + destructive +
    // requires_admin and NOT catastrophic and NOT read_only.
    void uninstallProgramListedMutatingDestructiveAdmin() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("software.uninstall_program")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(action.value(QStringLiteral("requires_admin")).toBool());
                QVERIFY(!action.value(QStringLiteral("catastrophic")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
            }
        }
        QVERIFY(found);
    }

    // W-uninstall (Win32): missing program_name fails cleanly (never enumerates or removes).
    void uninstallProgramMissingNameFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("software.uninstall_program")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("program_name")));
    }

    // W-uninstall (Win32): refused in a chat/research session (mutating; never enumerates).
    void uninstallProgramBlockedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("software.uninstall_program")},
            {QStringLiteral("arguments"), QStringLiteral("{\"program_name\":\"Whatever\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // W-uninstall (Win32): a name matching no installed Win32 program fails cleanly after a REAL
    // registry enumeration. Deterministic: this bogus name is never installed.
    void uninstallProgramNonexistentFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("software.uninstall_program")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"program_name\":\"SAK No Such Win32 Program ZZZ 9f3c\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("No installed")));
    }

    // W-net-dhcp: network.set_adapter_dhcp is listed mutating + requires_admin (netsh set needs
    // elevation), NOT destructive (reversible config change, no data loss), NOT catastrophic, NOT
    // read_only. Confirms the gate WILL fire but at the low-friction (Assisted-confirm) tier.
    void setAdapterDhcpListedMutatingAdmin() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("network.set_adapter_dhcp")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(action.value(QStringLiteral("requires_admin")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("catastrophic")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
            }
        }
        QVERIFY(found);
    }

    // W-net-dhcp: missing adapter_name fails cleanly (never touches netsh).
    void setAdapterDhcpMissingAdapterFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.set_adapter_dhcp")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("adapter_name")));
    }

    // W-net-dhcp: refused in a chat/research session (mutating; never runs netsh).
    void setAdapterDhcpBlockedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.set_adapter_dhcp")},
            {QStringLiteral("arguments"), QStringLiteral("{\"adapter_name\":\"Ethernet\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // W-net-dhcp: a name matching no system Ethernet adapter fails cleanly after a REAL adapter
    // enumeration -- the exact-match guard refuses it BEFORE any netsh set, so no config changes.
    // Deterministic + env-tolerant: this bogus name is never a real adapter (and on a host with no
    // dedicated adapter the list is empty), so it always fails with "No network adapter".
    void setAdapterDhcpUnknownAdapterFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.set_adapter_dhcp")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"adapter_name\":\"SAK No Such Adapter ZZZ 9f3c\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("No network adapter")));
    }

    // W-net-static: network.set_adapter_static_ip is listed mutating + requires_admin, NOT
    // destructive (reversible config change, no data loss), NOT catastrophic, NOT read_only --
    // same tier as set_adapter_dhcp (the gate fires at the Assisted-confirm tier).
    void setAdapterStaticIpListedMutatingAdmin() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("network.set_adapter_static_ip")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(action.value(QStringLiteral("requires_admin")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("catastrophic")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
            }
        }
        QVERIFY(found);
    }

    // W-net-static: missing required args (here ip_address) fails cleanly, never touching netsh.
    void setAdapterStaticIpMissingArgsFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.set_adapter_static_ip")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"adapter_name\":\"Ethernet\",\"subnet_mask\":\"255.255.255.0\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("ip_address")));
    }

    // W-net-static: a malformed IPv4 (validated up front) is refused BEFORE any adapter enumeration
    // or netsh call, with an honest error -- the defense-in-depth address validation.
    void setAdapterStaticIpInvalidIpFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.set_adapter_static_ip")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"adapter_name\":\"Ethernet\",\"ip_address\":\"999.1.2.3 extra\","
                            "\"subnet_mask\":\"255.255.255.0\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("ip_address")));
    }

    // W-net-static: refused in a chat/research session (mutating; never runs netsh).
    void setAdapterStaticIpBlockedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.set_adapter_static_ip")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"adapter_name\":\"Ethernet\",\"ip_address\":\"192.168.1.50\","
                            "\"subnet_mask\":\"255.255.255.0\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // W-net-static: a name matching no system adapter fails cleanly after a REAL enumeration -- the
    // exact-match guard refuses it before any netsh set, so no config changes. Deterministic +
    // env-tolerant (this bogus name is never a real adapter; the args are otherwise valid so the op
    // reaches resolution rather than failing arg validation).
    void setAdapterStaticIpUnknownAdapterFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.set_adapter_static_ip")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"adapter_name\":\"SAK No Such Adapter ZZZ 9f3c\",\"ip_address\":"
                            "\"192.168.1.50\",\"subnet_mask\":\"255.255.255.0\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("No network adapter")));
    }

    // W-net-static: a malformed dns_servers entry is refused (validated BEFORE any adapter
    // enumeration or netsh call). Deterministic + env-independent -- DNS validation runs ahead of
    // resolution, so it fails regardless of what adapters the host has.
    void setAdapterStaticIpInvalidDnsFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.set_adapter_static_ip")},
            {QStringLiteral("arguments"),
             QStringLiteral(
                 "{\"adapter_name\":\"Ethernet\",\"ip_address\":\"192.168.1.50\","
                 "\"subnet_mask\":\"255.255.255.0\",\"dns_servers\":[\"not-an-ip\"]}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("dns_servers")));
    }

    // W-net-dns: network.set_adapter_dns is listed mutating + requires_admin, NOT destructive
    // (reversible -- set_adapter_dhcp returns DNS to automatic), NOT catastrophic, NOT read_only --
    // same tier as the other adapter-admin ops (gate fires at the Assisted-confirm tier).
    void setAdapterDnsListedMutatingAdmin() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("network.set_adapter_dns")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(action.value(QStringLiteral("requires_admin")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("catastrophic")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
            }
        }
        QVERIFY(found);
    }

    // W-net-dns: refused in a chat/research session (mutating; never runs netsh).
    void setAdapterDnsBlockedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.set_adapter_dns")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"adapter_name\":\"Ethernet\",\"dns_servers\":[\"1.1.1.1\"]}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // W-net-dns: an empty/absent dns_servers list is refused up front (a static DNS op with no
    // servers is meaningless; set_adapter_dhcp is the way to return DNS to automatic). Env-
    // independent -- validated before any adapter enumeration or netsh call.
    void setAdapterDnsMissingDnsFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.set_adapter_dns")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"adapter_name\":\"Ethernet\",\"dns_servers\":[]}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("dns_servers")));
    }

    // W-net-dns: a malformed dns_servers entry is refused (validated BEFORE any adapter enumeration
    // or netsh call). Deterministic + env-independent.
    void setAdapterDnsInvalidDnsFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.set_adapter_dns")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"adapter_name\":\"Ethernet\",\"dns_servers\":"
                                        "[\"1.1.1.1\",\"not-an-ip\"]}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("dns_servers")));
    }

    // W-net-dns: a name matching no system adapter fails cleanly after a REAL enumeration -- the
    // exact-match guard refuses it before any netsh set (args otherwise valid so it reaches
    // resolution). Deterministic + env-tolerant (this bogus name is never a real adapter).
    void setAdapterDnsUnknownAdapterFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.set_adapter_dns")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"adapter_name\":\"SAK No Such Adapter ZZZ 9f3c\","
                                        "\"dns_servers\":[\"1.1.1.1\"]}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("No network adapter")));
    }

    // W-restore-point: diagnostics.create_restore_point is listed mutating + requires_admin (needs
    // elevation), NOT destructive (it ADDS a recovery checkpoint, removes nothing), NOT
    // catastrophic, NOT read_only -- the gate fires at the Assisted-confirm tier.
    void createRestorePointListedMutatingAdmin() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("diagnostics.create_restore_point")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(action.value(QStringLiteral("requires_admin")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("catastrophic")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
            }
        }
        QVERIFY(found);
    }

    // W-restore-point: refused in a chat/research session (mutating; never runs
    // Checkpoint-Computer).
    void createRestorePointBlockedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("diagnostics.create_restore_point")},
            {QStringLiteral("arguments"), QStringLiteral("{\"description\":\"Before update\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // W-restore-point: an empty/absent description is refused up front. This is the op-level guard
    // that keeps an empty string from reaching RestorePointManager::createRestorePoint, whose
    // Q_ASSERT(!description.isEmpty()) would abort a Debug build -- so the op MUST fail cleanly
    // BEFORE the engine. Env-independent (validated before any elevation/PowerShell).
    void createRestorePointEmptyDescriptionFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("diagnostics.create_restore_point")},
            {QStringLiteral("arguments"), QStringLiteral("{\"description\":\"   \"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("description")));
    }

    // W-restore-point: a description longer than the engine's 64-char cap is refused up front, so
    // the op never reports a full label the engine would silently truncate. Env-independent.
    void createRestorePointTooLongDescriptionFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QString long_desc(80, QLatin1Char('x'));  // > 64
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("diagnostics.create_restore_point")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"description\":\"%1\"}").arg(long_desc)}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("description")));
    }

    // W-flush-dns: network.flush_dns is listed mutating, NOT requires_admin (flushdns needs no
    // elevation), NOT destructive (the cache repopulates), NOT catastrophic, NOT read_only -- the
    // gate fires at the Assisted-confirm tier.
    void flushDnsListedMutating() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")}});
        const QJsonArray actions = result.value(QStringLiteral("actions")).toArray();
        bool found = false;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            if (action.value(QStringLiteral("id")).toString() ==
                QLatin1String("network.flush_dns")) {
                found = true;
                QVERIFY(action.value(QStringLiteral("mutating")).toBool());
                QVERIFY(!action.value(QStringLiteral("requires_admin")).toBool());
                QVERIFY(!action.value(QStringLiteral("destructive")).toBool());
                QVERIFY(!action.value(QStringLiteral("catastrophic")).toBool());
                QVERIFY(!action.value(QStringLiteral("read_only")).toBool());
            }
        }
        QVERIFY(found);
    }

    // W-flush-dns: refused in a chat/research session (mutating; never runs ipconfig).
    void flushDnsBlockedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.flush_dns")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("policy_blocked"));
    }

    // W-flush-dns: unlike the other mutating ops (which change adapters / write files / create a
    // real restore point), flushing the DNS cache is harmless + fast, so this actually EXECUTES it
    // in Unattended and asserts an honest success -- covering the real success path end to end
    // (ipconfig /flushdns is always present on Windows and needs no elevation).
    void flushDnsExecutesInUnattended() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.flush_dns")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("Flushed"), Qt::CaseInsensitive));
    }

    // W-uninstall (Win32): the silent-command builder is the guard that keeps a headless
    // uninstall from launching an interactive uninstaller. It accepts a publisher quiet command
    // and an MSI (building a /qn removal), and REFUSES a bare interactive uninstallString.
    void silentUninstallCommandBuilder() {
        QString cmd;

        sak::ProgramInfo quiet;
        quiet.quietUninstallString = QStringLiteral("\"C:\\App\\unins000.exe\" /SILENT");
        QVERIFY(sak::UninstallWorker::buildSilentUninstallCommand(quiet, cmd));
        QCOMPARE(cmd, QStringLiteral("\"C:\\App\\unins000.exe\" /SILENT"));

        sak::ProgramInfo msi;
        msi.uninstallString =
            QStringLiteral("MsiExec.exe /X{12345678-90AB-CDEF-1234-567890ABCDEF}");
        cmd.clear();
        QVERIFY(sak::UninstallWorker::buildSilentUninstallCommand(msi, cmd));
        QVERIFY(cmd.contains(QStringLiteral("/qn")));
        QVERIFY(cmd.contains(QStringLiteral("{12345678-90AB-CDEF-1234-567890ABCDEF}")));

        sak::ProgramInfo interactive;
        interactive.uninstallString = QStringLiteral("\"C:\\App\\setup.exe\" /uninstall");
        cmd.clear();
        QVERIFY(!sak::UninstallWorker::buildSilentUninstallCommand(interactive, cmd));
    }

    // W-uninstall (Win32): the pure resolution core over a raw (non-deduped) list. Certifies the
    // two safety guards the adversarial review added -- a system component is refused, and two
    // genuinely-distinct same-name programs are refused as ambiguous (never a wrong removal) --
    // plus the happy path (a name double-registered across hives with the SAME command resolves
    // to one) and the interactive/not-found refusals.
    void win32ResolutionCoreGuards() {
        auto msi = [](const QString& name, const QString& guid, bool system) {
            sak::ProgramInfo p;
            p.displayName = name;
            p.uninstallString = QStringLiteral("MsiExec.exe /X") + guid;
            p.isSystemComponent = system;
            return p;
        };
        const QString guid_a = QStringLiteral("{11111111-1111-1111-1111-111111111111}");
        const QString guid_b = QStringLiteral("{22222222-2222-2222-2222-222222222222}");
        sak::ProgramInfo out;

        // System component -> refused (never silently removed headless).
        QVERIFY(sak::resolveWin32ProgramFromList({msi(QStringLiteral("VC Runtime"), guid_a, true)},
                                                 QStringLiteral("VC Runtime"),
                                                 out)
                    .has_value());

        // Two distinct programs sharing a name (different GUIDs -> different commands) ->
        // ambiguous.
        const QVector<sak::ProgramInfo> distinct{msi(QStringLiteral("Updater"), guid_a, false),
                                                 msi(QStringLiteral("Updater"), guid_b, false)};
        const auto ambiguous =
            sak::resolveWin32ProgramFromList(distinct, QStringLiteral("Updater"), out);
        QVERIFY(ambiguous.has_value());
        QVERIFY(ambiguous->message.contains(QStringLiteral("distinct")));

        // Same program double-registered across hives (same GUID -> same command) -> resolves to
        // one.
        const QVector<sak::ProgramInfo> dup{msi(QStringLiteral("App"), guid_a, false),
                                            msi(QStringLiteral("App"), guid_a, false)};
        QVERIFY(!sak::resolveWin32ProgramFromList(dup, QStringLiteral("App"), out).has_value());

        // Interactive-only (no silent command) -> refused; missing name -> refused.
        sak::ProgramInfo interactive;
        interactive.displayName = QStringLiteral("Setupy");
        interactive.uninstallString = QStringLiteral("\"C:\\App\\setup.exe\" /uninstall");
        QVERIFY(sak::resolveWin32ProgramFromList({interactive}, QStringLiteral("Setupy"), out)
                    .has_value());
        QVERIFY(sak::resolveWin32ProgramFromList({}, QStringLiteral("Nope"), out).has_value());
    }

    // Wave 1 (network): list_adapters drives NetworkAdapterInspector (GetAdaptersAddresses:
    // local, no admin/network) and returns at least the loopback adapter -- deterministic
    // on any Windows host. Read-only, runs ungated.
    void listAdaptersReturnsAdapters() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.list_adapters")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(data.value(QStringLiteral("adapter_count")).toInt() >= 1);
        QVERIFY(!data.value(QStringLiteral("adapters")).toArray().isEmpty());
    }

    // Wave 1 (network): dns_query with no hostname fails cleanly (never queries).
    void dnsQueryMissingHostnameFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.dns_query")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("hostname")));
    }

    // Wave 1 (network): dns_query resolves "localhost" (served by the Windows resolver
    // from the hosts file / built-in, so no external network) -- the op succeeds and the
    // lookup itself succeeds. Runs ungated in a chat/research session.
    void dnsQueryResolvesLocalhost() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.dns_query")},
            {QStringLiteral("arguments"), QStringLiteral("{\"hostname\":\"localhost\"}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(data.value(QStringLiteral("success")).toBool());
    }

    // Wave 1 (network): list_connections enumerates the active TCP/UDP tables. The op always
    // completes (a machine may have any number of sockets, including zero after filtering), so
    // this asserts the structural invariant count == tcp_count + udp_count rather than a fixed
    // number. Local table read, no admin, no network -- deterministic completion.
    void listConnectionsSucceeds() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.list_connections")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(data.contains(QStringLiteral("connections")));
        const int count = data.value(QStringLiteral("count")).toInt();
        const int tcp = data.value(QStringLiteral("tcp_count")).toInt();
        const int udp = data.value(QStringLiteral("udp_count")).toInt();
        QCOMPARE(count, tcp + udp);
    }

    // Wave 1 (network): with both TCP and UDP suppressed the enumeration returns nothing --
    // a deterministic zero-connection success (proves the show_tcp/show_udp config threads
    // through startMonitoring to the engine's refreshNow).
    void listConnectionsTcpUdpOffReturnsEmpty() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.list_connections")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"show_tcp\":false,\"show_udp\":false}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("count")).toInt(), 0);
        QVERIFY(data.value(QStringLiteral("connections")).toArray().isEmpty());
    }

    // Wave 1 (network): list_connections runs UNGATED in a Chat & Research session (read-only,
    // local enumeration mutating nothing), like list_adapters/dns_query.
    void listConnectionsRunsUngatedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.list_connections")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
    }

    // Wave 1 (network): audit_firewall enumerates Windows Firewall rules via INetFwPolicy2 COM
    // (the engine self-initialises COM, so it works on any thread) and runs conflict + gap
    // analysis. Every Windows host ships default rules, so this asserts >=1 rule AND the
    // no-filter invariant matched_rules == total_rules (every rule passes an empty filter).
    void auditFirewallSucceeds() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.audit_firewall")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(data.value(QStringLiteral("total_rules")).toInt() >= 1);
        QCOMPARE(data.value(QStringLiteral("matched_rules")).toInt(),
                 data.value(QStringLiteral("total_rules")).toInt());
        QVERIFY(data.contains(QStringLiteral("conflicts")));
        QVERIFY(data.contains(QStringLiteral("gaps")));
    }

    // Wave 1 (network): a name_filter that matches nothing returns zero rules WITHOUT hiding
    // the fact that the host has rules (total_rules stays >=1) -- proves the filter trims only
    // the returned array, while conflict/gap analysis still runs over the full policy.
    void auditFirewallNameFilterTrimsRulesOnly() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.audit_firewall")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"name_filter\":\"zzz_no_such_rule_zzz_9137\"}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("matched_rules")).toInt(), 0);
        QVERIFY(data.value(QStringLiteral("rules")).toArray().isEmpty());
        QVERIFY(data.value(QStringLiteral("total_rules")).toInt() >= 1);
    }

    // Wave 1 (network): audit_firewall runs UNGATED in a Chat & Research session (read-only COM
    // enumeration, mutates nothing), like the sibling network read ops.
    void auditFirewallRunsUngatedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.audit_firewall")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
    }

    // Wave 1 (network): wifi_scan. The result is HARDWARE-dependent (a host with no WiFi radio,
    // like a CI runner or a desktop, has no networks and may report the adapter unavailable), so
    // this certifies the dispatch + result SHAPE + honest failure classification rather than a
    // fixed network count. Either a success with a well-formed networks array, or an honest
    // failure whose message names the WiFi adapter -- never a malformed result or a crash.
    void wifiScanReturnsWellFormedResult() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.wifi_scan")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.contains(QStringLiteral("success")));
        if (result.value(QStringLiteral("success")).toBool()) {
            const QJsonObject data = result.value(QStringLiteral("data")).toObject();
            QVERIFY(data.contains(QStringLiteral("networks")));
            QVERIFY(data.value(QStringLiteral("network_count")).toInt() >= 0);
            QVERIFY(data.contains(QStringLiteral("channels")));
        } else {
            QVERIFY(result.value(QStringLiteral("message"))
                        .toString()
                        .contains(QStringLiteral("WiFi"), Qt::CaseInsensitive));
        }
    }

    // Wave 1 (network): wifi_scan is read-only, so it runs UNGATED in a Chat & Research session
    // -- it must never be POLICY-blocked (no failure_class), regardless of whether the scan
    // itself succeeds or reports the adapter unavailable (an operational failure, not a gate).
    void wifiScanRunsUngatedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.wifi_scan")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
    }

    // Network: list_wifi_profiles enumerates the machine's SAVED WiFi profiles (distinct from
    // wifi_scan, which scans nearby APs). ENVIRONMENT-dependent: a host with no WLAN service (CI
    // runner, wired desktop) cannot enumerate and reports an HONEST failure (fail-closed) rather
    // than a misleading "0 profiles" success. Certifies dispatch + SHAPE + honesty + the security
    // invariant that NO key material is surfaced: each listed profile carries only name and
    // security_type, never xml_data / keyMaterial / a "key" field.
    void listWifiProfilesReturnsWellFormedAndKeyless() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.list_wifi_profiles")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.contains(QStringLiteral("success")));
        if (result.value(QStringLiteral("success")).toBool()) {
            const QJsonObject data = result.value(QStringLiteral("data")).toObject();
            QVERIFY(data.contains(QStringLiteral("profiles")));
            QVERIFY(data.value(QStringLiteral("profile_count")).toInt() >= 0);
            for (const auto& entry : data.value(QStringLiteral("profiles")).toArray()) {
                const QJsonObject profile = entry.toObject();
                QVERIFY(profile.contains(QStringLiteral("profile_name")));
                QVERIFY(profile.contains(QStringLiteral("security_type")));
                // Security invariant: no re-importable key material ever reaches the model.
                QVERIFY(!profile.contains(QStringLiteral("xml_data")));
                QVERIFY(!profile.contains(QStringLiteral("keyMaterial")));
                QVERIFY(!profile.contains(QStringLiteral("key")));
            }
        } else {
            QVERIFY(result.value(QStringLiteral("message"))
                        .toString()
                        .contains(QStringLiteral("WiFi"), Qt::CaseInsensitive));
        }
    }

    // Network: list_wifi_profiles is read-only, so it runs UNGATED in a Chat & Research session --
    // never POLICY-blocked, whether or not the enumeration itself succeeds.
    void listWifiProfilesRunsUngatedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.list_wifi_profiles")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
    }

    // Diagnostics: list_restore_points drives RestorePointManager (Get-ComputerRestorePoint via a
    // no-shell bounded process). The outcome is ENVIRONMENT/elevation-dependent: a non-elevated
    // process (like the test runner) may not be able to read the restore-point store, in which
    // case the op reports an HONEST failure (fail-closed) rather than a misleading "0 restore
    // points" success. So this certifies the dispatch + SHAPE + honesty: either a success with
    // the state fields (system_restore_enabled/elevated/count/restore_points), or a failure whose
    // message names restore points -- never a malformed result, and never a confident empty from
    // a failed query.
    void listRestorePointsReturnsWellFormed() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("diagnostics.list_restore_points")},
            {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.contains(QStringLiteral("success")));
        if (result.value(QStringLiteral("success")).toBool()) {
            const QJsonObject data = result.value(QStringLiteral("data")).toObject();
            QVERIFY(data.contains(QStringLiteral("system_restore_enabled")));
            QVERIFY(data.contains(QStringLiteral("elevated")));
            QVERIFY(data.value(QStringLiteral("count")).toInt() >= 0);
            QVERIFY(data.contains(QStringLiteral("restore_points")));
        } else {
            QVERIFY(result.value(QStringLiteral("message"))
                        .toString()
                        .contains(QStringLiteral("restore points"), Qt::CaseInsensitive));
        }
    }

    // Diagnostics: list_restore_points is read-only, so it runs UNGATED in a Chat & Research
    // session -- never POLICY-blocked (no failure_class), whether or not the query itself
    // succeeds (an unreadable store is an operational failure, not a gate).
    void listRestorePointsRunsUngatedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("diagnostics.list_restore_points")},
            {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
    }

    // Diagnostics: read_temperatures drives ThermalMonitor::pollOnce (a bounded no-shell
    // powershell sensor poll). The outcome is ENVIRONMENT/elevation-dependent (a VM / desktop /
    // non-elevated process may expose no readable sensors; some sensors need admin), so this
    // certifies the dispatch + SHAPE + honesty: either a success with the sensors array + counts
    // (possibly zero, phrased as "no readable sensors"), or an honest failure whose message names
    // the sensor query -- never a malformed result. sensor_count == reported when under the cap.
    void readTemperaturesReturnsWellFormed() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("diagnostics.read_temperatures")},
            {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.contains(QStringLiteral("success")));
        if (result.value(QStringLiteral("success")).toBool()) {
            const QJsonObject data = result.value(QStringLiteral("data")).toObject();
            QVERIFY(data.contains(QStringLiteral("sensors")));
            QVERIFY(data.value(QStringLiteral("sensor_count")).toInt() >= 0);
        } else {
            QVERIFY(result.value(QStringLiteral("message"))
                        .toString()
                        .contains(QStringLiteral("thermal"), Qt::CaseInsensitive));
        }
    }

    // Diagnostics: read_temperatures is read-only -> UNGATED in a Chat & Research session
    // (never policy-blocked), whether or not sensors are readable.
    void readTemperaturesRunsUngatedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("diagnostics.read_temperatures")},
            {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
    }

    // System: list_users drives WindowsUserScanner (NetUserEnum). Env-tolerant: on an elevated
    // host it succeeds with a well-formed user array (>=1: the current account has a profile); on
    // a NON-elevated host the detailed USER_INFO_3 enumeration can be access-denied -> the op must
    // fail HONESTLY (never a fake "0 users" success). Either way the shape is asserted, so the
    // fail-open honesty hole stays closed and the test never flakes on the runner's elevation.
    void listUsersReturnsWellFormed() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("system.list_users")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.contains(QStringLiteral("success")));
        if (result.value(QStringLiteral("success")).toBool()) {
            const QJsonObject data = result.value(QStringLiteral("data")).toObject();
            QVERIFY(data.contains(QStringLiteral("users")));
            QVERIFY(data.value(QStringLiteral("count")).toInt() >= 0);
            QCOMPARE(data.value(QStringLiteral("users")).toArray().size(),
                     data.value(QStringLiteral("reported_count")).toInt());
        } else {
            QVERIFY(result.value(QStringLiteral("message"))
                        .toString()
                        .contains(QStringLiteral("user account"), Qt::CaseInsensitive));
        }
    }

    // System: list_users is read-only -> UNGATED in a Chat & Research session (never
    // policy-blocked), whether or not the enumeration succeeds.
    void listUsersRunsUngatedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("system.list_users")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
    }

    // Network: list_shares enumerates the LOCAL machine's shares read-only (no write probe).
    // Env-tolerant: NetShareEnum on the local machine succeeds with a well-formed share array
    // (shape asserted, NO fixed count -- a non-admin host may see fewer shares); on failure the op
    // must fail HONESTLY (never a fake "0 shares" success). access_tested is always false (no
    // probe). On success the message states "the local machine".
    void listSharesLocalReturnsWellFormed() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.list_shares")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(result.contains(QStringLiteral("success")));
        if (result.value(QStringLiteral("success")).toBool()) {
            const QJsonObject data = result.value(QStringLiteral("data")).toObject();
            QVERIFY(data.contains(QStringLiteral("shares")));
            QVERIFY(data.value(QStringLiteral("count")).toInt() >= 0);
            QCOMPARE(data.value(QStringLiteral("access_tested")).toBool(), false);
            QCOMPARE(data.value(QStringLiteral("shares")).toArray().size(),
                     data.value(QStringLiteral("reported_count")).toInt());
        } else {
            QVERIFY(result.value(QStringLiteral("message"))
                        .toString()
                        .contains(QStringLiteral("share"), Qt::CaseInsensitive));
        }
    }

    // Network: list_shares is LOCAL-ONLY by design (no target argument). A prompt-injected model
    // that supplies a hostname must NOT be able to redirect enumeration at a remote host -- the
    // arg is structurally ignored (never reaches NetShareEnum). Proven by: an injected public
    // hostname yields the SAME local behavior (success-with-shape OR honest failure), and the
    // message always refers to "the local machine"/"share", never the injected host.
    void listSharesIgnoresInjectedHostname() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.list_shares")},
            {QStringLiteral("arguments"), QStringLiteral("{\"hostname\":\"8.8.8.8\"}")}});
        QVERIFY(result.contains(QStringLiteral("success")));
        const QString message = result.value(QStringLiteral("message")).toString();
        QVERIFY(
            !message.contains(QStringLiteral("8.8.8.8")));  // never enumerated the injected host
        if (result.value(QStringLiteral("success")).toBool()) {
            QVERIFY(message.contains(QStringLiteral("local machine"), Qt::CaseInsensitive));
        } else {
            QVERIFY(message.contains(QStringLiteral("share"), Qt::CaseInsensitive));
        }
    }

    // Network: list_shares is read-only -> UNGATED in a Chat & Research session (never
    // policy-blocked), whether or not enumeration succeeds.
    void listSharesRunsUngatedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.list_shares")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
    }

    // Wave 1 (network probes): ping with no target fails cleanly (never sends).
    void pingMissingTargetFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.ping")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("target")));
    }

    // Wave 1 (network probes): ping the loopback (127.0.0.1 -- always reachable, no external
    // network, no admin). The op succeeds AND every echo is received (0% loss). Drives the
    // real NetworkProbeWorker -> ConnectivityTester through AsyncActionInvocation, so this
    // also exercises the worker-thread + timeout + cancel plumbing. Deterministic on Windows.
    void pingLoopbackSucceeds() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.ping")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"target\":\"127.0.0.1\",\"count\":2,\"interval_ms\":0,"
                                        "\"timeout_ms\":1000}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("sent")).toInt(), 2);
        QCOMPARE(data.value(QStringLiteral("received")).toInt(), 2);
        QCOMPARE(data.value(QStringLiteral("loss_percent")).toDouble(), 0.0);
    }

    // Wave 1 (network probes): ping runs UNGATED in a Chat & Research session (read-only),
    // exactly like dns_query -- it must never be policy-blocked or gated.
    void pingRunsUngatedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.ping")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"target\":\"127.0.0.1\",\"count\":1,\"interval_ms\":0}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
    }

    // Wave 1 (network probes): traceroute with no target fails cleanly.
    void tracerouteMissingTargetFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.traceroute")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("target")));
    }

    // Wave 1 (network probes): traceroute to the loopback reaches the target in the first
    // hop (ttl=1 echo to 127.0.0.1 replies from 127.0.0.1). Deterministic, no network.
    void tracerouteLoopbackReachesTarget() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.traceroute")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"target\":\"127.0.0.1\",\"max_hops\":5,\"probes_per_hop\":1,"
                            "\"timeout_ms\":1000,\"resolve_hostnames\":false}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QVERIFY(data.value(QStringLiteral("reached_target")).toBool());
    }

    // Wave 1 (network probes): mtr with no target fails cleanly.
    void mtrMissingTargetFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.mtr")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("target")));
    }

    // Wave 1 (network probes): mtr against the loopback runs the requested cycles and reports
    // hop 1 (127.0.0.1) with 0% loss. total_cycles == the requested cycle count because each
    // cycle sends exactly one echo to the first (and only) hop. Deterministic, no network, no
    // admin -- also exercises the continuous-run worker path through AsyncActionInvocation.
    void mtrLoopbackSucceeds() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.mtr")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"target\":\"127.0.0.1\",\"cycles\":2,\"max_hops\":5,"
                                        "\"timeout_ms\":1000,\"interval_ms\":0}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("total_cycles")).toInt(), 2);
        QVERIFY(data.value(QStringLiteral("hop_count")).toInt() >= 1);
        const QJsonArray hops = data.value(QStringLiteral("hops")).toArray();
        QVERIFY(!hops.isEmpty());
        QCOMPARE(hops.first().toObject().value(QStringLiteral("loss_percent")).toDouble(), 0.0);
    }

    // Wave 1 (network probes): mtr against a resolvable-but-unreachable literal reports an
    // HONEST failure, not a cheerful "0 hops, 0.0% loss" success. 0.0.0.0 resolves locally
    // (numeric literal) but IcmpSendEcho rejects the unspecified destination immediately, so
    // no hop is ever discovered -> empty hop list -> the unreachable guard fires. Deterministic
    // and instant (no timeout wait, no real network). Regression guard for the honesty fix.
    void mtrUnreachableTargetReportsFailure() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.mtr")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"target\":\"0.0.0.0\",\"cycles\":1,\"max_hops\":2,"
                                        "\"timeout_ms\":500,\"interval_ms\":0}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("unreachable")));
    }

    // Wave 1 (network probes): mtr runs UNGATED in a Chat & Research session (read-only), like
    // ping/traceroute -- it must never be policy-blocked or gated.
    void mtrRunsUngatedInChatSession() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(0);  // Chat & Research (no execution)
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.mtr")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"target\":\"127.0.0.1\",\"cycles\":1,\"max_hops\":3,"
                                        "\"timeout_ms\":1000,\"interval_ms\":0}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QVERIFY(!result.contains(QStringLiteral("failure_class")));  // never gated
    }

    // Wave 1 (network probes): port_scan with no target fails cleanly.
    void portScanMissingTargetFails() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.port_scan")},
                        {QStringLiteral("arguments"), QStringLiteral("{}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("target")));
    }

    // Wave 1 (network probes): port_scan with a target but NO ports (neither an explicit
    // list nor a valid range) fails before touching the network.
    void portScanRequiresPorts() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.port_scan")},
            {QStringLiteral("arguments"), QStringLiteral("{\"target\":\"127.0.0.1\"}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("ports")));
    }

    // Wave 1 (network probes): the assistant's port scanner is restricted to local/private
    // targets. A public IP is refused before any connection attempt -- an active scan of an
    // arbitrary third-party host is exactly the prompt-injection abuse vector we bound out.
    void portScanRefusesPublicTarget() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.port_scan")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"target\":\"8.8.8.8\",\"ports\":[443]}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(
            result.value(QStringLiteral("message")).toString().contains(QStringLiteral("local")));
    }

    // Wave 1 (network probes): a hostname (which could resolve anywhere) is refused too --
    // the scanner requires a private IP literal or "localhost".
    void portScanRefusesHostnameTarget() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.port_scan")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"target\":\"example.com\",\"ports\":[80]}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
    }

    // Wave 1 (network probes): the sequential scan is hard-capped so an unbounded port range
    // cannot become an unbounded wall time. A 1-200 range exceeds the 128-port cap and is
    // refused before any connection attempt.
    void portScanRejectsTooManyPorts() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("network.port_scan")},
                        {QStringLiteral("arguments"),
                         QStringLiteral("{\"target\":\"127.0.0.1\",\"port_range_start\":1,"
                                        "\"port_range_end\":200}")}});
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("message")).toString().contains(QStringLiteral("128")));
    }

    // Wave 1 (network probes): scanning one closed loopback port succeeds as an OP (the
    // port's state is closed/filtered, but a completed scan is a successful op like a failed
    // DNS lookup) and reports exactly one scanned port. No external network. Small timeout.
    void portScanLoopbackReportsResult() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        const QJsonObject result = panel.runAppActionTool(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("run")},
            {QStringLiteral("action_id"), QStringLiteral("network.port_scan")},
            {QStringLiteral("arguments"),
             QStringLiteral("{\"target\":\"127.0.0.1\",\"ports\":[1],\"timeout_ms\":500,"
                            "\"grab_banners\":false}")}});
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        const QJsonObject data = result.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("scanned_count")).toInt(), 1);
        QCOMPARE(data.value(QStringLiteral("results")).toArray().size(), 1);
    }

    // REAL-HARDWARE guard probe (opt-in: set SAK_LIVE_DISK_PROBE). Runs the actual
    // no-elevation inventory scan and prints what resolveFlashTarget /
    // resolvePartitionApplyTarget decide for EVERY physical disk on this machine, then
    // asserts the running OS/system disk is REFUSED by both. Skipped by default so CI
    // and normal runs stay deterministic and touch no hardware.
    void liveGuardProbeAgainstRealDisks() {
        if (!qEnvironmentVariableIsSet("SAK_LIVE_DISK_PROBE")) {
            QSKIP("Set SAK_LIVE_DISK_PROBE to run the real-hardware guard probe");
        }
        const PartitionInventory inventory = StorageInventoryWorker::scanCurrentSystem(false);
        qInfo("=== LIVE guard probe: %lld disk(s), layout_hash=%s, warnings=%lld ===",
              static_cast<long long>(inventory.disks.size()),
              qUtf8Printable(inventory.layout_hash),
              static_cast<long long>(inventory.warnings.size()));
        for (const PartitionDiskInfo& disk : inventory.disks) {
            const int number = static_cast<int>(disk.disk_number);
            const FlashTargetResolution flash = resolveFlashTarget(inventory, number);
            const PartitionApplyResolution apply =
                resolvePartitionApplyTarget(inventory, number, inventory.layout_hash);
            qInfo("disk %d [%s] sys=%d boot=%d ro=%d parts=%lld | flash=%s (%s) | apply=%s (%s)",
                  number,
                  qUtf8Printable(disk.model),
                  disk.is_system,
                  disk.is_boot,
                  disk.is_read_only,
                  static_cast<long long>(disk.partitions.size()),
                  flash.ok ? "ACCEPT" : "REFUSE",
                  qUtf8Printable(flash.ok ? flash.device_path : flash.error.message),
                  apply.ok ? "ACCEPT" : "REFUSE",
                  qUtf8Printable(apply.ok ? apply.device_path : apply.error.message));
            if (disk.is_system || disk.is_boot) {
                QVERIFY2(!flash.ok, "flash guard must REFUSE the OS/system disk");
                QVERIFY2(!apply.ok, "apply guard must REFUSE the OS/system disk");
            }
        }
    }

    // REAL flash-execution cert (opt-in + DESTRUCTIVE): set SAK_LIVE_FLASH=<image path>
    // and SAK_LIVE_FLASH_DISK=<number> of a DISPOSABLE, already-cleared disk. Runs the
    // actual FlashCoordinator (the flash_image engine) with SHA-512 verification against
    // real hardware. Must run ELEVATED (raw physical-disk write). QSKIP by default; this
    // OVERWRITES the target disk, so it is never run in CI and only against a disk the
    // guard accepts (a leftover EFI/system partition is refused first).
    void liveFlashCertToDisposableDisk() {
        const QString image = qEnvironmentVariable("SAK_LIVE_FLASH");
        if (image.isEmpty()) {
            QSKIP("Set SAK_LIVE_FLASH=<image> and SAK_LIVE_FLASH_DISK=<n> to run (DESTRUCTIVE)");
        }
        const int disk_number = qEnvironmentVariable("SAK_LIVE_FLASH_DISK", "999").toInt();
        const PartitionInventory inventory = StorageInventoryWorker::scanCurrentSystem(false);
        const FlashTargetResolution target = resolveFlashTarget(inventory, disk_number);
        QVERIFY2(target.ok,
                 qUtf8Printable(QStringLiteral("guard refused disk %1: %2")
                                    .arg(disk_number)
                                    .arg(target.error.message)));
        qInfo("=== LIVE flash: %s -> %s (SHA-512 verify on) ===",
              qUtf8Printable(image),
              qUtf8Printable(target.device_path));

        FlashCoordinator coordinator;
        coordinator.setVerificationEnabled(true);
        bool done = false;
        bool ok = false;
        qint64 bytes = 0;
        QString detail;
        QObject::connect(&coordinator,
                         &FlashCoordinator::flashCompleted,
                         &coordinator,
                         [&](const sak::FlashResult& result) {
                             ok = result.success && !result.hasErrors();
                             bytes = result.bytesWritten;
                             detail = result.errorMessages.join(QStringLiteral("; "));
                             if (!result.sourceChecksum.isEmpty()) {
                                 detail += QStringLiteral(" sha512=") + result.sourceChecksum;
                             }
                             done = true;
                         });
        QObject::connect(
            &coordinator, &FlashCoordinator::flashError, &coordinator, [&](const QString& error) {
                detail = error;
                done = true;
            });
        QVERIFY(coordinator.startFlash(image, QStringList{target.device_path}));
        QTRY_VERIFY_WITH_TIMEOUT(done, 600'000);  // up to 10 min
        qInfo("=== flash result: ok=%d bytes=%lld detail=%s ===",
              ok,
              static_cast<long long>(bytes),
              qUtf8Printable(detail));
        QVERIFY2(ok,
                 qUtf8Printable(QStringLiteral("flash failed/verify mismatch: %1").arg(detail)));
    }

    // REAL apply-execution cert (opt-in + DESTRUCTIVE): set SAK_LIVE_APPLY_DISK=<number>
    // of a DISPOSABLE disk the guard accepts. Resolves the target the same way the op does,
    // then drives the REAL PartitionApplyWorker (worker thread -> PartitionExecutor ->
    // elevated diskpart) with an initialize_disk operation. Must run ELEVATED. QSKIP by
    // default; this WRITES a GPT to the target disk, so it is never run in CI.
    void liveApplyCertToDisposableDisk() {
        if (!qEnvironmentVariableIsSet("SAK_LIVE_APPLY_DISK")) {
            QSKIP("Set SAK_LIVE_APPLY_DISK=<n> to run the live apply cert (DESTRUCTIVE)");
        }
        const int disk_number = qEnvironmentVariable("SAK_LIVE_APPLY_DISK").toInt();
        const PartitionInventory inventory = StorageInventoryWorker::scanCurrentSystem(false);
        const PartitionApplyResolution target =
            resolvePartitionApplyTarget(inventory, disk_number, inventory.layout_hash);
        QVERIFY2(target.ok,
                 qUtf8Printable(QStringLiteral("guard refused disk %1: %2")
                                    .arg(disk_number)
                                    .arg(target.error.message)));
        qInfo("=== LIVE apply: initialize_disk on %s ===", qUtf8Printable(target.description));

        PartitionTarget pt;
        pt.kind = PartitionTargetKind::Disk;
        pt.disk_number = static_cast<uint32_t>(disk_number);
        const PartitionOperation op = PartitionOperationPlanner::makeOperation(
            PartitionOperationType::InitializeDisk, pt, QJsonObject{});

        PartitionApplyWorker worker(QVector<PartitionOperation>{op},
                                    /*dry_run=*/false,
                                    /*use_elevation=*/true);
        bool done = false;
        bool ok = false;
        QString detail;
        QObject::connect(&worker, &WorkerBase::finished, &worker, [&]() {
            const PartitionExecutionResult& result = worker.result();
            ok = result.success;
            detail = result.message;
            if (!result.steps.isEmpty()) {
                detail += QStringLiteral(" | ") + result.steps.first().error_message;
            }
            done = true;
        });
        QObject::connect(&worker, &WorkerBase::failed, &worker, [&](int, const QString& error) {
            detail = error;
            done = true;
        });
        QObject::connect(&worker, &WorkerBase::cancelled, &worker, [&]() {
            detail = QStringLiteral("cancelled");
            done = true;
        });
        worker.start();
        QTRY_VERIFY_WITH_TIMEOUT(done, 600'000);  // up to 10 min
        qInfo("=== apply result: ok=%d detail=%s ===", ok, qUtf8Printable(detail));
        QVERIFY2(ok, qUtf8Printable(QStringLiteral("live apply failed: %1").arg(detail)));
    }

    void cleanup() {
        // Never leave the approval seam installed for the next case or production.
        aiApprovalPromptTestHook() = nullptr;
    }

    // Wave 1b: a catastrophic/irreversible command must get an explicit human
    // confirmation gate even in Unattended mode -- it cannot silently auto-run.
    void catastrophicCommandForcesConfirmEvenInUnattended() {
        AiAssistantPanel panel;
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        ai::OpenAIFunctionCall call;
        call.name = QStringLiteral("run_powershell");
        AiAssistantPanel::PendingToolCallContext ctx;
        ctx.call = &call;
        ai::OpenAIFunctionOutput output;
        const bool proceeded =
            panel.authorizeCommandForAccessMode(ctx, makePlan(true, true), &output);

        QVERIFY(proceeded);
        QVERIFY(countTitles(titles, QStringLiteral("Approve AI Command")) == 1);
        QVERIFY(countTitles(titles, QStringLiteral("Create Restore Point")) == 1);
    }

    // A merely-risky (non-catastrophic) command in Unattended mode keeps the
    // low-friction behavior: no mandatory confirm, and the restore point is
    // offered only once per session.
    void nonCatastrophicRiskyInUnattendedSkipsConfirmAndOffersOnce() {
        AiAssistantPanel panel;
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        ai::OpenAIFunctionCall call;
        call.name = QStringLiteral("run_powershell");
        AiAssistantPanel::PendingToolCallContext ctx;
        ctx.call = &call;
        ai::OpenAIFunctionOutput output;
        QVERIFY(panel.authorizeCommandForAccessMode(ctx, makePlan(true, false), &output));
        QVERIFY(panel.authorizeCommandForAccessMode(ctx, makePlan(true, false), &output));

        QCOMPARE(countTitles(titles, QStringLiteral("Approve AI Command")), 0);
        QCOMPARE(countTitles(titles, QStringLiteral("Create Restore Point")), 1);
    }

    // The once-per-session restore-point dedup must NOT suppress the offer for
    // catastrophic commands: a single early restore point cannot be allowed to
    // leave later disk wipes unprotected.
    void catastrophicCommandReOffersRestorePointDespiteSessionFlag() {
        AiAssistantPanel panel;
        setUnattended(panel);
        panel.m_restorePointOfferedThisSession = true;  // an earlier op already offered
        QStringList titles;
        installApprovalHook(&titles);

        ai::OpenAIFunctionCall call;
        call.name = QStringLiteral("run_powershell");
        AiAssistantPanel::PendingToolCallContext ctx;
        ctx.call = &call;
        ai::OpenAIFunctionOutput output;
        QVERIFY(panel.authorizeCommandForAccessMode(ctx, makePlan(true, true), &output));
        QVERIFY(panel.authorizeCommandForAccessMode(ctx, makePlan(true, true), &output));

        QCOMPARE(countTitles(titles, QStringLiteral("Approve AI Command")), 2);
        QCOMPARE(countTitles(titles, QStringLiteral("Create Restore Point")), 2);
    }

    // The catastrophic gate must also cover WORKFLOW tool phases: a catastrophic
    // phase (disk format) requires an explicit human confirm even in Unattended,
    // mirroring the command-tool gate. Previously authorizeWorkflowToolPhase
    // ignored catastrophic_change, so such a phase got only a skippable restore
    // point offer in Unattended.
    void catastrophicWorkflowPhaseForcesConfirmEvenInUnattended() {
        AiAssistantPanel panel;
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        ai::WorkflowPhase phase;
        phase.id = QStringLiteral("p1");
        phase.tool = QStringLiteral("run_powershell");
        AiAssistantPanel::WorkflowToolDispatchPlan plan;
        plan.policy = ai::AiToolPolicy::MutatingRequiresLease;
        plan.request.tool_name = QStringLiteral("run_powershell");
        // Classified catastrophic by the policy regex (disk format), so
        // evaluateToolPolicy sets decision.catastrophic_change.
        plan.request.command_preview = QStringLiteral("format c: /y");
        plan.ok = true;

        QVERIFY(panel.authorizeWorkflowToolPhase(phase, plan));
        QCOMPARE(countTitles(titles, QStringLiteral("Approve AI Command")), 1);
    }

    // A merely-risky (non-catastrophic) workflow phase in Unattended keeps the
    // low-friction path: no mandatory confirm, just the restore-point offer.
    void nonCatastrophicRiskyWorkflowPhaseInUnattendedSkipsConfirm() {
        AiAssistantPanel panel;
        setUnattended(panel);
        QStringList titles;
        installApprovalHook(&titles);

        ai::WorkflowPhase phase;
        phase.id = QStringLiteral("p2");
        phase.tool = QStringLiteral("run_powershell");
        AiAssistantPanel::WorkflowToolDispatchPlan plan;
        plan.policy = ai::AiToolPolicy::MutatingRequiresLease;
        plan.request.tool_name = QStringLiteral("run_powershell");
        plan.request.command_preview = QStringLiteral("Restart-Service Spooler");
        plan.phase_risky = true;  // gate is needed, but the command is not catastrophic
        plan.ok = true;

        QVERIFY(panel.authorizeWorkflowToolPhase(phase, plan));
        QCOMPARE(countTitles(titles, QStringLiteral("Approve AI Command")), 0);
        QCOMPARE(countTitles(titles, QStringLiteral("Create Restore Point")), 1);
    }

    // Teardown safety: once shutdown has begun, the per-phase modal gates must
    // decline immediately WITHOUT showing a modal. Otherwise a workflow worker
    // blocked marshaling an approval onto the GUI thread would have that modal
    // shown by drainWorkflowRun's event-loop pump and block on exec() forever
    // (no user answers during teardown). The approval hook stands in for the
    // modal, so a zero hook count proves no modal was ever opened.
    void shutdownDeclinesPerPhaseModalWithoutShowingIt() {
        AiAssistantPanel panel;
        QStringList titles;
        installApprovalHook(&titles);
        panel.m_shuttingDown = true;

        QVERIFY(!panel.confirmCommandWithUser(
            QStringLiteral("PowerShell"), QStringLiteral("format c: /y"), true, true));
        QVERIFY(!panel.offerRestorePointIfNeeded(QStringLiteral("format c: /y"), true, true));
        QCOMPARE(titles.size(), 0);
    }

    // Wave 2: repeated executor faults (hangs) trip the health circuit breaker for
    // a command tool, and the raw-command health gate then suppresses new calls
    // instead of launching into a known-bad executor.
    void healthCircuitBreakerSuppressesCommandTool() {
        AiAssistantPanel panel;
        resetHealthLedger(panel);
        const QString key = AiAssistantPanel::commandHealthKey(QStringLiteral("run_powershell"));
        panel.m_currentCommandHealthKey = key;

        ai::OpenAIFunctionCall call;
        call.name = QStringLiteral("run_powershell");
        AiAssistantPanel::PendingToolCallContext ctx;
        ctx.call = &call;
        ai::AiCommandToolPlan plan;
        plan.policy_request.tool_name = QStringLiteral("run_powershell");

        // Healthy to start: the gate lets the command through.
        ai::OpenAIFunctionOutput allow_output;
        QVERIFY(panel.applyCommandHealthGate(ctx, plan, &allow_output));

        // Three executor hangs trip the breaker...
        ai::AiCommandResult timed_out;
        timed_out.started = true;
        timed_out.timed_out = true;
        for (int i = 0; i < 3; ++i) {
            panel.recordCommandHealth(timed_out);
        }
        QVERIFY(!panel.m_toolHealthLedger->check(key).available);

        // ...and the gate now suppresses new calls (returns false).
        ai::OpenAIFunctionOutput deny_output;
        QVERIFY(!panel.applyCommandHealthGate(ctx, plan, &deny_output));
    }

    // A command that ran to completion is a healthy executor even if the command
    // itself exited nonzero -- that must not trip the breaker.
    void nonZeroExitKeepsCommandToolHealthy() {
        AiAssistantPanel panel;
        resetHealthLedger(panel);
        const QString key = AiAssistantPanel::commandHealthKey(QStringLiteral("run_cmd"));
        panel.m_currentCommandHealthKey = key;

        ai::AiCommandResult ran;
        ran.started = true;
        ran.exit_code = 1;
        for (int i = 0; i < 5; ++i) {
            panel.recordCommandHealth(ran);
        }
        QVERIFY(panel.m_toolHealthLedger->check(key).available);
    }

    // A user Stop is not an executor-health signal; cancels never trip the breaker.
    void cancelledCommandDoesNotTripBreaker() {
        AiAssistantPanel panel;
        resetHealthLedger(panel);
        const QString key = AiAssistantPanel::commandHealthKey(QStringLiteral("run_cmd"));
        panel.m_currentCommandHealthKey = key;

        ai::AiCommandResult cancelled;
        cancelled.started = true;
        cancelled.cancelled = true;
        for (int i = 0; i < 5; ++i) {
            panel.recordCommandHealth(cancelled);
        }
        QVERIFY(panel.m_toolHealthLedger->check(key).available);
    }

    // Wave 3: the sak_skill tool lists the catalog (no bodies), loads a full body
    // on demand by id, and reports an error for an unknown id.
    void skillToolListsLoadsAndRejectsUnknown() {
        AiAssistantPanel panel;
        QVERIFY(panel.m_skillStore != nullptr);
        const ai::Skill skill = ai::Skill::fromMarkdown(
            QByteArray("---\nid: cert-skill\ndescription: cert desc.\nwhen_to_use: x\n---\n"
                       "# Cert Skill\nthe full guidance body\n"),
            QStringLiteral("/x/cert-skill.md"));
        QVERIFY(panel.m_skillStore->addSkill(skill));

        const QJsonObject list =
            panel.runSkillTool(QJsonObject{{QStringLiteral("operation"), QStringLiteral("list")},
                                           {QStringLiteral("skill_id"), QString()}});
        QVERIFY(list.value(QStringLiteral("success")).toBool());
        const QJsonArray catalog = list.value(QStringLiteral("skills")).toArray();
        bool found = false;
        for (const auto& entry : catalog) {
            const QJsonObject obj = entry.toObject();
            if (obj.value(QStringLiteral("id")).toString() == QLatin1String("cert-skill")) {
                found = true;
                QVERIFY(!obj.contains(QStringLiteral("body")));  // list never leaks bodies
            }
        }
        QVERIFY(found);

        const QJsonObject loaded = panel.runSkillTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("load")},
                        {QStringLiteral("skill_id"), QStringLiteral("cert-skill")}});
        QVERIFY(loaded.value(QStringLiteral("success")).toBool());
        QVERIFY(loaded.value(QStringLiteral("body"))
                    .toString()
                    .contains(QStringLiteral("the full guidance body")));

        const QJsonObject unknown = panel.runSkillTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("load")},
                        {QStringLiteral("skill_id"), QStringLiteral("does-not-exist")}});
        QVERIFY(!unknown.value(QStringLiteral("success")).toBool());
    }

    // End-to-end regression: a model-issued sak_skill call must travel the real
    // chat tool loop (router classify -> dispatchBuiltInToolCall -> dispatcher ->
    // handler). If the router misclassifies sak_skill as Unknown, the loop answers
    // "Unknown function" and the handler never runs -- so asserting the handler
    // fired proves the whole chain is wired, which the direct-call test cannot.
    // The model's delegate_subagent tool must route through the real chat tool
    // loop (router -> dispatcher -> handler) and actually run a scoped sub-agent.
    // Uses an injected fake model client so no network is touched.
    void delegateSubagentToolSpawnsScopedSubagentThroughRealLoop() {
        std::atomic<bool> subagent_invoked{false};
        AiAssistantPanel panel;
        panel.m_delegateSubagentModelFactoryOverride = [&subagent_invoked]() {
            return std::make_unique<FakeSubagentModelClient>(&subagent_invoked);
        };
        allowLocalToolsAndValidRun(panel);

        panel.beginToolTurn(delegateResponse());

        // delegate_subagent is an async built-in: it dispatched off the GUI thread,
        // ran the sub-agent runner, and invoked the sub-agent's (fake) model client.
        QTRY_VERIFY_WITH_TIMEOUT(subagent_invoked.load(), 5000);
    }

    // run_workflow must fail closed on a missing or unknown workflow id before it
    // commits to any orchestration.
    void runWorkflowToolValidatesWorkflowId() {
        AiAssistantPanel panel;

        const QJsonObject no_id = panel.runRunWorkflowTool(QJsonObject{});
        QCOMPARE(no_id.value(QStringLiteral("success")).toBool(), false);
        QVERIFY(no_id.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(QStringLiteral("workflow_id")));

        const QJsonObject unknown = panel.runRunWorkflowTool(
            QJsonObject{{QStringLiteral("workflow_id"), QStringLiteral("does_not_exist_xyz")}});
        QVERIFY(unknown.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(QStringLiteral("Unknown workflow_id")));
    }

    // A sub-agent must never be able to delegate to another sub-agent or launch a
    // workflow -- a structural backstop beyond the executor allowlist, so
    // orchestration can never recurse.
    void subagentCannotDelegateOrRunWorkflow() {
        AiAssistantPanel panel;
        ai::AiToolCallRequest request;
        request.tool_name = QStringLiteral("delegate_subagent");
        const QJsonObject delegated = panel.dispatchSubagentToolCall(
            ai::AiToolPolicy::ReadOnlyPc, request, QJsonObject{}, QStringLiteral("sub"));
        QVERIFY(delegated.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(QStringLiteral("may not delegate")));

        request.tool_name = QStringLiteral("run_workflow");
        const QJsonObject workflowed = panel.dispatchSubagentToolCall(
            ai::AiToolPolicy::ReadOnlyPc, request, QJsonObject{}, QStringLiteral("sub"));
        QVERIFY(workflowed.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(QStringLiteral("may not delegate")));
    }

    // A workflow phase likewise may not use the orchestration tools (an authored
    // template could otherwise recurse unboundedly).
    void workflowPhaseCannotUseOrchestrationTools() {
        AiAssistantPanel panel;
        ai::WorkflowPhase phase;
        phase.id = QStringLiteral("p1");
        phase.type = QStringLiteral("tool_action");
        phase.tool = QStringLiteral("run_workflow");
        ai::AiWorkflowPhaseContext context;
        const QJsonObject result =
            panel.dispatchWorkflowToolPhase(phase, ai::AiToolPolicy::ReadOnlyPc, context);
        QVERIFY(result.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(QStringLiteral("may not use")));
    }

    void skillToolReachesHandlerThroughRealToolLoop() {
        AiAssistantPanel panel;
        std::atomic<bool> handler_ran{false};
        panel.m_toolDispatcher->registerHandler(
            QStringLiteral("sak_skill"),
            [&handler_ran](const QJsonObject& args, const ai::AiToolPolicyDecision&) {
                handler_ran = true;
                return QJsonObject{{QStringLiteral("success"), true},
                                   {QStringLiteral("operation"),
                                    args.value(QStringLiteral("operation")).toString()}};
            });
        allowLocalToolsAndValidRun(panel);

        panel.beginToolTurn(skillResponse());

        // sak_skill is a synchronous built-in, dispatched inline in the tool loop.
        QTRY_VERIFY_WITH_TIMEOUT(handler_ran.load(), 5000);
    }

    // Wave 5: the loop guard breaks a stuck model that re-emits the SAME tool with
    // byte-identical arguments. With the default threshold of 4, the first three
    // identical calls dispatch to the handler; the fourth is refused (handler not
    // invoked) and a corrective output is fed back instead -- long before the flat
    // per-message turn cap would burn the whole budget.
    void loopGuardRefusesIdenticalRepeatedToolCall() {
        AiAssistantPanel panel;
        std::atomic<int> handler_calls{0};
        panel.m_toolDispatcher->registerHandler(
            QStringLiteral("sak_skill"),
            [&handler_calls](const QJsonObject&, const ai::AiToolPolicyDecision&) {
                handler_calls.fetch_add(1);
                return QJsonObject{{QStringLiteral("success"), true}};
            });
        allowLocalToolsAndValidRun(panel);

        // One response carrying four identical sak_skill calls; the loop is
        // processed inline because sak_skill is a synchronous built-in.
        panel.beginToolTurn(repeatedSkillResponse(4, /*identical=*/true));

        QCOMPARE(handler_calls.load(), 3);  // 4th refused before dispatch
        QVERIFY(panel.m_toolLoopDetector.isLooping());
        QCOMPARE(panel.m_toolLoopDetector.maxRepeatCount(),
                 ai::AiToolLoopDetector::kDefaultRepeatThreshold);
    }

    // The guard keys on tool name PLUS arguments: distinct arguments are distinct
    // work and must never be mistaken for a loop, even well past the threshold.
    void loopGuardAllowsDistinctArgumentsPastThreshold() {
        AiAssistantPanel panel;
        std::atomic<int> handler_calls{0};
        panel.m_toolDispatcher->registerHandler(
            QStringLiteral("sak_skill"),
            [&handler_calls](const QJsonObject&, const ai::AiToolPolicyDecision&) {
                handler_calls.fetch_add(1);
                return QJsonObject{{QStringLiteral("success"), true}};
            });
        allowLocalToolsAndValidRun(panel);

        panel.beginToolTurn(repeatedSkillResponse(6, /*identical=*/false));

        QCOMPARE(handler_calls.load(), 6);  // every distinct call dispatched
        QVERIFY(!panel.m_toolLoopDetector.isLooping());
    }

    // Regression (adversarial-review finding): an identical repeat of an
    // UNRECOGNIZED tool name is the most wasteful loop (no useful work at all), so
    // the guard must observe it too rather than letting the "Unknown function"
    // path run all the way to the 12-turn hard cap. The detector must therefore
    // trip even though the tool is never dispatched.
    void loopGuardCountsUnrecognizedRepeatedToolCall() {
        AiAssistantPanel panel;
        allowLocalToolsAndValidRun(panel);

        // "no_such_tool" maps to AiToolCallKind::Unknown, so every call takes the
        // unrecognized path; the loop guard sits ahead of that split now.
        panel.beginToolTurn(repeatedNamedResponse(QStringLiteral("no_such_tool"),
                                                  4,
                                                  /*identical=*/true));

        QVERIFY(panel.m_toolLoopDetector.isLooping());
        QCOMPARE(panel.m_toolLoopDetector.maxRepeatCount(),
                 ai::AiToolLoopDetector::kDefaultRepeatThreshold);
    }

    // CODEX_REVIEW_2 line 100: a non-batch app action's confirm preview must render
    // its destructive target FULLY, never truncated behind a global char cap that
    // could hide the target off the end. Drive a real registered mutating action in
    // Assisted mode with an over-long path argument and assert the full path reaches
    // the human confirm text (which the old 200-char JSON truncation would have cut).
    void nonBatchAppActionConfirmShowsFullTarget() {
        AiAssistantPanel panel;
        panel.ensureAppActionService();
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(1);  // Assisted Full Access -> confirms
        // A path far longer than the retired 200-char cap; a marker at the very end
        // proves nothing was truncated.
        const QString long_path = QStringLiteral("C:\\Temp\\") + QString(400, QLatin1Char('a')) +
                                  QStringLiteral("\\END");
        QString captured_detail;
        aiApprovalPromptTestHook() = [&captured_detail](const QString& title,
                                                        const QString& detail) -> int {
            if (title.contains(QStringLiteral("Approve AI Command"))) {
                captured_detail = detail;
            }
            return 3;  // Cancel -- capture the preview, never invoke the action
        };
        const QString args = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("sources"), QJsonArray{QStringLiteral("x")}},
                                      {QStringLiteral("output_path"), long_path}})
                .toJson(QJsonDocument::Compact));
        const QJsonObject result = panel.runAppActionTool(
            QJsonObject{{QStringLiteral("operation"), QStringLiteral("run")},
                        {QStringLiteral("action_id"), QStringLiteral("files.compress_zip")},
                        {QStringLiteral("arguments"), args}});
        QCOMPARE(result.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("user_declined"));     // declined at the confirm, never invoked
        aiApprovalPromptTestHook() = nullptr;
        QVERIFY(!captured_detail.isEmpty());           // the confirm fired
        QVERIFY(captured_detail.contains(long_path));  // full target, not truncated
    }

    // CODEX_REVIEW_2 line 182: the workflow tool-dispatch plan must carry the
    // originating user_message into its policy request, or package/MCP phases
    // over-block. Assert initializeWorkflowToolPlan propagates context.user_message.
    void workflowToolPlanCarriesUserMessage() {
        AiAssistantPanel panel;
        ai::WorkflowPhase phase;
        phase.id = QStringLiteral("p1");
        phase.tool = QStringLiteral("sak_package_manager");
        phase.operation = QStringLiteral("search");
        phase.agent = QStringLiteral("pkg-agent");  // avoid the policy-reclamp branch
        ai::AiWorkflowPhaseContext context;
        context.user_message = QStringLiteral("install 7zip for the customer");
        AiAssistantPanel::WorkflowToolDispatchPlan plan;
        panel.initializeWorkflowToolPlan(phase, ai::AiToolPolicy::PackageToolsOnly, context, &plan);
        QCOMPARE(plan.request.user_message, context.user_message);
    }

private:
    // A single response carrying `count` calls of an arbitrary tool `name`. When
    // `identical`, every call has byte-identical arguments (trips the loop guard);
    // otherwise each carries a distinct index so no two signatures match.
    static ai::OpenAIResponseResult repeatedNamedResponse(const QString& name,
                                                          int count,
                                                          bool identical) {
        ai::OpenAIResponseResult response;
        response.id = QStringLiteral("resp_named_1");
        for (int i = 0; i < count; ++i) {
            ai::OpenAIFunctionCall call;
            call.call_id = QStringLiteral("call_named_%1").arg(i);
            call.name = name;
            const QString value = identical ? QStringLiteral("same")
                                            : QStringLiteral("v_%1").arg(i);
            call.arguments_json = QStringLiteral("{\"x\":\"%1\"}").arg(value);
            response.function_calls.append(call);
        }
        return response;
    }

    // A single response carrying `count` sak_skill calls. When `identical`, every
    // call has byte-identical arguments (trips the loop guard); otherwise each
    // carries a distinct skill_id so no two signatures match.
    static ai::OpenAIResponseResult repeatedSkillResponse(int count, bool identical) {
        ai::OpenAIResponseResult response;
        response.id = QStringLiteral("resp_loop_1");
        for (int i = 0; i < count; ++i) {
            ai::OpenAIFunctionCall call;
            call.call_id = QStringLiteral("call_loop_%1").arg(i);
            call.name = QStringLiteral("sak_skill");
            const QString skill_id = identical ? QStringLiteral("same")
                                               : QStringLiteral("id_%1").arg(i);
            call.arguments_json =
                QStringLiteral("{\"operation\":\"load\",\"skill_id\":\"%1\"}").arg(skill_id);
            response.function_calls.append(call);
        }
        return response;
    }

    // Give the panel a fresh, non-persistent health ledger so a prior run's
    // persisted circuit state (test-mode data dir) cannot leak into these breaker
    // assertions. applyCommandHealthGate/recordCommandHealth use m_toolHealthLedger.
    static void resetHealthLedger(AiAssistantPanel& panel) {
        panel.m_toolHealthLedger = std::make_unique<ai::AiToolHealthLedger>();
    }

    // Unattended Full Access (combo index 2) -- the mode where risky commands run
    // without a per-command confirm unless the new catastrophic gate fires.
    static void setUnattended(AiAssistantPanel& panel) {
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(2);
    }

    static ai::AiCommandToolPlan makePlan(bool risky, bool catastrophic) {
        ai::AiCommandToolPlan plan;
        plan.shell_label = QStringLiteral("PowerShell");
        plan.preview = catastrophic ? QStringLiteral("format C: /y")
                                    : QStringLiteral("choco upgrade all -y");
        plan.risky_change = risky;
        plan.policy_decision.allowed = true;
        plan.policy_decision.risky_change = risky;
        plan.policy_decision.catastrophic_change = catastrophic;
        return plan;
    }

    // Scripts the modal approval seam: approve command confirmations, and choose
    // "Proceed Without" for restore-point offers (records the title, never creates
    // a real restore point). Records every prompt title it is shown.
    static void installApprovalHook(QStringList* titles) {
        aiApprovalPromptTestHook() = [titles](const QString& title, const QString&) -> int {
            titles->append(title);
            if (title.contains(QStringLiteral("Approve AI Command"))) {
                return 0;  // ApprovalPromptChoice::Accept
            }
            if (title.contains(QStringLiteral("Create Restore Point"))) {
                return 1;  // ApprovalPromptChoice::Secondary (Proceed Without)
            }
            return 3;      // ApprovalPromptChoice::Cancel
        };
    }

    static int countTitles(const QStringList& titles, const QString& needle) {
        int count = 0;
        for (const auto& title : titles) {
            if (title.contains(needle)) {
                ++count;
            }
        }
        return count;
    }

    static ai::OpenAIResponseResult downloadResponse() {
        ai::OpenAIFunctionCall call;
        call.call_id = QStringLiteral("call_1");
        call.name = QStringLiteral("download_file");
        call.arguments_json =
            QStringLiteral("{\"url\":\"https://example.com/f\",\"filename\":\"f\"}");
        ai::OpenAIResponseResult response;
        response.id = QStringLiteral("resp_1");
        response.function_calls.append(call);
        return response;
    }

    static ai::OpenAIResponseResult skillResponse() {
        ai::OpenAIFunctionCall call;
        call.call_id = QStringLiteral("call_skill_1");
        call.name = QStringLiteral("sak_skill");
        call.arguments_json = QStringLiteral("{\"operation\":\"list\",\"skill_id\":\"\"}");
        ai::OpenAIResponseResult response;
        response.id = QStringLiteral("resp_skill_1");
        response.function_calls.append(call);
        return response;
    }

    static ai::OpenAIResponseResult delegateResponse() {
        ai::OpenAIFunctionCall call;
        call.call_id = QStringLiteral("call_delegate_1");
        call.name = QStringLiteral("delegate_subagent");
        call.arguments_json = QStringLiteral(
            "{\"objective\":\"check disk space\",\"tool_policy\":\"read_only_pc\","
            "\"expected_output\":\"summary\"}");
        ai::OpenAIResponseResult response;
        response.id = QStringLiteral("resp_delegate_1");
        response.function_calls.append(call);
        return response;
    }

    // Assisted Full Access (combo index 1) allows local tools with a lease; a
    // valid root run token makes Stop/cancel meaningful.
    static void allowLocalToolsAndValidRun(AiAssistantPanel& panel) {
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(1);
        panel.m_runToken = ai::CancellationToken::createRoot(QStringLiteral("test_run"));
    }

    // Replace the real download handler with one that blocks on the test's gate,
    // recording the thread it ran on.
    static void installBlockingDownloadHandler(AiAssistantPanel& panel,
                                               std::atomic<bool>* entered,
                                               std::atomic<Qt::HANDLE>* handler_thread,
                                               QSemaphore* gate) {
        panel.m_toolDispatcher->registerHandler(
            QStringLiteral("download_file"),
            [entered, handler_thread, gate](const QJsonObject&, const ai::AiToolPolicyDecision&) {
                *handler_thread = QThread::currentThreadId();
                *entered = true;
                gate->acquire();
                return QJsonObject{{QStringLiteral("success"), true},
                                   {QStringLiteral("path"), QStringLiteral("f")}};
            });
    }
};

}  // namespace sak

QTEST_MAIN(sak::AiAssistantPanelToolDispatchTest)
#include "test_ai_assistant_panel_tool_dispatch.moc"
