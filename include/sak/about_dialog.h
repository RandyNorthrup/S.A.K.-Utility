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
    void setupUi();
    void setupAboutTab();
    void setupLicenseTab();
    void setupCreditsTab();
    void setupSystemTab();
    
    QString getVersionInfo() const;
    QString getLicenseText() const;
    QString getCreditsText() const;
    QString getSystemInfo() const;
    
    QTabWidget* m_tab_widget;
    QLabel* m_icon_label;
    QLabel* m_title_label;
    QLabel* m_version_label;
    QTextBrowser* m_license_browser;
    QTextBrowser* m_credits_browser;
    QTextBrowser* m_system_browser;
    QPushButton* m_close_button;
};
