#pragma once

#include <QWidget>
#include <QTextBrowser>
#include <QPushButton>
#include <QCheckBox>
#include <QLineEdit>
#include <QComboBox>

/**
 * @brief Log viewer widget with filtering and export
 * 
 * Features:
 * - Syntax highlighting for log levels
 * - Auto-scroll option
 * - Filter by level (Info, Warning, Error)
 * - Search functionality
 * - Clear and save options
 */
class LogViewer : public QWidget {
    Q_OBJECT

public:
    enum class LogLevel {
        All,
        Info,
        Warning,
        Error
    };

    explicit LogViewer(QWidget* parent = nullptr);
    ~LogViewer() override = default;

    LogViewer(const LogViewer&) = delete;
    LogViewer& operator=(const LogViewer&) = delete;
    LogViewer(LogViewer&&) = delete;
    LogViewer& operator=(LogViewer&&) = delete;

    /**
     * @brief Append log message
     * @param message Log message text
     * @param level Log level (Info, Warning, Error)
     */
    void appendLog(const QString& message, LogLevel level = LogLevel::Info);

    /**
     * @brief Load log file
     * @param file_path Path to log file
     * @return True if successful
     */
    bool loadLogFile(const QString& file_path);

    /**
     * @brief Clear all log content
     */
    void clear();

    /**
     * @brief Set auto-scroll enabled
     * @param enabled True to enable auto-scroll
     */
    void setAutoScroll(bool enabled);

    /**
     * @brief Get auto-scroll state
     * @return True if auto-scroll enabled
     */
    [[nodiscard]] bool autoScroll() const noexcept { return m_auto_scroll; }

public Q_SLOTS:
    void on_clear_clicked();
    void on_save_clicked();
    void on_filter_changed(int index);
    void on_search_text_changed(const QString& text);
    void on_auto_scroll_toggled(bool checked);

private:
    void setup_ui();
    void apply_filters();
    QString format_log_message(const QString& message, LogLevel level) const;
    QString get_level_color(LogLevel level) const;
    QString get_level_text(LogLevel level) const;

    QTextBrowser* m_text_browser;
    QPushButton* m_clear_button;
    QPushButton* m_save_button;
    QCheckBox* m_auto_scroll_checkbox;
    QComboBox* m_filter_combo;
    QLineEdit* m_search_edit;

    QStringList m_all_logs;
    LogLevel m_current_filter;
    bool m_auto_scroll;
};
