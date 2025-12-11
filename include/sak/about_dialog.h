#pragma once

#include <QDialog>
#include <QLabel>
#include <QTabWidget>
#include <QTextBrowser>
#include <QPushButton>

/**
 * @brief About dialog displaying application information
 * 
 * Shows:
 * - Version information
 * - License text
 * - Credits
 * - System information
 */
class AboutDialog : public QDialog {
    Q_OBJECT

public:
    explicit AboutDialog(QWidget* parent = nullptr);
    ~AboutDialog() override = default;

    AboutDialog(const AboutDialog&) = delete;
    AboutDialog& operator=(const AboutDialog&) = delete;
    AboutDialog(AboutDialog&&) = delete;
    AboutDialog& operator=(AboutDialog&&) = delete;

private:
    void setup_ui();
    void setup_about_tab();
    void setup_license_tab();
    void setup_credits_tab();
    void setup_system_tab();
    
    QString get_version_info() const;
    QString get_license_text() const;
    QString get_credits_text() const;
    QString get_system_info() const;
    
    QTabWidget* m_tab_widget;
    QLabel* m_icon_label;
    QLabel* m_title_label;
    QLabel* m_version_label;
    QTextBrowser* m_license_browser;
    QTextBrowser* m_credits_browser;
    QTextBrowser* m_system_browser;
    QPushButton* m_close_button;
};
