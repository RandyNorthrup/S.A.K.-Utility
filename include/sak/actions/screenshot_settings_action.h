// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/network_diagnostic_types.h"
#include "sak/quick_action.h"

#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QString>
#include <QVector>
#include <qwindowdefs.h>  // WId

#include <optional>

class ScreenshotSettingsActionTests;  // unit-test friend (global namespace)

namespace sak {

/**
 * @brief Screenshot Settings Action
 *
 * Captures screenshots of configured key Windows Settings pages for documentation.
 */
class ScreenshotSettingsAction : public QuickAction {
    Q_OBJECT

    friend class ::ScreenshotSettingsActionTests;

public:
    explicit ScreenshotSettingsAction(const QString& output_location, QObject* parent = nullptr);

    QString name() const override { return "Screenshot Settings"; }
    QString description() const override { return "Capture screenshots of Windows Settings"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::EmergencyRecovery; }
    bool requiresAdmin() const override { return false; }

    void execute() override;

private:
    /// @brief Result of capturing all settings pages
    struct CaptureResult {
        int screenshots_taken{0};
        int failed_attempts{0};
        QStringList captured_pages;
        QStringList failed_pages;
        /// Pages this PC does not have, each as "<page>: <why>": not failures, and never saved
        /// as a picture of the page before them.
        QStringList absent_pages;
    };

    /// @brief One Settings page: the name its picture is saved under, the ms-settings: names to
    ///        open it by in the order tried (Microsoft's published name first, then the name
    ///        another Windows version uses), and whether it exists only with a Wi-Fi adapter.
    struct SettingsPage {
        QString name;
        QStringList uris;
        bool needs_wifi{false};
    };
    /// @brief Every page captured, About first. Every page is opened from About, which exists on
    ///        every Windows: a link that is no page leaves About on screen.
    [[nodiscard]] static QList<SettingsPage> settingsPages();

    /// How long a picture must hold still to count as drawn: twice the longest a page drawn half
    /// way held. Measured 2026-11-06, every page opened as the run opens it: at most 0.65 s on
    /// testpc1; in the capture VM Installed apps held its title over an empty page for 3.83 s,
    /// then six of its 32 apps for 3.62 s. What else held 3 s or more there was the page drawn:
    /// Storage with a loading ring on one row, Personalization before its wallpaper preview
    /// loaded, Privacy 17 px short of the end of its slide-in.
    static constexpr qint64 kPageSettleMs = 8000;
    /// How long About must hold still in a window of its own once it is known. It must also be
    /// close to the About learned at the start, so no other page, half drawn or not, passes.
    static constexpr qint64 kAboutSettleMs = 2000;
    /// Two pictures are of one page while fewer than one pixel in this many differ. Measured:
    /// every page differed from About in 21 to 45 % of the window (VM and testpc1); About
    /// differed from itself by at most 4.8 % while it slid in, and by under 0.9 % from one
    /// window to the next.
    static constexpr qint64 kSamePagePixelDenominator = 10;

    /// What waiting on the Settings window for one page came to.
    enum class PageOutcome {
        Captured,
        NotOnThisWindows,
        Failed
    };

    /// @brief Watches the pictures one Settings window gives after one ms-settings: link was
    ///        opened. Pure: each look carries its time, so the decision is tested without
    ///        Settings.
    struct PageWatch {
        /// What the window showed when the link was opened in it (About), or null for a window
        /// the link itself opened. A page is never a picture close to this one.
        QImage opened_on;
        /// The picture the window must settle on, when it is known (About, in a window of its
        /// own, once two such windows agreed on it), or null.
        QImage expected;
        qint64 settle_ms{kPageSettleMs};  ///< how long a picture must hold still to count
        QImage held;                      ///< the latest picture that was not blank
        qint64 held_since_ms{0};          ///< when that picture was first seen
        bool saw_opened_on{false};        ///< a picture close to opened_on was seen
        bool left_opened_on{false};       ///< a picture that is not close to opened_on was seen
        /// The page, once a picture has held still for the settle time: not blank, not close to
        /// opened_on, and close to expected when that is set. Null until then.
        [[nodiscard]] QImage look(const QImage& now, qint64 now_ms);
        /// When the wait runs out: a window that showed opened_on and never left it did not open
        /// a page (on Windows, that page does not exist there). Anything else, a page that never
        /// held still included, is a failed try. Nothing is saved for either.
        [[nodiscard]] PageOutcome outcomeAtDeadline() const;
    };
    /// @brief True when @p a and @p b are pictures of different pages: other sizes, or more than
    ///        a small share of their pixels apart. A blinking caret, a scroll bar fading in or a
    ///        highlight under the pointer leave a page the same page.
    [[nodiscard]] static bool picturesDiffer(const QImage& a, const QImage& b);
    /// @brief The watch for About in a window of its own: before About is known, any picture
    ///        that holds still for the page settle time; once it is known, @p learned_about,
    ///        held for the shorter About settle time.
    [[nodiscard]] static PageWatch aboutWatch(const QImage& learned_about);
    /// @brief True when About from the window before (@p previous) and from this one (@p now)
    ///        agree. A first window has nothing to agree with.
    [[nodiscard]] static bool aboutWindowsAgree(const QImage& previous, const QImage& now);
    /// About as Settings draws it, from two windows of its own that agreed; null until then.
    QImage m_about_page;

