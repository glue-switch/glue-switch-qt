#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QMap>
#include <QTranslator>

class QButtonGroup;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QTabBar;
class QToolButton;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    enum class AppMode {
        Edit,
        Use
    };

    enum class LanguagePreference {
        FollowSystem,
        English,
        Chinese
    };

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    enum class ResizeRegion {
        None,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight
    };

    struct FileEntry {
        QString id;
        QString targetPath;
        QString displayName;
        QString selectedState;
        QString activeState;
        QMap<QString, QString> states;
    };

    void setupWindow();
    void setupUi();
    void setupTitleBar();
    void setupResizeHandles();
    void setupStyles();
    void connectSignals();
    void applyLanguagePreference();
    void retranslateUi();
    void showSettingsDialog();

    void loadLibrary();
    bool saveLibrary();
    QString storageRoot() const;
    QString libraryFilePath() const;

    void importFiles(const QStringList &paths);
    void refreshFileList();
    void refreshStateTabs();
    void refreshEditor();
    void refreshModeUi();
    void refreshWindowState();
    void refreshFileStatuses();
    void updateSaveBadge(bool saved);
    void updateSelectionForCurrentFile();

    int currentFileIndex() const;
    FileEntry *currentFile();
    const FileEntry *currentFile() const;
    QString currentStateName() const;
    QString readTextFile(const QString &path) const;
    bool writeTextFile(const QString &path, const QString &content) const;

    bool maybeSavePendingChanges();
    bool saveCurrentStateContent();
    void markEditorDirty(bool dirty);
    void applyStateToTarget(FileEntry &entry, const QString &stateName);
    void setMode(AppMode mode);
    void selectFileById(const QString &fileId);
    QString displayStateName(const QString &stateName) const;

    void openFilesDialog();
    void handleFileSelectionChanged();
    void handleTabChanged(int index);
    void createState();
    void removeState(int index);
    void updateResizeHandles();
    ResizeRegion resizeRegionForWidget(const QObject *watched) const;
    QRect resizedGeometryForGlobalPos(const QPoint &globalPos) const;
    Qt::CursorShape cursorForResizeRegion(ResizeRegion region) const;
    void refreshTabHeaderStyles();

    QWidget *m_rootWidget = nullptr;
    QWidget *m_titleBar = nullptr;
    QLabel *m_titleLabel = nullptr;
    QPushButton *m_editModeButton = nullptr;
    QPushButton *m_useModeButton = nullptr;
    QButtonGroup *m_modeButtonGroup = nullptr;
    QToolButton *m_settingsButton = nullptr;
    QToolButton *m_minimizeButton = nullptr;
    QToolButton *m_maximizeButton = nullptr;
    QToolButton *m_closeButton = nullptr;

    QPushButton *m_openButton = nullptr;
    QToolButton *m_refreshButton = nullptr;
    QLabel *m_fileListLabel = nullptr;
    QListWidget *m_fileListWidget = nullptr;

    QWidget *m_dropHint = nullptr;
    QLabel *m_dropTextLabel = nullptr;
    QTabBar *m_stateTabBar = nullptr;
    QToolButton *m_addStateButton = nullptr;
    QLabel *m_editorTitleLabel = nullptr;
    QLabel *m_saveBadgeLabel = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    QPushButton *m_saveButton = nullptr;
    QWidget *m_leftResizeHandle = nullptr;
    QWidget *m_rightResizeHandle = nullptr;
    QWidget *m_topResizeHandle = nullptr;
    QWidget *m_bottomResizeHandle = nullptr;
    QWidget *m_topLeftResizeHandle = nullptr;
    QWidget *m_topRightResizeHandle = nullptr;
    QWidget *m_bottomLeftResizeHandle = nullptr;
    QWidget *m_bottomRightResizeHandle = nullptr;

    QList<FileEntry> m_files;
    QString m_currentFileId;
    AppMode m_mode = AppMode::Edit;
    LanguagePreference m_languagePreference = LanguagePreference::FollowSystem;
    QTranslator m_translator;
    QTranslator m_qtBaseTranslator;
    bool m_isEditorDirty = false;
    bool m_isUpdatingUi = false;
    bool m_draggingWindow = false;
    bool m_isResizing = false;
    QPoint m_dragOffset;
    QPoint m_resizeStartGlobalPos;
    QRect m_resizeStartGeometry;
    ResizeRegion m_activeResizeRegion = ResizeRegion::None;
};

#endif // MAINWINDOW_H
