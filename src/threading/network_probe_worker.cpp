// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_probe_worker.cpp
/// @brief Worker-thread wrapper around one blocking network-probe engine call.

#include "sak/network_probe_worker.h"

#include "sak/layout_constants.h"

#include <utility>

namespace sak {

NetworkProbeWorker::NetworkProbeWorker(ConnectivityTester::PingConfig config, QObject* parent)
    : WorkerBase(parent), m_kind(Kind::Ping), m_pingConfig(std::move(config)) {}

NetworkProbeWorker::NetworkProbeWorker(ConnectivityTester::TracerouteConfig config, QObject* parent)
    : WorkerBase(parent), m_kind(Kind::Traceroute), m_traceConfig(std::move(config)) {}

NetworkProbeWorker::NetworkProbeWorker(PortScanner::ScanConfig config, QObject* parent)
    : WorkerBase(parent), m_kind(Kind::PortScan), m_scanConfig(std::move(config)) {}

NetworkProbeWorker::NetworkProbeWorker(ConnectivityTester::MtrConfig config, QObject* parent)
    : WorkerBase(parent), m_kind(Kind::Mtr), m_mtrConfig(std::move(config)) {}

NetworkProbeWorker::~NetworkProbeWorker() {
    // Join the thread HERE, while m_tester/m_scanner are still alive. WorkerBase's own
    // destructor runs AFTER this body and AFTER the engine members are destroyed, so if the
    // join were left to it the still-running execute() would use a dead engine (UAF).
    // Bounded: cooperative cancel first, then wait, then force-terminate as a backstop.
    // cancel() is harmless on the idle engine.
    //
    // The cancel atomic is polled between each echo/hop/port, so the ping/traceroute
    // send loop and the sequential port scan return within one timeout of a cancel. The
    // ONE uninterruptible section is name resolution -- getaddrinfo (ping/traceroute target)
    // and the per-hop reverse getnameinfo -- which ignores the flag and blocks for the OS
    // resolver timeout. ACCEPTED RESIDUAL (adversarial review, low-probability): if a run is
    // cancelled/torn down while stuck in a slow resolver past the 15s wait, terminate() fires
    // inside Winsock. This is a read-only probe that mutates no local state and holds only
    // stack-local engine handles, so the cost is at worst a stranded resolver/loader lock;
    // bounding teardown is judged better than an unbounded hang on an unresponsive resolver.
    if (isRunning()) {
        requestStop();
        m_tester.cancel();
        m_scanner.cancel();
        if (!wait(kTimeoutThreadShutdownMs)) {
            terminate();
            wait(kTimeoutThreadTerminateMs);
        }
    }
}

void NetworkProbeWorker::cancelExecution() {
    requestStop();  // so WorkerBase emits cancelled() rather than finished()
    m_tester.cancel();
    m_scanner.cancel();
}

auto NetworkProbeWorker::execute() -> std::expected<void, sak::error_code> {
    switch (m_kind) {
    case Kind::Ping:
        runPing();
        break;
    case Kind::Traceroute:
        runTraceroute();
        break;
    case Kind::PortScan:
        runPortScan();
        break;
    case Kind::Mtr:
        runMtr();
        break;
    }
    return {};
}

void NetworkProbeWorker::runPing() {
    // DirectConnection: the engine emits inline on THIS (worker) thread, so the slot runs
    // synchronously here and the result is written before execute() returns.
    QObject::connect(
        &m_tester,
        &ConnectivityTester::pingComplete,
        &m_tester,
        [this](const PingResult& r) {
            m_ping = r;
            m_captured = true;
        },
        Qt::DirectConnection);
    QObject::connect(
        &m_tester,
        &ConnectivityTester::errorOccurred,
        &m_tester,
        [this](const QString& e) { m_error = e; },
        Qt::DirectConnection);
    m_tester.ping(m_pingConfig);
}

void NetworkProbeWorker::runTraceroute() {
    QObject::connect(
        &m_tester,
        &ConnectivityTester::tracerouteComplete,
        &m_tester,
        [this](const TracerouteResult& r) {
            m_trace = r;
            m_captured = true;
        },
        Qt::DirectConnection);
    QObject::connect(
        &m_tester,
        &ConnectivityTester::errorOccurred,
        &m_tester,
        [this](const QString& e) { m_error = e; },
        Qt::DirectConnection);
    m_tester.traceroute(m_traceConfig);
}

void NetworkProbeWorker::runPortScan() {
    QObject::connect(
        &m_scanner,
        &PortScanner::scanComplete,
        &m_scanner,
        [this](const QVector<PortScanResult>& r) {
            m_ports = r;
            m_captured = true;
        },
        Qt::DirectConnection);
    QObject::connect(
        &m_scanner,
        &PortScanner::errorOccurred,
        &m_scanner,
        [this](const QString& e) { m_error = e; },
        Qt::DirectConnection);
    m_scanner.scan(m_scanConfig);
}

void NetworkProbeWorker::runMtr() {
    // mtr() polls the same cancel atomic between every hop, so cancelExecution()/teardown
    // return within ~one timeout (plus at most one inter-cycle sleep) of the request. Its
    // continuous nature is bounded upstream by hard-clamped cycles/maxHops/timeoutMs so the
    // engine cannot run past the driveNetworkProbe wall-time ceiling.
    QObject::connect(
        &m_tester,
        &ConnectivityTester::mtrComplete,
        &m_tester,
        [this](const MtrResult& r) {
            m_mtr = r;
            m_captured = true;
        },
        Qt::DirectConnection);
    QObject::connect(
        &m_tester,
        &ConnectivityTester::errorOccurred,
        &m_tester,
        [this](const QString& e) { m_error = e; },
        Qt::DirectConnection);
    m_tester.mtr(m_mtrConfig);
}

}  // namespace sak
