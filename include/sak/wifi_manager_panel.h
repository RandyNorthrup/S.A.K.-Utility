// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QFuture>
#include <QGroupBox>
#include <QImage>
#include <QLineEdit>
#include <QList>
#include <QModelIndex>
#include <QPair>
#include <QPushButton>
#include <QString>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QWidget>

#include <atomic>
#include <utility>

class QLabel;
class QFormLayout;
class QStackedWidget;
class QVBoxLayout;
class QTextStream;
class QFileDevice;

namespace sak {

class LogToggleSwitch;

/**
 * @brief WiFi Manager panel  --  generates scannable WiFi QR codes and network scripts
 *
 * Allows entry of one or more WiFi networks and exports QR codes via a mini
 * wizard (PNG / PDF / JPG / BMP), plus Windows netsh scripts and macOS profiles.
 *
 * WiFi payload format: WIFI:T:<security>;S:<ssid>;P:<password>;H:<hidden>;;
 * QR error correction: HIGH (30%)
 */
class WifiManagerPanel : public QWidget {
    Q_OBJECT

public:
    explicit WifiManagerPanel(QWidget* parent = nullptr);
    ~WifiManagerPanel() override;

    WifiManagerPanel(const WifiManagerPanel&) = delete;
    WifiManagerPanel& operator=(const WifiManagerPanel&) = delete;
    WifiManagerPanel(WifiManagerPanel&&) = delete;
    WifiManagerPanel& operator=(WifiManagerPanel&&) = delete;

    /// @brief Access the log toggle switch for MainWindow log window connection
    [[nodiscard]] LogToggleSwitch* logToggle() const { return m_logToggle; }

    /// @brief Generate a Windows netsh .cmd script that provisions one network
    /// @return Script text, or an empty string if the SSID cannot be embedded
    ///         safely (contains a double quote or a control character)
    /// @note Public + static so SSID-injection escaping can be unit tested; the
    ///       SSID reaches escapeBatchString/ssidIsBatchSafe untrusted (a scanned
    ///       network name may be attacker-chosen).
    [[nodiscard]] static QString buildWindowsScript(const QString& ssid,
                                                    const QString& password,
                                                    const QString& security,
                                                    bool hidden);

    /// @brief Fail-closed predicate for a credential-table JSON write
    /// @return true only if every byte was written AND the file committed
    /// @note Public + static so the short-write / commit-failure rule can be
    ///       unit tested; a truncated credential table must never report
    ///       success (NO FALLBACKS / FAIL CLOSED).
    [[nodiscard]] static bool jsonWriteSucceeded(qint64 written, qint64 expected, bool committed);

    /// @brief One row of the credential table: a network as the technician entered or imported it.
    /// @note Public because wifiProfileInstallRefusal takes one, so the refusal rule can be
    ///       exercised by a test that builds a row directly.
    struct WifiConfig {
        QString location;
        QString ssid;
        QString password;
        QString security;
        bool hidden{false};
    };

    /// @brief Why a table row cannot be installed as a Windows WLAN profile, or an empty string
    ///        when it can.
    /// @note This is the panel's OWN precondition, not security resolution -- what a given
    ///       security label means is decided once, in sak::addWifiProfileWindows, which both this
    ///       panel and the assistant's connect_wifi action install through.
    ///       An empty security label is refused HERE even though the shared seam would accept it
    ///       as "caller did not specify, use the interoperable default". That default is right for
    ///       connect_wifi, where security is an optional argument; it is wrong for this table,
    ///       where every row has a security column, so an empty one means an imported or
    ///       hand-edited row is malformed and guessing would install a profile the technician
    ///       never described.
    /// @note Public + static and PURE so the refusal can be unit tested without installing
    ///       anything. A test that asserted this by calling the installer would run a live
    ///       `netsh wlan add profile` the moment the refusal regressed.
    [[nodiscard]] static QString wifiProfileInstallRefusal(const WifiConfig& cfg);

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void logOutput(const QString& message);

private Q_SLOTS:
    void onAddToTableClicked();
    void onDeleteSelectedClicked();
    void onGenerateQrClicked();
    void onExportWindowsScriptClicked();
    void onExportMacosProfileClicked();
    void onTableDoubleClicked(const QModelIndex& index);
    void onSearchChanged(const QString& text);
    void onFindNext();
    void onFindPrev();
    void onSecurityChanged(const QString& value);
    void onTogglePasswordVisibility();
    void onSaveTableClicked();
    void onLoadTableClicked();
    void onConnectWithPhoneClicked();
    void onScanNetworksClicked();
    void onAddToWindowsClicked();
    void onSelectionChanged();
    void onTableItemChanged(QTableWidgetItem* item);

private:
    // -------------------------------------------------------------------------
    // UI setup helpers
    // -------------------------------------------------------------------------
    void setupUi();
    void setupFormGroup();
    void setupNetworkIdentityFields(QFormLayout* layout);
    void setupNetworkPasswordField(QFormLayout* layout);
    void setupNetworkOptionsFields(QFormLayout* layout);
    void setupFormActionButtons(QFormLayout* layout);
    void setupTableGroup();
    void setupTableSearchRow(QVBoxLayout* layout);
    void setupNetworkTable(QVBoxLayout* layout);
    void setupTableActionButtons(QVBoxLayout* layout);
    void setupActionButtons();
    void connectSignals();

