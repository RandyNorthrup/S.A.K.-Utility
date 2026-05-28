// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file message_box_helpers.h
/// @brief Logged wrappers for user-visible dialogs.

#pragma once

#include "sak/logger.h"

#include <QMessageBox>
#include <QString>

#include <utility>

namespace sak {

inline void logDialogWarning(const QString& title, const QString& message, const QString& context) {
    const QString resolved_context = context.isEmpty() ? title : context;
    logWarning("{}: {}", resolved_context.toStdString(), message.toStdString());
}

inline void logDialogCritical(const QString& title,
                              const QString& message,
                              const QString& context) {
    const QString resolved_context = context.isEmpty() ? title : context;
    logCritical("{}: {}", resolved_context.toStdString(), message.toStdString());
}

inline void logDialogInfo(const QString& title, const QString& message, const QString& context) {
    const QString resolved_context = context.isEmpty() ? title : context;
    logInfo("{}: {}", resolved_context.toStdString(), message.toStdString());
}

template <typename... Args>
inline QMessageBox::StandardButton showWarningLogged(QWidget* parent,
                                                     const QString& title,
                                                     const QString& message,
                                                     Args&&... args) {
    logDialogWarning(title, message, {});
    return QMessageBox::warning(parent, title, message, std::forward<Args>(args)...);
}

inline QMessageBox::StandardButton showWarningLogged(QWidget* parent,
                                                     const QString& title,
                                                     const QString& message,
                                                     const QString& log_context) {
    logDialogWarning(title, message, log_context);
    return QMessageBox::warning(parent, title, message);
}

inline QMessageBox::StandardButton showWarningLogged(QWidget* parent,
                                                     const QString& title,
                                                     const QString& message,
                                                     QString&& log_context) {
    logDialogWarning(title, message, log_context);
    return QMessageBox::warning(parent, title, message);
}

template <typename... Args>
inline QMessageBox::StandardButton showCriticalLogged(QWidget* parent,
                                                      const QString& title,
                                                      const QString& message,
                                                      Args&&... args) {
    logDialogCritical(title, message, {});
    return QMessageBox::critical(parent, title, message, std::forward<Args>(args)...);
}

inline QMessageBox::StandardButton showCriticalLogged(QWidget* parent,
                                                      const QString& title,
                                                      const QString& message,
                                                      const QString& log_context) {
    logDialogCritical(title, message, log_context);
    return QMessageBox::critical(parent, title, message);
}

inline QMessageBox::StandardButton showCriticalLogged(QWidget* parent,
                                                      const QString& title,
                                                      const QString& message,
                                                      QString&& log_context) {
    logDialogCritical(title, message, log_context);
    return QMessageBox::critical(parent, title, message);
}

template <typename... Args>
inline QMessageBox::StandardButton showInformationLogged(QWidget* parent,
                                                         const QString& title,
                                                         const QString& message,
                                                         Args&&... args) {
    logDialogInfo(title, message, {});
    return QMessageBox::information(parent, title, message, std::forward<Args>(args)...);
}

inline QMessageBox::StandardButton showInformationLogged(QWidget* parent,
                                                         const QString& title,
                                                         const QString& message,
                                                         const QString& log_context) {
    logDialogInfo(title, message, log_context);
    return QMessageBox::information(parent, title, message);
}

inline QMessageBox::StandardButton showInformationLogged(QWidget* parent,
                                                         const QString& title,
                                                         const QString& message,
                                                         QString&& log_context) {
    logDialogInfo(title, message, log_context);
    return QMessageBox::information(parent, title, message);
}

template <typename... Args>
inline QMessageBox::StandardButton showQuestionLogged(QWidget* parent,
                                                      const QString& title,
                                                      const QString& message,
                                                      Args&&... args) {
    logDialogInfo(title, message, {});
    return QMessageBox::question(parent, title, message, std::forward<Args>(args)...);
}

inline QMessageBox::StandardButton showQuestionLogged(QWidget* parent,
                                                      const QString& title,
                                                      const QString& message,
                                                      const QString& log_context) {
    logDialogInfo(title, message, log_context);
    return QMessageBox::question(parent, title, message);
}

inline QMessageBox::StandardButton showQuestionLogged(QWidget* parent,
                                                      const QString& title,
                                                      const QString& message,
                                                      QString&& log_context) {
    logDialogInfo(title, message, log_context);
    return QMessageBox::question(parent, title, message);
}

}  // namespace sak