    /// @brief True when @p adapters hold a Wi-Fi adapter. Settings has no Wi-Fi page without one.
    [[nodiscard]] static bool hasWifiAdapter(const QVector<NetworkAdapterInfo>& adapters);
    /// @brief Whether this PC has a Wi-Fi adapter; nothing when its adapters cannot be listed.
    [[nodiscard]] static std::optional<bool> wifiAdapterPresent();
    /// @brief "this PC has no Wi-Fi adapter" and "this page does not exist on this Windows".
    [[nodiscard]] static QString absentReason(PageOutcome outcome, bool no_wifi_adapter);
    /// @brief One link's outcome from its tries: captured if one try captured it, not on this
    ///        Windows only if every try saw the page before, otherwise failed.
    [[nodiscard]] static PageOutcome outcomeOfAttempts(const QList<PageOutcome>& attempts);
    /// @brief Adds @p page_name to the list @p outcome puts it in.
    static void recordOutcome(const QString& page_name,
                              PageOutcome outcome,
                              bool no_wifi_adapter,
                              CaptureResult* capture);

    int detectMonitorCount();
    /// @brief Count and log the connected monitors. MUST run on the GUI thread
    ///        (QGuiApplication::screens()/QScreen access is GUI-thread-affine);
    ///        detectMonitorCount() marshals it there from the worker thread.
    static int countMonitorsOnGui();
    /// @brief True when a picture is one colour throughout, which is what copying a window drawn
    ///        by the compositor gives. Such a picture is a failed capture, never a saved one.
    [[nodiscard]] static bool pictureIsBlank(const QImage& image);
    /// @brief The picture @p window renders of itself, taken on the GUI thread
    ///        (BlockingQueuedConnection) so a worker thread can capture safely; null (fails
    ///        closed) if there is no QCoreApplication or the invocation cannot be marshalled.
    static QImage renderOnGui(WId window);
    /// @brief Brings @p window to the front and feeds @p watch what it shows until the watch has
    ///        the page, the wait runs out, or the run is cancelled. @p page is set to the page.
    PageOutcome waitForDrawnPage(WId window, PageWatch* watch, QImage* page);

    /// @brief Opens About in a Settings window of its own, the one before closed, and waits for
    ///        it to settle on About (on m_about_page, once that is known). @p window is set to the
    ///        window and @p about to its picture.
    PageOutcome openAboutInItsOwnWindow(WId* window, QImage* about);
    /// @brief Learns m_about_page: About in windows of its own until two in a row agree.
    bool learnAboutPage();
    /// @brief learnAboutPage(), ending the run as failed (or cancelled) when it cannot.
    bool startFromAbout(const QDateTime& start_time);
    /// @brief Ends the run as failed when a Settings window is already open; true when it did.
    bool refuseWhileSettingsIsOpen(const QDateTime& start_time);
    /// @brief One try at @p ms_uri: About in a window of its own, then the link in that window,
    ///        compared against the About it showed when the link was opened.
    PageOutcome tryPageFromAbout(const QString& ms_uri, const QString& page_name, QImage* page);
    /// @brief Opens ms-settings:@p ms_uri through explorer.exe; false when it could not start.
    [[nodiscard]] static bool openSettingsLink(const QString& ms_uri);

    /// @brief Each of @p page's names in turn until one opens; NotOnThisWindows when none does.
    PageOutcome captureSettingsPage(const SettingsPage& page,
                                    const QString& output_dir_path,
                                    const QString& timestamp);
    /// @brief Saves ms-settings:@p ms_uri's page, trying again after a try that failed.
    PageOutcome captureSettingsUri(const QString& ms_uri,
                                   const QString& page_name,
                                   const QDir& output_dir,
                                   const QString& timestamp);
    /// @brief The visible, not minimized Settings frame window, found by EnumWindows, or 0.
    static WId findSettingsWindow();
    /// @brief True when any Settings window is open, minimized or not.
    [[nodiscard]] static bool settingsWindowIsOpen();
    /// @brief Poll (bounded, cancellable) until a Settings window exists and answers messages.
    WId findRespondingSettingsWindow();
    /// @brief WM_CLOSE to the Settings window, then a bounded wait for it to go. The run starts
    ///        only with no Settings window open, so the window is always one this run opened.
    ///        False when it was still open when the wait ran out.
    bool closeSettingsWindowWeOpened();
    /// @brief Saves @p page as @p page_name's picture in @p output_dir.
    [[nodiscard]] static PageOutcome savePage(const QImage& page,
                                              const QDir& output_dir,
                                              const QString& page_name,
                                              const QString& timestamp);
    /// @brief Write the screenshot report to disk.
    /// @return true only if the report was fully written and committed; a write
    /// failure is surfaced (the report path is not advertised as if it exists).
    bool generateReport(const QString& output_dir_path,
                        const QString& timestamp,
                        int monitor_count,
                        const CaptureResult& capture);

    /// @brief Build the full screenshot report text (pure; no I/O).
    static QString buildScreenshotReportText(int monitor_count,
                                             const CaptureResult& capture,
                                             const QString& output_dir_path,
                                             const QString& timestamp);

    /// @brief Structured-log line for the report: the real path only when the
    /// report was actually written, otherwise a write-failure marker.
    static QString reportPathLine(bool report_written, const QString& report_path);

    /// Ends the run as failed when the output folder is blank or relative; true when it did.
    bool rejectInvalidOutputFolder();

    /// Creates the run's capture folder; the error to report when it cannot (empty on success).
    [[nodiscard]] static QString createCaptureFolder(const QDir& output_dir);

    /// @brief Context needed to assemble the final execution result
    struct CaptureContext {
        int total_pages{0};
        int monitor_count{0};
        QString timestamp;
        QDateTime start_time;
        bool report_written{false};
    };

    /// @brief Assemble structured log and ExecutionResult from capture outcome
    void buildExecutionResult(const CaptureResult& capture,
                              const QDir& output_dir,
                              const CaptureContext& context);
};

}  // namespace sak
