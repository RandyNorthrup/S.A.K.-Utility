// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// sak_ai_tool_bridge: stdio MCP relay for agent runtimes.
//
// Agent runtimes (Codex, Claude Code, Gemini CLI, Muse Code) launch this as an
// ordinary stdio MCP server. It connects to the running S.A.K. instance's
// per-session local socket, authenticates with the token from the environment,
// and relays newline-delimited JSON-RPC in both directions. All tool logic and
// policy stays inside S.A.K.

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QMetaObject>
#include <QStringList>

#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr int kConnectTimeoutMs = 10'000;
constexpr int kExitUsage = 2;
constexpr int kExitConnect = 3;
constexpr auto kTokenVariable = "SAK_AI_TOOL_BRIDGE_TOKEN";

void setBinaryStdio() {
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

void writeStdout(const QByteArray& data) {
    std::fwrite(data.constData(), 1, static_cast<size_t>(data.size()), stdout);
    std::fflush(stdout);
}

QString pipeArgument(const QStringList& arguments) {
    const qsizetype index = arguments.indexOf(QStringLiteral("--pipe"));
    return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : QString();
}

/// Blocking stdin reader; each line is forwarded to the socket on the GUI-less
/// main thread. EOF (the agent closed the server) quits the relay.
void startStdinReader(QLocalSocket* socket) {
    std::thread([socket]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            QByteArray bytes = QByteArray::fromStdString(line).trimmed();
            if (bytes.isEmpty()) {
                continue;
            }
            bytes.append('\n');
            QMetaObject::invokeMethod(socket, [socket, bytes]() {
                socket->write(bytes);
                socket->flush();
            });
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), []() { QCoreApplication::quit(); });
    }).detach();
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    setBinaryStdio();

    const QString pipe = pipeArgument(QCoreApplication::arguments());
    const QByteArray token = qgetenv(kTokenVariable);
    if (pipe.isEmpty() || token.isEmpty()) {
        std::fprintf(stderr,
                     "usage: sak_ai_tool_bridge --pipe <name> (token via %s)\n",
                     kTokenVariable);
        return kExitUsage;
    }

    QLocalSocket socket;
    socket.connectToServer(pipe);
    if (!socket.waitForConnected(kConnectTimeoutMs)) {
        std::fprintf(stderr,
                     "sak_ai_tool_bridge: cannot reach S.A.K.: %s\n",
                     qPrintable(socket.errorString()));
        return kExitConnect;
    }
    QByteArray hello =
        QJsonDocument(QJsonObject{{QStringLiteral("sak_bridge_token"), QString::fromLatin1(token)}})
            .toJson(QJsonDocument::Compact);
    hello.append('\n');
    socket.write(hello);
    socket.flush();

    QObject::connect(&socket, &QLocalSocket::readyRead, &socket, [&socket]() {
        writeStdout(socket.readAll());
    });
    QObject::connect(&socket, &QLocalSocket::disconnected, &app, &QCoreApplication::quit);
    startStdinReader(&socket);
    return QCoreApplication::exec();
}
