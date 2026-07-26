// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_probe_worker.h
/// @brief Runs a single blocking ConnectivityTester/PortScanner probe on its own
/// worker thread so a headless ping/traceroute/port-scan is bounded and cancellable.

#pragma once

#include "sak/connectivity_tester.h"
#include "sak/network_diagnostic_types.h"
#include "sak/port_scanner.h"
#include "sak/worker_base.h"

#include <QString>
#include <QVector>

namespace sak {

/// Wraps ONE blocking network-probe engine call -- ConnectivityTester::ping /
/// ::traceroute or PortScanner::scan -- in a WorkerBase thread.
///
/// The assistant's read-only ping/traceroute/port_scan ops must not call the BLOCKING
/// engine directly on the tool worker thread: against an unreachable target a run can
/// block for its full config-bounded duration (tens of seconds to minutes) with no
/// reachable Stop, and the tool thread has no event loop to pump. Running it here makes
/// it drivable exactly like the search/apply paths: connect finished()/failed()/
/// cancelled() to an AsyncActionInvocation, and the bounded WorkerBase shutdown
/// (requestStop + wait + terminate) plus the cooperative engine cancel() (forwarded
/// from cancelExecution() and the destructor) cap how long a run can hold the thread.
///
/// The relevant *Complete signal is captured via a Qt::DirectConnection installed on the
/// worker thread inside execute(), so the result is written synchronously before execute()
/// returns; the finished() slot on the caller thread then reads it happens-after.
class NetworkProbeWorker : public WorkerBase {
    Q_OBJECT

public:
    enum class Kind {
        Ping,
        Traceroute,
        PortScan
    };

    explicit NetworkProbeWorker(ConnectivityTester::PingConfig config, QObject* parent = nullptr);
    explicit NetworkProbeWorker(ConnectivityTester::TracerouteConfig config,
                                QObject* parent = nullptr);
    explicit NetworkProbeWorker(PortScanner::ScanConfig config, QObject* parent = nullptr);
    ~NetworkProbeWorker() override;

    /// Cooperatively cancel an in-flight run: requests WorkerBase stop AND forwards to the
    /// engines' cancel() (thread-safe atomic flags). Safe from any thread; a no-op if idle.
    void cancelExecution();

    [[nodiscard]] Kind kind() const noexcept { return m_kind; }

    /// True once the engine's *Complete signal was captured. A completed run with a
    /// non-empty error() is still a failure (resolve failure emits both).
    [[nodiscard]] bool captured() const noexcept { return m_captured; }
    [[nodiscard]] const QString& error() const noexcept { return m_error; }

    /// Results are valid only after finished()/cancelled() has been delivered.
    [[nodiscard]] const PingResult& pingResult() const noexcept { return m_ping; }
    [[nodiscard]] const TracerouteResult& tracerouteResult() const noexcept { return m_trace; }
    [[nodiscard]] const QVector<PortScanResult>& portScanResult() const noexcept { return m_ports; }

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    void runPing();
    void runTraceroute();
    void runPortScan();

    Kind m_kind;
    ConnectivityTester m_tester;
    PortScanner m_scanner;
    ConnectivityTester::PingConfig m_pingConfig;
    ConnectivityTester::TracerouteConfig m_traceConfig;
    PortScanner::ScanConfig m_scanConfig;

    bool m_captured = false;
    QString m_error;
    PingResult m_ping;
    TracerouteResult m_trace;
    QVector<PortScanResult> m_ports;
};

}  // namespace sak
