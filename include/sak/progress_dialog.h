#pragma once

#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <chrono>

/**
 * @brief Custom progress dialog with elapsed time and ETA
 * 
 * Provides determinate/indeterminate progress display with:
 * - Multi-line status text
 * - Elapsed time display
 * - ETA calculation
 * - Cancellation with confirmation
 */
class ProgressDialog : public QDialog {
    Q_OBJECT

public:
    /**
     * @brief Construct progress dialog
     * @param title Window title
     * @param label_text Initial status text
     * @param parent Parent widget
     */
    explicit ProgressDialog(const QString& title, const QString& label_text = QString(), QWidget* parent = nullptr);
    ~ProgressDialog() override;

    ProgressDialog(const ProgressDialog&) = delete;
    ProgressDialog& operator=(const ProgressDialog&) = delete;
    ProgressDialog(ProgressDialog&&) = delete;
    ProgressDialog& operator=(ProgressDialog&&) = delete;

    /**
     * @brief Set progress range
     * @param minimum Minimum value (use 0 for indeterminate)
     * @param maximum Maximum value (use 0 for indeterminate)
     */
    void setRange(int minimum, int maximum);

    /**
     * @brief Set current progress value
     * @param value Current value
     */
    void setValue(int value);

    /**
     * @brief Get current progress value
     * @return Current value
     */
    [[nodiscard]] int value() const;

    /**
     * @brief Set status label text
     * @param text Status text
     */
    void setLabelText(const QString& text);

    /**
     * @brief Append text to status display
     * @param text Text to append
     */
    void appendStatusText(const QString& text);

    /**
     * @brief Clear status text
     */
    void clearStatusText();

    /**
     * @brief Set whether to show elapsed time
     * @param show True to show elapsed time
     */
    void setShowElapsedTime(bool show);

    /**
     * @brief Set whether to show ETA
     * @param show True to show ETA
     */
    void setShowETA(bool show);

    /**
     * @brief Reset progress and timer
     */
    void reset();

    /**
     * @brief Check if dialog was cancelled
     * @return True if cancelled
     */
    [[nodiscard]] bool wasCancelled() const noexcept { return m_cancelled; }

Q_SIGNALS:
    /**
     * @brief Emitted when cancel button clicked
     */
    void cancelled();

private Q_SLOTS:
    void on_cancel_clicked();
    void update_time_displays();

private:
    void setup_ui();
    void update_eta(int current, int maximum);
    QString format_duration(std::chrono::seconds duration) const;

    QLabel* m_title_label;
    QLabel* m_status_label;
    QProgressBar* m_progress_bar;
    QTextEdit* m_status_text;
    QLabel* m_elapsed_label;
    QLabel* m_eta_label;
    QPushButton* m_cancel_button;
    
    QTimer* m_timer;
    std::chrono::steady_clock::time_point m_start_time;
    std::chrono::seconds m_estimated_remaining;
    
    bool m_show_elapsed_time;
    bool m_show_eta;
    bool m_cancelled;
    int m_last_value;
};