    struct QrExportFormats {
        bool png{false};
        bool pdf{false};
        bool jpg{false};
        bool bmp{false};
    };

    // -------------------------------------------------------------------------
    // WiFi payload helpers
    // -------------------------------------------------------------------------
    /** Escape special characters per WiFi QR spec: \ ; , : -> \\ \; \, \: */
    static QString escapeWifiField(const QString& value);

    /** Normalize security string for QR payload ("WPA/WPA2/WPA3" -> "WPA", etc.) */
    static QString normalizeSecurityForQr(const QString& security);

    /** Build the WIFI: URI payload from the current form values */
    QString buildWifiPayload() const;

    /** Build the WIFI: URI payload from an explicit WifiConfig struct */
    static QString buildWifiPayloadFromConfig(const WifiConfig& cfg);

    // -------------------------------------------------------------------------
    // Windows script export helpers
    // -------------------------------------------------------------------------
    void exportSingleWindowsScript(const WifiConfig& cfg);
    void exportMultipleWindowsScripts(const QList<WifiConfig>& sources);

    // -------------------------------------------------------------------------
    // QR generation
    // -------------------------------------------------------------------------
    /** Render a QR code for @p payload into a 640x640 QImage (white background) */
    static QImage generateQrImage(const QString& payload);

    // ------------------------------------------------------------------------- // QR wizard
    /// decomposition helpers
    // -------------------------------------------------------------------------
    /** Bundle of widget pointers shared between QR-wizard page builders */
    struct QrWizardControls {
        QLabel* previewLabel{};
        QCheckBox* chkPng{};
        QCheckBox* chkPdf{};
        QCheckBox* chkJpg{};
        QCheckBox* chkBmp{};
        LogToggleSwitch* headerToggle{};
        QPushButton* btnCancel0{};
        QPushButton* btnNext{};
        QLineEdit* dirEdit{};
        QLabel* subLabel{};
        QPushButton* btnBack{};
        QPushButton* btnCancel1{};
        QPushButton* btnBrowse{};
        QPushButton* btnGenerate{};
    };

    /** Render a QR image with an optional location header banner */
    static QImage renderQrWithHeader(const QString& payload,
                                     const QString& location,
                                     bool show_header);

    /** Write a QR image as a full-page A4 PDF; returns true on success */
    static bool exportQrToPdf(const QImage& image, const QString& path, const QString& title);
    /// Write the checked formats under @p path_stem (no extension); true if ANY landed.
    /// Each branch checks its own write, so a format that failed never counts as saved.
    static bool saveQrFormats(const QImage& img,
                              const QString& path_stem,
                              const QString& ssid,
                              const QrExportFormats& formats);

    /** Show a QR dialog for a single WiFi network */
    void showSingleNetworkQrDialog(const WifiConfig& cfg);

    /** Show a navigable QR dialog for multiple WiFi networks */
    void showMultiNetworkQrDialog(const QList<WifiConfig>& sources);

    /** Show the single-network QR wizard dialog */
    void showSingleQrWizard(const WifiConfig& cfg);

    /** Build wizard page 0 (format + header selection) */
    QWidget* buildQrFormatPage(const QString& payload,
                               const QString& location,
                               QrWizardControls& ctl);

    /** Build wizard page 1 (output directory selection) */
    QWidget* buildQrOutputPage(QrWizardControls& ctl);

    /// @brief Content parameters for QR code export wizard
    struct QrExportContent {
        QString payload;
        QString ssid;
        QString location;
        QString sub_name;
    };

    /** Wire all signals for the single-network QR wizard */
    void connectSingleQrWizard(QDialog* dlg,
                               QStackedWidget* stack,
                               QrWizardControls ctl,
                               const QrExportContent& content);

    /** Execute the single-network QR export (save selected formats) */
    void executeSingleQrExport(QDialog* dlg, QrWizardControls ctl, const QrExportContent& content);

    void saveCheckedFormats(QDialog* dlg,
                            QrWizardControls ctl,
                            const QImage& final_img,
                            const QrExportContent& content,
                            QStringList& saved);

    /** Show the batch (multi-network) QR export dialog */
    void showBatchQrDialog(const QList<WifiConfig>& sources);

    /** Execute the batch QR export loop for selected formats */
    void executeBatchQrExport(QDialog* dlg,
                              const QList<WifiConfig>& sources,
                              const QString& base_dir,
                              bool show_header,
                              const QrExportFormats& formats);
    /** Export QR images for single network in specified formats */
    bool executeSingleQrNetwork(const WifiConfig& cfg,
                                const QString& base_dir,
                                bool show_header,
                                const QrExportFormats& formats);

    // -----------------------------------------------------------------
    // Export helpers
    // -------------------------------------------------------------------------
    /** Generate a macOS .mobileconfig plist for one or more networks */
    static QString buildMacosProfile(const QList<WifiConfig>& networks);

