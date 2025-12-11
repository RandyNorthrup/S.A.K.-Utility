#pragma once

#include "sak/user_profile_types.h"
#include <QDialog>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QGroupBox>

class QDir;
class QTreeWidgetItem;

namespace sak {

/**
 * @brief Dialog for customizing which folders/files to backup for a specific user
 * 
 * Allows granular control over:
 * - Standard folders (Documents, Desktop, Pictures, etc.)
 * - Application data (selective AppData items)
 * - Custom folder additions
 * - Include/exclude patterns per folder
 */
class PerUserCustomizationDialog : public QDialog {
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param profile User profile to customize
     * @param parent Parent widget
     */
    explicit PerUserCustomizationDialog(UserProfile& profile, QWidget* parent = nullptr);
    
    /**
     * @brief Get updated folder selections
     * @return Modified folder selections
     */
    QVector<FolderSelection> getFolderSelections() const;

private Q_SLOTS:
    void onSelectAll();
    void onSelectNone();
    void onSelectRecommended();
    void onTreeItemChanged(QTreeWidgetItem* item, int column);
    void onAddCustomFolder();
    void onRemoveFolder();
    void onExpandAll();
    void onCollapseAll();
    void updateSummary();

private:
    void setupUi();
    void populateTree();
    void addFolderToTree(const FolderSelection& selection, QTreeWidgetItem* parent = nullptr);
    void addDirectoryContents(const QDir& dir, QTreeWidgetItem* parent, qint64& totalSize, int& totalFiles, bool checked, int depth = 0, int maxDepth = 3);
    void calculateDirectorySize(const QDir& dir, qint64& totalSize, int& fileCount, int depth = 0, int maxDepth = 10);
    void setChildrenCheckState(QTreeWidgetItem* item, Qt::CheckState state);
    void updateParentCheckState(QTreeWidgetItem* item);
    void updateFolderCheckStates(QTreeWidgetItem* item);
    qint64 calculateTotalSize() const;
    
    UserProfile& m_profile;
    
    // UI Components
    QLabel* m_usernameLabel{nullptr};
    QLabel* m_profilePathLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    
    QTreeWidget* m_folderTree{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QPushButton* m_selectRecommendedButton{nullptr};
    QPushButton* m_addCustomButton{nullptr};
    QPushButton* m_removeButton{nullptr};
    QPushButton* m_customizeButton{nullptr};
    
    QGroupBox* m_appDataGroup{nullptr};
    QCheckBox* m_browserBookmarksCheck{nullptr};
    QCheckBox* m_emailSignaturesCheck{nullptr};
    QCheckBox* m_officeTemplatesCheck{nullptr};
    QCheckBox* m_vsCodeSettingsCheck{nullptr};
    
    QPushButton* m_okButton{nullptr};
    QPushButton* m_cancelButton{nullptr};
};

} // namespace sak
