// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/about_dialog.h"
#include "sak/version.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QSysInfo>
#include <QString>

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent)
    , m_tab_widget(nullptr)
    , m_icon_label(nullptr)
    , m_title_label(nullptr)
    , m_version_label(nullptr)
    , m_license_browser(nullptr)
    , m_credits_browser(nullptr)
    , m_system_browser(nullptr)
    , m_close_button(nullptr)
{
    setWindowTitle("About S.A.K. Utility");
    setModal(true);
    setMinimumSize(600, 500);
    setup_ui();
}

void AboutDialog::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(12);
    main_layout->setContentsMargins(16, 16, 16, 16);

    // Header with icon and title
    auto* header_layout = new QHBoxLayout();
    
    m_icon_label = new QLabel(this);
    m_icon_label->setFixedSize(64, 64);
    m_icon_label->setStyleSheet("QLabel { background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #3b82f6, stop:1 #2563eb); border-radius: 12px; }");
    header_layout->addWidget(m_icon_label);
    
    auto* title_layout = new QVBoxLayout();
    m_title_label = new QLabel("<b>S.A.K. Utility</b>", this);
    m_title_label->setStyleSheet("font-size: 18pt; font-weight: 700; color: #0f172a;");
    title_layout->addWidget(m_title_label);
    
    m_version_label = new QLabel(get_version_info(), this);
    m_version_label->setStyleSheet("font-size: 10pt; color: #64748b;");
    title_layout->addWidget(m_version_label);
    
    header_layout->addLayout(title_layout);
    header_layout->addStretch();
    
    main_layout->addLayout(header_layout);

    // Tab widget
    m_tab_widget = new QTabWidget(this);
    setup_about_tab();
    setup_license_tab();
    setup_credits_tab();
    setup_system_tab();
    main_layout->addWidget(m_tab_widget);

    // Close button
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();
    
    m_close_button = new QPushButton("Close", this);
    connect(m_close_button, &QPushButton::clicked, this, &AboutDialog::accept);
    button_layout->addWidget(m_close_button);
    
    main_layout->addLayout(button_layout);
}

void AboutDialog::setup_about_tab()
{
    auto* about_widget = new QWidget(this);
    auto* about_layout = new QVBoxLayout(about_widget);
    
    auto* description = new QLabel(
        "<p><b>Swiss Army Knife (S.A.K.) Utility</b></p>"
        "<p><b>PC Technician's Toolkit for Windows Migration and Maintenance</b></p>"
        "<p>Designed for PC technicians who need to migrate systems, backup user profiles, "
        "and manage files efficiently. Built with modern C++23 and Qt6 for Windows 10/11 x64.</p>"
        "<p><b>Core Features:</b></p>"
        "<ul>"
        "<li>User Profile Backup & Restore - Comprehensive wizards for PC migrations</li>"
        "<li>Application Migration - Automated software reinstallation via Chocolatey</li>"
        "<li>Directory Organizer - Quick file sorting by extension</li>"
        "<li>Duplicate File Finder - Free up disk space with MD5 detection</li>"
        "<li>License Key Scanner - Locate registry-stored product keys</li>"
        "</ul>",
        this
    );
    description->setWordWrap(true);
    description->setOpenExternalLinks(true);
    about_layout->addWidget(description);
    about_layout->addStretch();
    
    m_tab_widget->addTab(about_widget, "About");
}

void AboutDialog::setup_license_tab()
{
    m_license_browser = new QTextBrowser(this);
    m_license_browser->setOpenExternalLinks(true);
    m_license_browser->setHtml(get_license_text());
    m_tab_widget->addTab(m_license_browser, "License");
}

void AboutDialog::setup_credits_tab()
{
    m_credits_browser = new QTextBrowser(this);
    m_credits_browser->setOpenExternalLinks(true);
    m_credits_browser->setHtml(get_credits_text());
    m_tab_widget->addTab(m_credits_browser, "Credits");
}

void AboutDialog::setup_system_tab()
{
    m_system_browser = new QTextBrowser(this);
    m_system_browser->setPlainText(get_system_info());
    m_tab_widget->addTab(m_system_browser, "System");
}

QString AboutDialog::get_version_info() const
{
    return QString("Version %1 - %2").arg(sak::get_version(), sak::get_build_date());
}

QString AboutDialog::get_license_text() const
{
    return R"(
<h3>GNU General Public License v2.0</h3>
<p>Copyright (C) 2025 Randy Northrup</p>

<p>This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.</p>

<p>This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.</p>

<p>You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.</p>

<p><b>Note:</b> This application uses Qt Framework (LGPL v3) and Chocolatey (Apache 2.0).</p>
)";
}

QString AboutDialog::get_credits_text() const
{
    return R"(
<h3>Development</h3>
<p><b>Lead Developer:</b> Randy Northrup</p>
<p><b>Original Python Version:</b> Archived proof of concept</p>

<h3>Third-Party Components</h3>
<ul>
<li><b>Qt Framework 6.5.3</b> - GUI framework and cryptographic functions
    <br/>Licensed under LGPL v3
    <br/><a href="https://www.qt.io/">https://www.qt.io/</a>
    <br/>Used for: GUI, threading (QtConcurrent), networking, cryptography (QCryptographicHash)</li>
<li><b>Chocolatey</b> - Windows package manager (embedded)
    <br/>Licensed under Apache 2.0
    <br/><a href="https://chocolatey.org/">https://chocolatey.org/</a>
    <br/>Used for: Application migration and automated software installation</li>
</ul>

<h3>Special Thanks</h3>
<p>To the C++ and Qt communities for their excellent documentation and support.</p>
<p>To Microsoft for Windows API documentation.</p>
)";
}

QString AboutDialog::get_system_info() const
{
    QString info;
    
    info += QString("Application Version: %1\n").arg(sak::get_version());
    info += QString("Build Date: %1 %2\n\n").arg(sak::get_build_date(), sak::get_build_time());
    
    info += QString("Operating System: %1\n").arg(QSysInfo::prettyProductName());
    info += QString("Kernel: %1 %2\n").arg(QSysInfo::kernelType(), QSysInfo::kernelVersion());
    info += QString("Architecture: %1\n").arg(QSysInfo::currentCpuArchitecture());
    info += QString("Build ABI: %1\n\n").arg(QSysInfo::buildAbi());
    
    info += QString("Qt Version: %1 (Runtime: %2)\n").arg(QT_VERSION_STR, qVersion());
    info += QString("Compiler: ");
    
#if defined(__clang__)
    info += QString("Clang %1.%2.%3\n").arg(__clang_major__).arg(__clang_minor__).arg(__clang_patchlevel__);
#elif defined(__GNUC__)
    info += QString("GCC %1.%2.%3\n").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    info += QString("MSVC %1\n").arg(_MSC_VER);
#else
    info += "Unknown\n";
#endif
    
    info += QString("\nC++ Standard: C++%1\n").arg(__cplusplus);
    
#ifdef _WIN32
    info += "\nPlatform: Windows";
#elif defined(__APPLE__)
    info += "\nPlatform: macOS";
#elif defined(__linux__)
    info += "\nPlatform: Linux";
#else
    info += "\nPlatform: Unknown";
#endif
    
    return info;
}