    // -------------------------------------------------------------------------
    // Table helpers
    // -------------------------------------------------------------------------
    WifiConfig configFromForm() const;
    void loadConfigToForm(const WifiConfig& cfg);
    void addRowToTable(const WifiConfig& cfg);
    WifiConfig configFromRow(int row) const;
    QList<WifiConfig> allConfigs() const;
    /** Returns WifiConfig for every checked table row; empty list if none checked */
    QList<WifiConfig> checkedConfigs() const;
    /** Set the header's tri-state box from the row totals (no rows / none / some / all checked) */
    void updateHeaderTriState(int total, int checked);
    /** Enable and label the Save and Add-to-Windows buttons from the row totals */
    void updateSaveAndInstallButtons(int total, int checked);
    /**
     * Read the machine's known WiFi networks through the shared headless scanner.
     * @return the configs, and whether the scan was COMPLETE -- an empty list with a false flag
     *         means the profiles could not be read, which is not the same as having none.
     */
    static std::pair<QList<WifiConfig>, bool> scanKnownWifiNetworks();
    void updateSearchMatches(const QString& text);
    void highlightSearchMatches();
    /** Set all table row checkboxes to the given state */
    void setAllCheckStates(bool all_checked);
    /** Returns true if any visible column in the given row matches @p text */
    bool rowMatchesSearch(int row, const QString& text) const;
    // -------------------------------------------------------------------------
    // Windows WiFi profile helpers
    //
    // This panel does not build WLAN profile XML and does not run netsh. It calls
    // sak::addWifiProfileWindows, the same seam the assistant's connect_wifi action installs
    // through. It used to carry its own copy of both, and the copies disagreed -- see that
    // function's header comment for what the panel's own resolver did to its own default
    // security label.
    // -------------------------------------------------------------------------
    QList<int> checkedWifiRows() const;
    QList<WifiConfig> configsFromRows(const QList<int>& rows) const;
    void startAddToWindowsProfiles(const QList<WifiConfig>& configs);
    /// Install each profile through the shared seam, checking @p cancel between profiles so
    /// teardown can stop the loop after at most one bounded netsh call rather than after all N.
    static QPair<int, int> installWlanProfiles(const QList<WifiConfig>& configs,
                                               const std::atomic<bool>* cancel);

    // In-flight "add to Windows profiles" install (netsh wlan add profile). Tracked so the
    // destructor can COOPERATIVELY cancel (via m_wlanInstallCancel, checked between profiles) and
    // then bounded-wait it, instead of letting the mutation run detached past teardown or blocking
    // teardown for one netsh call per selected network.
    QFuture<QPair<int, int>> m_wlanInstallFuture;
    std::atomic<bool> m_wlanInstallCancel{false};

    // -------------------------------------------------------------------------
    // Persistence
    // -------------------------------------------------------------------------
    void saveTableToJson(const QString& path);
    /// Atomically (QSaveFile) write the checked-row subset to JSON; fail closed on a short write or
    /// failed commit so a truncated credential table is never reported as saved.
    void saveCheckedRowsToJson(const QString& path, const QList<int>& checked_rows);
    void loadTableFromJson(const QString& path);
    /// Fail-closed finalize for a QTextStream-backed export: flush and verify no stream/device
    /// error before any success is reported (disk-full / IO failure must fail closed).
    [[nodiscard]] static bool flushTextStreamOk(QTextStream& stream, const QFileDevice& file);

    // =========================================================================
    // UI widgets
    // =========================================================================

    // -- Form group --
    QGroupBox* m_form_group{nullptr};
    QLineEdit* m_location_input{nullptr};
    QLineEdit* m_ssid_input{nullptr};
    QLineEdit* m_password_input{nullptr};
    QToolButton* m_password_toggle_btn{nullptr};
    QComboBox* m_security_combo{nullptr};
    QCheckBox* m_hidden_checkbox{nullptr};

    // -- Table group --
    QGroupBox* m_table_group{nullptr};
    QTableWidget* m_network_table{nullptr};
    QLineEdit* m_search_input{nullptr};
    QToolButton* m_search_up_btn{nullptr};
    QToolButton* m_search_down_btn{nullptr};
    QPushButton* m_add_table_btn{nullptr};
    QPushButton* m_delete_selected_btn{nullptr};
    QPushButton* m_add_to_windows_btn{nullptr};
    QPushButton* m_save_table_btn{nullptr};
    QPushButton* m_load_table_btn{nullptr};

    // -- Action buttons --
    QPushButton* m_generate_qr_btn{nullptr};
    QPushButton* m_export_script_btn{nullptr};
    QPushButton* m_export_macos_btn{nullptr};
    QPushButton* m_connect_phone_btn{nullptr};
    QPushButton* m_scan_networks_btn{nullptr};

    // =========================================================================
    // State
    // =========================================================================
    QList<int> m_search_matches;
    int m_search_index{-1};
    QString m_save_path;
    LogToggleSwitch* m_logToggle{nullptr};
};

}  // namespace sak
