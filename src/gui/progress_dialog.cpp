// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/progress_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>

ProgressDialog::ProgressDialog(const QString& title, const QString& label_text, QWidget* parent)
    : QDialog(parent)
    , m_title_label(nullptr)
    , m_status_label(nullptr)
    , m_progress_bar(nullptr)
    , m_status_text(nullptr)
    , m_elapsed_label(nullptr)
    , m_eta_label(nullptr)
    , m_cancel_button(nullptr)
    , m_timer(nullptr)
    , m_start_time(std::chrono::steady_clock::now())
    , m_estimated_remaining(0)
    , m_show_elapsed_time(true)
    , m_show_eta(true)
    , m_cancelled(false)
    , m_last_value(0)
{
    setWindowTitle(title);
    setModal(true);
    setMinimumWidth(500);
    setupUi();
    
    if (!label_text.isEmpty()) {
        setLabelText(label_text);
    }

    // Start timer for elapsed time updates
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &ProgressDialog::updateTimeDisplays);
    m_timer->start(1000); // Update every second
}

ProgressDialog::~ProgressDialog()
{
    if (m_timer) {
        m_timer->stop();
    }
}

void ProgressDialog::setupUi()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(12);
    main_layout->setContentsMargins(16, 16, 16, 16);

    // Title label
    m_title_label = new QLabel(this);
    m_title_label->setWordWrap(true);
    m_title_label->setStyleSheet("font-size: 14pt; font-weight: bold;");
    main_layout->addWidget(m_title_label);

    // Status label
    m_status_label = new QLabel(this);
    m_status_label->setWordWrap(true);
    main_layout->addWidget(m_status_label);

    // Progress bar
    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setMinimumHeight(25);
    m_progress_bar->setTextVisible(true);
    main_layout->addWidget(m_progress_bar);

    // Time information layout
    auto* time_layout = new QHBoxLayout();
    
    m_elapsed_label = new QLabel("Elapsed: 00:00:00", this);
    time_layout->addWidget(m_elapsed_label);
    
    time_layout->addStretch();
    
    m_eta_label = new QLabel("ETA: Calculating...", this);
    time_layout->addWidget(m_eta_label);
    
    main_layout->addLayout(time_layout);

    // Status text (multi-line)
    m_status_text = new QTextEdit(this);
    m_status_text->setReadOnly(true);
    m_status_text->setMaximumHeight(150);
    m_status_text->setVisible(false); // Hidden by default
    main_layout->addWidget(m_status_text);

    // Cancel button
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();
    
    m_cancel_button = new QPushButton("Cancel", this);
    connect(m_cancel_button, &QPushButton::clicked, this, &ProgressDialog::onCancelClicked);
    button_layout->addWidget(m_cancel_button);
    
    main_layout->addLayout(button_layout);
}

void ProgressDialog::setRange(int minimum, int maximum)
{
    m_progress_bar->setRange(minimum, maximum);
    
    // Reset ETA calculation when range changes
    m_start_time = std::chrono::steady_clock::now();
    m_last_value = minimum;
    
    if (maximum == 0) {
        // Indeterminate mode
        m_eta_label->setVisible(false);
    } else {
        m_eta_label->setVisible(m_show_eta);
    }
}

void ProgressDialog::setValue(int value)
{
    m_progress_bar->setValue(value);
    
    // Update ETA if in determinate mode
    if (m_progress_bar->maximum() > 0) {
        updateEta(value, m_progress_bar->maximum());
    }
    
    m_last_value = value;
}

int ProgressDialog::value() const
{
    return m_progress_bar->value();
}

void ProgressDialog::setLabelText(const QString& text)
{
    m_status_label->setText(text);
}

void ProgressDialog::appendStatusText(const QString& text)
{
    if (!m_status_text->isVisible()) {
        m_status_text->setVisible(true);
    }
    m_status_text->append(text);
}

void ProgressDialog::clearStatusText()
{
    m_status_text->clear();
    m_status_text->setVisible(false);
}

void ProgressDialog::setShowElapsedTime(bool show)
{
    m_show_elapsed_time = show;
    m_elapsed_label->setVisible(show);
}

void ProgressDialog::setShowETA(bool show)
{
    m_show_eta = show;
    if (m_progress_bar->maximum() > 0) {
        m_eta_label->setVisible(show);
    }
}

void ProgressDialog::reset()
{
    m_progress_bar->setValue(0);
    m_status_text->clear();
    m_start_time = std::chrono::steady_clock::now();
    m_estimated_remaining = std::chrono::seconds(0);
    m_cancelled = false;
    m_last_value = 0;
    m_cancel_button->setEnabled(true);
}

void ProgressDialog::onCancelClicked()
{
    auto reply = QMessageBox::question(
        this,
        "Confirm Cancellation",
        "Are you sure you want to cancel this operation?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    
    if (reply == QMessageBox::Yes) {
        m_cancelled = true;
        m_cancel_button->setEnabled(false);
        m_cancel_button->setText("Cancelling...");
        setLabelText("Cancelling operation...");
        Q_EMIT cancelled();
    }
}

void ProgressDialog::updateTimeDisplays()
{
    if (!m_show_elapsed_time && !m_show_eta) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_start_time);

    if (m_show_elapsed_time) {
        m_elapsed_label->setText(QString("Elapsed: %1").arg(formatDuration(elapsed)));
    }

    if (m_show_eta && m_progress_bar->maximum() > 0) {
        if (m_estimated_remaining.count() > 0) {
            m_eta_label->setText(QString("ETA: %1").arg(formatDuration(m_estimated_remaining)));
        } else {
            m_eta_label->setText("ETA: Calculating...");
        }
    }
}

void ProgressDialog::updateEta(int current, int maximum)
{
    if (maximum <= 0 || current <= 0) {
        m_estimated_remaining = std::chrono::seconds(0);
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_start_time);

    if (elapsed.count() < 2) {
        // Not enough data yet
        return;
    }

    // Calculate rate and estimate remaining time
    double progress_ratio = static_cast<double>(current) / static_cast<double>(maximum);
    if (progress_ratio > 0.0 && progress_ratio < 1.0) {
        double total_estimated_seconds = elapsed.count() / progress_ratio;
        double remaining_seconds = total_estimated_seconds - elapsed.count();
        
        if (remaining_seconds > 0) {
            m_estimated_remaining = std::chrono::seconds(static_cast<long long>(remaining_seconds));
        } else {
            m_estimated_remaining = std::chrono::seconds(0);
        }
    }
}

QString ProgressDialog::formatDuration(std::chrono::seconds duration) const
{
    auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
    duration -= hours;
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);
    duration -= minutes;
    auto seconds = duration;

    return QString("%1:%2:%3")
        .arg(hours.count(), 2, 10, QChar('0'))
        .arg(minutes.count(), 2, 10, QChar('0'))
        .arg(seconds.count(), 2, 10, QChar('0'));
}
