#include "mainwindow.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTextOption>
#include <QToolButton>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

const QString kDefaultStateName = QStringLiteral("默认");
constexpr int kResizeHandleThickness = 8;
constexpr int kResizeCornerSize = 12;

QString modeToString(MainWindow::AppMode mode)
{
    return mode == MainWindow::AppMode::Edit ? QStringLiteral("edit") : QStringLiteral("use");
}

MainWindow::AppMode modeFromString(const QString &value)
{
    return value == QStringLiteral("use") ? MainWindow::AppMode::Use : MainWindow::AppMode::Edit;
}

QPoint globalMousePosition(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

QIcon loadIcon(const QString &path)
{
    return QIcon(path);
}

#ifdef Q_OS_WIN
void enableFramelessResize(HWND hwnd)
{
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR updatedStyle = style | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;
    if (updatedStyle != style) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, updatedStyle);
        SetWindowPos(hwnd,
                     nullptr,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}
#endif

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupWindow();
    setupUi();
    setupResizeHandles();
    setupStyles();
    connectSignals();
    loadLibrary();
    refreshModeUi();
    refreshFileList();
    refreshStateTabs();
    refreshEditor();
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeSavePendingChanges()) {
        event->ignore();
        return;
    }

    saveLibrary();
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_mode == AppMode::Use) {
        event->ignore();
        return;
    }

    const QMimeData *mimeData = event->mimeData();
    if (mimeData != nullptr && mimeData->hasUrls()) {
        event->acceptProposedAction();
        return;
    }

    event->ignore();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (m_mode == AppMode::Use) {
        event->ignore();
        return;
    }

    QStringList paths;
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }

    importFiles(paths);
    event->acceptProposedAction();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    const ResizeRegion resizeRegion = resizeRegionForWidget(watched);
    if (resizeRegion != ResizeRegion::None) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton && !isMaximized() && !isFullScreen()) {
                m_isResizing = true;
                m_activeResizeRegion = resizeRegion;
                m_resizeStartGlobalPos = globalMousePosition(mouseEvent);
                m_resizeStartGeometry = geometry();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (m_isResizing && mouseEvent->buttons().testFlag(Qt::LeftButton)) {
                setGeometry(resizedGeometryForGlobalPos(globalMousePosition(mouseEvent)));
                return true;
            }

            if (!m_isResizing) {
                setCursor(cursorForResizeRegion(resizeRegion));
            }
            return false;
        } else if (event->type() == QEvent::Enter) {
            if (!m_isResizing) {
                setCursor(cursorForResizeRegion(resizeRegion));
            }
            return false;
        } else if (event->type() == QEvent::Leave) {
            if (!m_isResizing) {
                unsetCursor();
            }
            return false;
        } else if (event->type() == QEvent::MouseButtonRelease) {
            m_isResizing = false;
            m_activeResizeRegion = ResizeRegion::None;
            unsetCursor();
            return true;
        }
    }

    if (watched == m_titleBar || watched == m_titleLabel) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton && !isMaximized()) {
                m_draggingWindow = true;
                m_dragOffset = globalMousePosition(mouseEvent) - frameGeometry().topLeft();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove && m_draggingWindow) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->buttons().testFlag(Qt::LeftButton)) {
                move(globalMousePosition(mouseEvent) - m_dragOffset);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            m_draggingWindow = false;
            return true;
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            if (isMaximized()) {
                showNormal();
            } else {
                showMaximized();
            }
            refreshWindowState();
            return true;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    Q_UNUSED(eventType);

    if (isMaximized() || isFullScreen()) {
        return QMainWindow::nativeEvent(eventType, message, result);
    }

    MSG *msg = static_cast<MSG *>(message);
    if (msg->message == WM_NCHITTEST) {
        const LONG borderWidth = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
        const RECT windowRect = [this]() {
            RECT rect {};
            const QRect geometry = frameGeometry();
            rect.left = geometry.left();
            rect.top = geometry.top();
            rect.right = geometry.right();
            rect.bottom = geometry.bottom();
            return rect;
        }();

        const LONG x = static_cast<LONG>(static_cast<short>(LOWORD(msg->lParam)));
        const LONG y = static_cast<LONG>(static_cast<short>(HIWORD(msg->lParam)));

        const bool onLeft = x >= windowRect.left && x < windowRect.left + borderWidth;
        const bool onRight = x <= windowRect.right && x > windowRect.right - borderWidth;
        const bool onTop = y >= windowRect.top && y < windowRect.top + borderWidth;
        const bool onBottom = y <= windowRect.bottom && y > windowRect.bottom - borderWidth;

        if (onTop && onLeft) {
            *result = HTTOPLEFT;
            return true;
        }
        if (onTop && onRight) {
            *result = HTTOPRIGHT;
            return true;
        }
        if (onBottom && onLeft) {
            *result = HTBOTTOMLEFT;
            return true;
        }
        if (onBottom && onRight) {
            *result = HTBOTTOMRIGHT;
            return true;
        }
        if (onLeft) {
            *result = HTLEFT;
            return true;
        }
        if (onRight) {
            *result = HTRIGHT;
            return true;
        }
        if (onTop) {
            *result = HTTOP;
            return true;
        }
        if (onBottom) {
            *result = HTBOTTOM;
            return true;
        }
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif

    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::setupWindow()
{
    setWindowTitle(tr("Glue Switch"));
    setMinimumSize(900, 520);
    resize(900, 520);
    setAcceptDrops(true);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);

#ifdef Q_OS_WIN
    enableFramelessResize(reinterpret_cast<HWND>(winId()));
#endif
}

void MainWindow::setupUi()
{
    m_rootWidget = new QWidget(this);
    m_rootWidget->setObjectName(QStringLiteral("RootWidget"));
    setCentralWidget(m_rootWidget);

    auto *rootLayout = new QVBoxLayout(m_rootWidget);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    setupTitleBar();
    rootLayout->addWidget(m_titleBar);

    auto *contentWidget = new QWidget(m_rootWidget);
    auto *contentLayout = new QHBoxLayout(contentWidget);
    contentLayout->setContentsMargins(16, 16, 16, 16);
    contentLayout->setSpacing(16);
    rootLayout->addWidget(contentWidget, 1);

    auto *leftPanel = new QFrame(contentWidget);
    leftPanel->setObjectName(QStringLiteral("SidePanel"));
    leftPanel->setFixedWidth(310);
    auto *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(14, 14, 14, 14);
    leftLayout->setSpacing(12);

    auto *listHeaderLayout = new QHBoxLayout();
    listHeaderLayout->setSpacing(10);

    m_openButton = new QPushButton(loadIcon(QStringLiteral(":/assets/file_open.png")), tr("打开文件"), leftPanel);
    m_openButton->setObjectName(QStringLiteral("PrimaryButton"));
    m_openButton->setMinimumHeight(42);
    listHeaderLayout->addWidget(m_openButton, 1);

    m_refreshButton = new QToolButton(leftPanel);
    m_refreshButton->setObjectName(QStringLiteral("GhostButton"));
    m_refreshButton->setIcon(loadIcon(QStringLiteral(":/assets/file_refresh.png")));
    m_refreshButton->setIconSize(QSize(18, 18));
    m_refreshButton->setToolTip(tr("刷新文件状态"));
    m_refreshButton->setFixedSize(42, 42);
    listHeaderLayout->addWidget(m_refreshButton, 0, Qt::AlignRight);
    leftLayout->addLayout(listHeaderLayout);

    auto *fileListLabel = new QLabel(tr("文件列表"), leftPanel);
    fileListLabel->setObjectName(QStringLiteral("SectionLabel"));
    leftLayout->addWidget(fileListLabel);

    m_fileListWidget = new QListWidget(leftPanel);
    m_fileListWidget->setObjectName(QStringLiteral("FileListWidget"));
    m_fileListWidget->setFrameShape(QFrame::NoFrame);
    m_fileListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_fileListWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    m_fileListWidget->setSpacing(8);
    leftLayout->addWidget(m_fileListWidget, 1);

    m_dropHint = new QFrame(leftPanel);
    m_dropHint->setObjectName(QStringLiteral("DropHint"));
    m_dropHint->setMinimumHeight(88);
    auto *dropLayout = new QVBoxLayout(m_dropHint);
    dropLayout->setContentsMargins(10, 16, 10, 16);
    dropLayout->setSpacing(8);

    auto *dropIconLabel = new QLabel(m_dropHint);
    dropIconLabel->setAlignment(Qt::AlignCenter);
    dropIconLabel->setPixmap(loadIcon(QStringLiteral(":/assets/file_drop.png")).pixmap(26, 26));
    dropLayout->addWidget(dropIconLabel);

    auto *dropTextLabel = new QLabel(tr("拖入文件到此处"), m_dropHint);
    dropTextLabel->setObjectName(QStringLiteral("DropHintLabel"));
    dropTextLabel->setAlignment(Qt::AlignCenter);
    dropLayout->addWidget(dropTextLabel);
    leftLayout->addWidget(m_dropHint);

    auto *rightPanel = new QFrame(contentWidget);
    rightPanel->setObjectName(QStringLiteral("EditorPanel"));
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(12);

    auto *tabRow = new QHBoxLayout();
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(8);

    m_stateTabBar = new QTabBar(rightPanel);
    m_stateTabBar->setObjectName(QStringLiteral("StateTabBar"));
    m_stateTabBar->setDocumentMode(true);
    m_stateTabBar->setDrawBase(false);
    m_stateTabBar->setExpanding(false);
    m_stateTabBar->setUsesScrollButtons(true);
    tabRow->addWidget(m_stateTabBar, 1);

    m_addStateButton = new QToolButton(rightPanel);
    m_addStateButton->setObjectName(QStringLiteral("StateAddButton"));
    m_addStateButton->setIcon(loadIcon(QStringLiteral(":/assets/tab_add.png")));
    m_addStateButton->setIconSize(QSize(16, 16));
    m_addStateButton->setToolTip(tr("新建状态"));
    m_addStateButton->setFixedSize(36, 36);
    tabRow->addWidget(m_addStateButton);
    rightLayout->addLayout(tabRow);

    auto *editorCard = new QFrame(rightPanel);
    editorCard->setObjectName(QStringLiteral("EditorCard"));
    auto *editorLayout = new QVBoxLayout(editorCard);
    editorLayout->setContentsMargins(16, 16, 16, 16);
    editorLayout->setSpacing(12);

    auto *editorHeader = new QHBoxLayout();
    editorHeader->setSpacing(8);
    m_editorTitleLabel = new QLabel(tr("未选择文件"), editorCard);
    m_editorTitleLabel->setObjectName(QStringLiteral("EditorTitle"));
    editorHeader->addWidget(m_editorTitleLabel, 1);

    m_saveBadgeLabel = new QLabel(tr("已保存"), editorCard);
    m_saveBadgeLabel->setObjectName(QStringLiteral("SaveBadge"));
    editorHeader->addWidget(m_saveBadgeLabel, 0, Qt::AlignRight);
    editorLayout->addLayout(editorHeader);

    m_editor = new QPlainTextEdit(editorCard);
    m_editor->setObjectName(QStringLiteral("Editor"));
    m_editor->setPlaceholderText(tr("在编辑模式下维护不同状态的文件内容，在使用模式下预览并切换状态。"));
    m_editor->setWordWrapMode(QTextOption::NoWrap);
    QFont editorFont(QStringLiteral("Consolas"));
    editorFont.setStyleHint(QFont::Monospace);
    editorFont.setPointSize(11);
    m_editor->setFont(editorFont);
    editorLayout->addWidget(m_editor, 1);

    m_saveButton = new QPushButton(loadIcon(QStringLiteral(":/assets/file_save.png")), tr("保存"), editorCard);
    m_saveButton->setObjectName(QStringLiteral("PrimaryButton"));
    m_saveButton->setMinimumHeight(40);
    m_saveButton->setFixedWidth(110);
    editorLayout->addWidget(m_saveButton, 0, Qt::AlignLeft);

    rightLayout->addWidget(editorCard, 1);

    contentLayout->addWidget(leftPanel);
    contentLayout->addWidget(rightPanel, 1);
}

void MainWindow::setupTitleBar()
{
    m_titleBar = new QWidget(m_rootWidget);
    m_titleBar->setObjectName(QStringLiteral("TitleBar"));
    m_titleBar->setFixedHeight(52);
    m_titleBar->installEventFilter(this);

    auto *titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(16, 8, 12, 8);
    titleLayout->setSpacing(10);

    auto *logoLabel = new QLabel(m_titleBar);
    logoLabel->setPixmap(loadIcon(QStringLiteral(":/assets/title_logo.png")).pixmap(24, 24));
    titleLayout->addWidget(logoLabel);

    m_titleLabel = new QLabel(tr("Glue Switch"), m_titleBar);
    m_titleLabel->setObjectName(QStringLiteral("WindowTitleLabel"));
    m_titleLabel->installEventFilter(this);
    titleLayout->addWidget(m_titleLabel);
    titleLayout->addStretch(1);

    auto *modeContainer = new QWidget(m_titleBar);
    modeContainer->setObjectName(QStringLiteral("ModeContainer"));
    auto *modeLayout = new QHBoxLayout(modeContainer);
    modeLayout->setContentsMargins(4, 4, 4, 4);
    modeLayout->setSpacing(4);

    m_editModeButton = new QPushButton(tr("编辑"), modeContainer);
    m_editModeButton->setCheckable(true);
    m_editModeButton->setObjectName(QStringLiteral("ModeButton"));
    m_useModeButton = new QPushButton(tr("使用"), modeContainer);
    m_useModeButton->setCheckable(true);
    m_useModeButton->setObjectName(QStringLiteral("ModeButton"));
    modeLayout->addWidget(m_editModeButton);
    modeLayout->addWidget(m_useModeButton);

    m_modeButtonGroup = new QButtonGroup(this);
    m_modeButtonGroup->setExclusive(true);
    m_modeButtonGroup->addButton(m_editModeButton, static_cast<int>(AppMode::Edit));
    m_modeButtonGroup->addButton(m_useModeButton, static_cast<int>(AppMode::Use));

    titleLayout->addWidget(modeContainer);

    m_settingsButton = new QToolButton(m_titleBar);
    m_settingsButton->setObjectName(QStringLiteral("WindowButton"));
    m_settingsButton->setIcon(loadIcon(QStringLiteral(":/assets/title_settings.png")));
    m_settingsButton->setIconSize(QSize(14, 14));
    m_settingsButton->setToolTip(tr("关于 Glue Switch"));
    m_settingsButton->setFixedSize(28, 28);
    titleLayout->addWidget(m_settingsButton);

    m_minimizeButton = new QToolButton(m_titleBar);
    m_minimizeButton->setObjectName(QStringLiteral("WindowButton"));
    m_minimizeButton->setIcon(loadIcon(QStringLiteral(":/assets/window_minimize.png")));
    m_minimizeButton->setIconSize(QSize(14, 14));
    m_minimizeButton->setFixedSize(28, 28);
    titleLayout->addWidget(m_minimizeButton);

    m_maximizeButton = new QToolButton(m_titleBar);
    m_maximizeButton->setObjectName(QStringLiteral("WindowButton"));
    m_maximizeButton->setIcon(loadIcon(QStringLiteral(":/assets/window_maximize.png")));
    m_maximizeButton->setIconSize(QSize(14, 14));
    m_maximizeButton->setFixedSize(28, 28);
    titleLayout->addWidget(m_maximizeButton);

    m_closeButton = new QToolButton(m_titleBar);
    m_closeButton->setObjectName(QStringLiteral("CloseButton"));
    m_closeButton->setIcon(loadIcon(QStringLiteral(":/assets/window_close.png")));
    m_closeButton->setIconSize(QSize(14, 14));
    m_closeButton->setFixedSize(28, 28);
    titleLayout->addWidget(m_closeButton);
}

void MainWindow::setupResizeHandles()
{
    auto createHandle = [this](ResizeRegion region, const QString &name) {
        auto *handle = new QWidget(m_rootWidget);
        handle->setObjectName(name);
        handle->setProperty("resizeRegion", static_cast<int>(region));
        handle->setMouseTracking(true);
        handle->installEventFilter(this);
        handle->raise();
        handle->setStyleSheet(QStringLiteral("background: transparent;"));
        return handle;
    };

    m_leftResizeHandle = createHandle(ResizeRegion::Left, QStringLiteral("LeftResizeHandle"));
    m_rightResizeHandle = createHandle(ResizeRegion::Right, QStringLiteral("RightResizeHandle"));
    m_topResizeHandle = createHandle(ResizeRegion::Top, QStringLiteral("TopResizeHandle"));
    m_bottomResizeHandle = createHandle(ResizeRegion::Bottom, QStringLiteral("BottomResizeHandle"));
    m_topLeftResizeHandle = createHandle(ResizeRegion::TopLeft, QStringLiteral("TopLeftResizeHandle"));
    m_topRightResizeHandle = createHandle(ResizeRegion::TopRight, QStringLiteral("TopRightResizeHandle"));
    m_bottomLeftResizeHandle = createHandle(ResizeRegion::BottomLeft, QStringLiteral("BottomLeftResizeHandle"));
    m_bottomRightResizeHandle = createHandle(ResizeRegion::BottomRight, QStringLiteral("BottomRightResizeHandle"));

    updateResizeHandles();
}

void MainWindow::setupStyles()
{
    setStyleSheet(QStringLiteral(
        "QMainWindow { background: #f3f6fb; }"
        "#RootWidget { background: #f3f6fb; }"
        "#TitleBar { background: #2f67e4; }"
        "#WindowTitleLabel { color: white; font-size: 18px; font-weight: 700; }"
        "#ModeContainer { background: rgba(255, 255, 255, 0.18); border-radius: 16px; }"
        "#ModeButton { color: rgba(255, 255, 255, 0.9); background: transparent; border: none; "
            "border-radius: 12px; padding: 6px 18px; font-weight: 600; min-width: 64px; }"
        "#ModeButton:checked { background: white; color: #2f67e4; }"
        "#WindowButton, #CloseButton { background: transparent; border: none; border-radius: 8px; }"
        "#WindowButton:hover { background: rgba(255, 255, 255, 0.18); }"
        "#CloseButton:hover { background: #ff5a5f; }"
        "#SidePanel, #EditorCard { background: white; border-radius: 16px; }"
        "#EditorPanel { background: transparent; }"
        "#SectionLabel { color: #7b8798; font-size: 12px; font-weight: 600; }"
        "#PrimaryButton { background: #2f67e4; color: white; border: none; border-radius: 10px; "
            "padding: 0 16px; font-weight: 600; }"
        "#PrimaryButton:hover { background: #2558c8; }"
        "#PrimaryButton:disabled { background: #dbe4f6; color: #94a0b8; }"
        "#GhostButton, #StateAddButton { background: #f4f7fd; border: 1px solid #dfe7f3; border-radius: 10px; }"
        "#GhostButton:hover, #StateAddButton:hover { background: #ebf1ff; }"
        "#GhostButton:disabled, #StateAddButton:disabled { background: #f5f7fb; border-color: #edf2f8; }"
        "#FileListWidget { background: transparent; border: none; outline: 0; }"
        "#DropHint { background: #f7faff; border: 2px dashed #d7e3fb; border-radius: 12px; }"
        "#DropHintLabel { color: #9ba8bf; font-size: 13px; }"
        "#EditorTitle { color: #202938; font-size: 16px; font-weight: 700; }"
        "#SaveBadge { background: #edf4ff; color: #6c8ee8; border-radius: 12px; padding: 6px 12px; font-weight: 600; }"
        "QTabBar::tab { background: #f5f7fb; color: #7b8798; border-radius: 10px; min-height: 42px; padding: 0 2px; margin-right: 8px; }"
        "QTabBar::tab:selected { background: #2f67e4; color: white; }"
        "#StateTabHeader { background: transparent; }"
        "#StateTabTitle { color: #7b8798; font-weight: 600; }"
        "#StateTabCloseButton { background: transparent; border: none; padding: 0; margin: 0; }"
        "#StateTabCloseButton:hover { background: transparent; }"
        "#Editor { background: #f5f7fb; border: none; border-radius: 14px; color: #202938; padding: 14px; }"
        "#Editor:disabled { color: #7f8aa0; }"
    ));
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateResizeHandles();
}

void MainWindow::connectSignals()
{
    connect(m_openButton, &QPushButton::clicked, this, &MainWindow::openFilesDialog);
    connect(m_refreshButton, &QToolButton::clicked, this, &MainWindow::refreshFileStatuses);
    connect(m_fileListWidget, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *, QListWidgetItem *) {
        handleFileSelectionChanged();
    });
    connect(m_stateTabBar, &QTabBar::currentChanged, this, [this](int index) {
        Q_UNUSED(index);
        refreshTabHeaderStyles();
    });
    connect(m_stateTabBar, &QTabBar::currentChanged, this, &MainWindow::handleTabChanged);
    connect(m_addStateButton, &QToolButton::clicked, this, &MainWindow::createState);
    connect(m_saveButton, &QPushButton::clicked, this, [this]() {
        saveCurrentStateContent();
    });
    connect(m_editor, &QPlainTextEdit::textChanged, this, [this]() {
        if (!m_isUpdatingUi && m_mode == AppMode::Edit) {
            markEditorDirty(true);
        }
    });

    connect(m_editModeButton, &QPushButton::clicked, this, [this]() {
        setMode(AppMode::Edit);
    });
    connect(m_useModeButton, &QPushButton::clicked, this, [this]() {
        setMode(AppMode::Use);
    });

    connect(m_settingsButton, &QToolButton::clicked, this, [this]() {
        QMessageBox::information(
            this,
            tr("关于 Glue Switch"),
            tr("Glue Switch 是一个轻量级配置文件切换工具。\n\n编辑模式用于维护不同状态的内容，使用模式用于将指定状态快速写回目标文件。"));
    });
    connect(m_minimizeButton, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(m_maximizeButton, &QToolButton::clicked, this, [this]() {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
        refreshWindowState();
    });
    connect(m_closeButton, &QToolButton::clicked, this, &QWidget::close);
}

void MainWindow::loadLibrary()
{
    QFile file(libraryFilePath());
    if (!file.exists()) {
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("读取失败"), tr("无法读取 Glue Switch 本地数据。"));
        return;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!document.isObject()) {
        return;
    }

    const QJsonObject root = document.object();
    m_mode = modeFromString(root.value(QStringLiteral("mode")).toString());
    m_currentFileId = root.value(QStringLiteral("currentFileId")).toString();

    const QJsonArray filesArray = root.value(QStringLiteral("files")).toArray();
    for (const QJsonValue &value : filesArray) {
        const QJsonObject object = value.toObject();
        FileEntry entry;
        entry.id = object.value(QStringLiteral("id")).toString(QUuid::createUuid().toString(QUuid::WithoutBraces));
        entry.targetPath = object.value(QStringLiteral("targetPath")).toString();
        entry.displayName = object.value(QStringLiteral("displayName")).toString(QFileInfo(entry.targetPath).fileName());
        entry.selectedState = object.value(QStringLiteral("selectedState")).toString(kDefaultStateName);
        entry.activeState = object.value(QStringLiteral("activeState")).toString(kDefaultStateName);

        const QJsonArray stateArray = object.value(QStringLiteral("states")).toArray();
        for (const QJsonValue &stateValue : stateArray) {
            const QJsonObject stateObject = stateValue.toObject();
            entry.states.insert(stateObject.value(QStringLiteral("name")).toString(),
                                stateObject.value(QStringLiteral("content")).toString());
        }

        if (!entry.states.contains(kDefaultStateName)) {
            entry.states.insert(kDefaultStateName, readTextFile(entry.targetPath));
        }
        if (!entry.states.contains(entry.selectedState)) {
            entry.selectedState = kDefaultStateName;
        }
        if (!entry.states.contains(entry.activeState)) {
            entry.activeState = entry.selectedState;
        }

        m_files.append(entry);
    }

    if (m_files.isEmpty()) {
        m_currentFileId.clear();
    } else if (m_currentFileId.isEmpty()) {
        m_currentFileId = m_files.first().id;
    } else {
        bool found = false;
        for (const FileEntry &entry : std::as_const(m_files)) {
            if (entry.id == m_currentFileId) {
                found = true;
                break;
            }
        }
        if (!found) {
            m_currentFileId = m_files.first().id;
        }
    }
}

bool MainWindow::saveLibrary()
{
    QDir().mkpath(storageRoot());

    QJsonArray filesArray;
    for (const FileEntry &entry : std::as_const(m_files)) {
        QJsonArray statesArray;
        const QStringList stateNames = entry.states.keys();
        for (const QString &stateName : stateNames) {
            QJsonObject stateObject;
            stateObject.insert(QStringLiteral("name"), stateName);
            stateObject.insert(QStringLiteral("content"), entry.states.value(stateName));
            statesArray.append(stateObject);
        }

        QJsonObject object;
        object.insert(QStringLiteral("id"), entry.id);
        object.insert(QStringLiteral("targetPath"), entry.targetPath);
        object.insert(QStringLiteral("displayName"), entry.displayName);
        object.insert(QStringLiteral("selectedState"), entry.selectedState);
        object.insert(QStringLiteral("activeState"), entry.activeState);
        object.insert(QStringLiteral("states"), statesArray);
        filesArray.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("mode"), modeToString(m_mode));
    root.insert(QStringLiteral("currentFileId"), m_currentFileId);
    root.insert(QStringLiteral("files"), filesArray);

    QSaveFile file(libraryFilePath());
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("保存失败"), tr("无法写入 Glue Switch 本地数据。"));
        return false;
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

QString MainWindow::storageRoot() const
{
    return QDir::home().filePath(QStringLiteral(".glue-switch"));
}

QString MainWindow::libraryFilePath() const
{
    return QDir(storageRoot()).filePath(QStringLiteral("library.json"));
}

void MainWindow::importFiles(const QStringList &paths)
{
    if (m_mode == AppMode::Use) {
        return;
    }

    QString lastImportedId;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            continue;
        }

        const QString absolutePath = info.absoluteFilePath();
        bool alreadyImported = false;
        for (const FileEntry &entry : std::as_const(m_files)) {
            if (QFileInfo(entry.targetPath).absoluteFilePath() == absolutePath) {
                lastImportedId = entry.id;
                alreadyImported = true;
                break;
            }
        }
        if (alreadyImported) {
            continue;
        }

        FileEntry entry;
        entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        entry.targetPath = absolutePath;
        entry.displayName = info.fileName();
        entry.selectedState = kDefaultStateName;
        entry.activeState = kDefaultStateName;
        entry.states.insert(kDefaultStateName, readTextFile(absolutePath));
        m_files.append(entry);
        lastImportedId = entry.id;
    }

    if (!lastImportedId.isEmpty()) {
        m_currentFileId = lastImportedId;
    } else if (!m_files.isEmpty() && m_currentFileId.isEmpty()) {
        m_currentFileId = m_files.first().id;
    }

    saveLibrary();
    refreshFileList();
    refreshStateTabs();
    refreshEditor();
}

void MainWindow::refreshFileList()
{
    m_isUpdatingUi = true;
    m_fileListWidget->blockSignals(true);
    m_fileListWidget->clear();

    for (const FileEntry &entry : std::as_const(m_files)) {
        auto *item = new QListWidgetItem(m_fileListWidget);
        item->setData(Qt::UserRole, entry.id);
        item->setSizeHint(QSize(0, 58));

        auto *rowWidget = new QWidget(m_fileListWidget);
        const bool selected = entry.id == m_currentFileId;
        const bool exists = QFileInfo::exists(entry.targetPath);
        rowWidget->setStyleSheet(QStringLiteral(
            "QWidget { background: %1; border-radius: 10px; }"
            "QLabel#FileName { color: #243041; font-weight: 600; }"
            "QLabel#StateName { color: %2; font-weight: 600; }")
                                     .arg(selected ? QStringLiteral("#edf4ff") : QStringLiteral("transparent"),
                                          exists ? QStringLiteral("#2f67e4") : QStringLiteral("#e05b5b")));

        auto *layout = new QHBoxLayout(rowWidget);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(10);

        auto *iconLabel = new QLabel(rowWidget);
        const QString iconPath = selected
                ? QStringLiteral(":/assets/file_item_active.png")
                : QStringLiteral(":/assets/file_item.png");
        iconLabel->setPixmap(loadIcon(iconPath).pixmap(18, 18));
        layout->addWidget(iconLabel);

        auto *nameLabel = new QLabel(entry.displayName, rowWidget);
        nameLabel->setObjectName(QStringLiteral("FileName"));
        layout->addWidget(nameLabel);

        auto *stateLabel = new QLabel(exists ? entry.activeState : tr("文件缺失"), rowWidget);
        stateLabel->setObjectName(QStringLiteral("StateName"));
        layout->addWidget(stateLabel, 0, Qt::AlignRight);

        rowWidget->setToolTip(entry.targetPath);
        m_fileListWidget->setItemWidget(item, rowWidget);
    }

    updateSelectionForCurrentFile();
    m_fileListWidget->blockSignals(false);
    m_isUpdatingUi = false;
}

void MainWindow::refreshStateTabs()
{
    m_isUpdatingUi = true;
    m_stateTabBar->blockSignals(true);
    while (m_stateTabBar->count() > 0) {
        m_stateTabBar->removeTab(0);
    }

    FileEntry *entry = currentFile();
    if (entry != nullptr) {
        const QStringList stateNames = entry->states.keys();
        for (const QString &stateName : stateNames) {
            const int index = m_stateTabBar->addTab(QString());
            m_stateTabBar->setTabButton(index, QTabBar::LeftSide, nullptr);
            m_stateTabBar->setTabButton(index, QTabBar::RightSide, nullptr);
            m_stateTabBar->setTabData(index, stateName);

            auto *tabHeader = new QWidget(m_stateTabBar);
            tabHeader->setObjectName(QStringLiteral("StateTabHeader"));
            tabHeader->setFixedHeight(42);
            auto *headerLayout = new QHBoxLayout(tabHeader);
            headerLayout->setContentsMargins(12, 0, 8, 0);
            headerLayout->setSpacing(8);

            auto *titleLabel = new QLabel(stateName, tabHeader);
            titleLabel->setObjectName(QStringLiteral("StateTabTitle"));
            headerLayout->addWidget(titleLabel);

            if (m_mode == AppMode::Edit && stateName != kDefaultStateName) {
                auto *closeButton = new QToolButton(tabHeader);
                closeButton->setObjectName(QStringLiteral("StateTabCloseButton"));
                closeButton->setIcon(loadIcon(QStringLiteral(":/assets/tab_close.png")));
                closeButton->setIconSize(QSize(10, 10));
                closeButton->setAutoRaise(true);
                closeButton->setCursor(Qt::PointingHandCursor);
                closeButton->setFixedSize(12, 12);
                closeButton->setToolTip(tr("删除状态"));
                headerLayout->addWidget(closeButton);

                connect(closeButton, &QToolButton::clicked, this, [this, stateName]() {
                    for (int tabIndex = 0; tabIndex < m_stateTabBar->count(); ++tabIndex) {
                        if (m_stateTabBar->tabData(tabIndex).toString() == stateName) {
                            removeState(tabIndex);
                            return;
                        }
                    }
                });
            }

            m_stateTabBar->setTabButton(index, QTabBar::LeftSide, tabHeader);
        }

        m_stateTabBar->setTabsClosable(false);
        int selectedIndex = stateNames.indexOf(entry->selectedState);
        if (selectedIndex < 0 && !stateNames.isEmpty()) {
            entry->selectedState = stateNames.first();
            selectedIndex = 0;
        }
        if (selectedIndex >= 0) {
            m_stateTabBar->setCurrentIndex(selectedIndex);
        }
    } else {
        m_stateTabBar->setTabsClosable(false);
    }

    m_stateTabBar->blockSignals(false);
    m_isUpdatingUi = false;
    refreshTabHeaderStyles();
}

void MainWindow::refreshEditor()
{
    m_isUpdatingUi = true;

    const FileEntry *entry = currentFile();
    if (entry == nullptr) {
        m_editorTitleLabel->setText(tr("未选择文件"));
        m_editor->clear();
        m_editor->setReadOnly(true);
        m_saveButton->setEnabled(false);
        updateSaveBadge(true);
        m_isUpdatingUi = false;
        return;
    }

    const QString stateName = currentStateName();
    m_editorTitleLabel->setText(tr("%1 - %2状态").arg(entry->displayName, stateName));
    m_editor->setPlainText(entry->states.value(stateName));
    m_editor->setReadOnly(m_mode == AppMode::Use);
    m_saveButton->setEnabled(m_mode == AppMode::Edit);
    markEditorDirty(false);

    m_isUpdatingUi = false;
}

void MainWindow::refreshModeUi()
{
    m_editModeButton->setChecked(m_mode == AppMode::Edit);
    m_useModeButton->setChecked(m_mode == AppMode::Use);

    const bool isEditMode = m_mode == AppMode::Edit;
    m_openButton->setEnabled(isEditMode);
    m_addStateButton->setEnabled(isEditMode && currentFile() != nullptr);
    m_saveButton->setEnabled(isEditMode && currentFile() != nullptr);
    m_dropHint->setEnabled(isEditMode);
    m_editor->setReadOnly(!isEditMode);

    refreshStateTabs();
    refreshEditor();
}

void MainWindow::refreshWindowState()
{
    m_maximizeButton->setToolTip(isMaximized() ? tr("还原") : tr("最大化"));
}

void MainWindow::refreshFileStatuses()
{
    refreshFileList();
    refreshEditor();
    refreshTabHeaderStyles();
}

void MainWindow::updateSaveBadge(bool saved)
{
    if (saved) {
        m_saveBadgeLabel->setText(tr("已保存"));
        m_saveBadgeLabel->setStyleSheet(QStringLiteral("background:#edf4ff;color:#6c8ee8;border-radius:12px;padding:6px 12px;font-weight:600;"));
    } else {
        m_saveBadgeLabel->setText(tr("未保存"));
        m_saveBadgeLabel->setStyleSheet(QStringLiteral("background:#fff3df;color:#dd8c1d;border-radius:12px;padding:6px 12px;font-weight:600;"));
    }
}

void MainWindow::updateSelectionForCurrentFile()
{
    for (int row = 0; row < m_fileListWidget->count(); ++row) {
        QListWidgetItem *item = m_fileListWidget->item(row);
        if (item == nullptr) {
            continue;
        }
        if (item->data(Qt::UserRole).toString() == m_currentFileId) {
            m_fileListWidget->setCurrentRow(row);
            return;
        }
    }
}

int MainWindow::currentFileIndex() const
{
    for (int index = 0; index < m_files.size(); ++index) {
        if (m_files.at(index).id == m_currentFileId) {
            return index;
        }
    }
    return -1;
}

MainWindow::FileEntry *MainWindow::currentFile()
{
    const int index = currentFileIndex();
    if (index < 0) {
        return nullptr;
    }
    return &m_files[index];
}

const MainWindow::FileEntry *MainWindow::currentFile() const
{
    const int index = currentFileIndex();
    if (index < 0) {
        return nullptr;
    }
    return &m_files[index];
}

QString MainWindow::currentStateName() const
{
    const FileEntry *entry = currentFile();
    if (entry == nullptr) {
        return QString();
    }
    return entry->selectedState;
}

QString MainWindow::readTextFile(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

bool MainWindow::writeTextFile(const QString &path, const QString &content) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(content.toUtf8());
    return file.commit();
}

bool MainWindow::maybeSavePendingChanges()
{
    if (!m_isEditorDirty || m_mode != AppMode::Edit) {
        return true;
    }

    const QMessageBox::StandardButton result = QMessageBox::question(
        this,
        tr("保存修改"),
        tr("当前状态内容尚未保存，是否先保存再继续？"),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
        QMessageBox::Yes);

    if (result == QMessageBox::Cancel) {
        return false;
    }
    if (result == QMessageBox::Yes) {
        return saveCurrentStateContent();
    }
    return true;
}

bool MainWindow::saveCurrentStateContent()
{
    FileEntry *entry = currentFile();
    if (entry == nullptr || m_mode != AppMode::Edit) {
        return false;
    }

    entry->states.insert(entry->selectedState, m_editor->toPlainText());
    if (!saveLibrary()) {
        return false;
    }

    markEditorDirty(false);
    refreshEditor();
    return true;
}

void MainWindow::markEditorDirty(bool dirty)
{
    m_isEditorDirty = dirty;
    updateSaveBadge(!dirty);
}

void MainWindow::applyStateToTarget(FileEntry &entry, const QString &stateName)
{
    if (!writeTextFile(entry.targetPath, entry.states.value(stateName))) {
        QMessageBox::warning(this, tr("应用失败"), tr("无法将当前状态写入目标文件：\n%1").arg(entry.targetPath));
        return;
    }

    entry.activeState = stateName;
    saveLibrary();
    refreshFileList();
    refreshEditor();
}

void MainWindow::setMode(AppMode mode)
{
    if (mode == m_mode) {
        return;
    }

    if (m_mode == AppMode::Edit && !maybeSavePendingChanges()) {
        refreshModeUi();
        return;
    }

    m_mode = mode;
    if (m_mode == AppMode::Use) {
        if (FileEntry *entry = currentFile()) {
            applyStateToTarget(*entry, entry->selectedState);
        }
    } else {
        saveLibrary();
    }

    refreshModeUi();
    refreshFileList();
    refreshStateTabs();
    refreshEditor();
}

void MainWindow::selectFileById(const QString &fileId)
{
    m_currentFileId = fileId;
    updateSelectionForCurrentFile();
}

void MainWindow::openFilesDialog()
{
    if (m_mode == AppMode::Use) {
        return;
    }

    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        tr("导入文件"),
        QDir::homePath(),
        tr("所有文件 (*.*)"));

    importFiles(files);
}

void MainWindow::handleFileSelectionChanged()
{
    if (m_isUpdatingUi) {
        return;
    }

    QListWidgetItem *item = m_fileListWidget->currentItem();
    if (item == nullptr) {
        return;
    }

    const QString selectedId = item->data(Qt::UserRole).toString();
    if (selectedId == m_currentFileId) {
        return;
    }

    if (!maybeSavePendingChanges()) {
        updateSelectionForCurrentFile();
        return;
    }

    m_currentFileId = selectedId;
    refreshFileList();
    refreshStateTabs();
    refreshEditor();
    refreshModeUi();
}

void MainWindow::handleTabChanged(int index)
{
    if (m_isUpdatingUi || index < 0) {
        refreshTabHeaderStyles();
        return;
    }

    FileEntry *entry = currentFile();
    if (entry == nullptr) {
        refreshTabHeaderStyles();
        return;
    }

    const QString stateName = m_stateTabBar->tabData(index).toString();
    if (stateName == entry->selectedState) {
        refreshTabHeaderStyles();
        return;
    }

    if (m_mode == AppMode::Edit && !maybeSavePendingChanges()) {
        refreshStateTabs();
        return;
    }

    entry->selectedState = stateName;
    if (m_mode == AppMode::Use) {
        applyStateToTarget(*entry, stateName);
    } else {
        saveLibrary();
    }

    refreshFileList();
    refreshEditor();
    refreshTabHeaderStyles();
}

void MainWindow::createState()
{
    FileEntry *entry = currentFile();
    if (entry == nullptr || m_mode != AppMode::Edit) {
        return;
    }

    const QString stateName = QInputDialog::getText(
        this,
        tr("新建状态"),
        tr("请输入新状态名称：")).trimmed();

    if (stateName.isEmpty()) {
        return;
    }
    if (entry->states.contains(stateName)) {
        QMessageBox::information(this, tr("状态已存在"), tr("该状态名称已经存在，请使用其他名称。"));
        return;
    }

    entry->states.insert(stateName, entry->states.value(entry->selectedState));
    entry->selectedState = stateName;
    saveLibrary();
    refreshStateTabs();
    refreshEditor();
}

void MainWindow::removeState(int index)
{
    FileEntry *entry = currentFile();
    if (entry == nullptr || m_mode != AppMode::Edit || index < 0) {
        return;
    }

    const QString stateName = m_stateTabBar->tabData(index).toString();
    if (stateName == kDefaultStateName) {
        QMessageBox::information(this, tr("无法删除"), tr("默认状态不能被删除。"));
        return;
    }

    const QMessageBox::StandardButton result = QMessageBox::question(
        this,
        tr("删除状态"),
        tr("确认删除状态“%1”吗？").arg(stateName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    if (result != QMessageBox::Yes) {
        return;
    }

    entry->states.remove(stateName);
    if (entry->selectedState == stateName) {
        entry->selectedState = entry->states.contains(entry->activeState) ? entry->activeState : entry->states.firstKey();
    }
    if (entry->activeState == stateName) {
        entry->activeState = kDefaultStateName;
    }

    saveLibrary();
    refreshFileList();
    refreshStateTabs();
    refreshEditor();
}

void MainWindow::updateResizeHandles()
{
    if (m_rootWidget == nullptr) {
        return;
    }

    const bool enableHandles = !isMaximized() && !isFullScreen();
    const QList<QWidget *> handles = {
        m_leftResizeHandle,
        m_rightResizeHandle,
        m_topResizeHandle,
        m_bottomResizeHandle,
        m_topLeftResizeHandle,
        m_topRightResizeHandle,
        m_bottomLeftResizeHandle,
        m_bottomRightResizeHandle
    };
    for (QWidget *handle : handles) {
        if (handle != nullptr) {
            handle->setVisible(enableHandles);
        }
    }

    if (!enableHandles) {
        return;
    }

    const int width = m_rootWidget->width();
    const int height = m_rootWidget->height();
    const int edge = kResizeHandleThickness;
    const int corner = kResizeCornerSize;

    if (m_leftResizeHandle != nullptr) {
        m_leftResizeHandle->setGeometry(0, corner, edge, height - (corner * 2));
        m_leftResizeHandle->raise();
    }
    if (m_rightResizeHandle != nullptr) {
        m_rightResizeHandle->setGeometry(width - edge, corner, edge, height - (corner * 2));
        m_rightResizeHandle->raise();
    }
    if (m_topResizeHandle != nullptr) {
        m_topResizeHandle->setGeometry(corner, 0, width - (corner * 2), edge);
        m_topResizeHandle->raise();
    }
    if (m_bottomResizeHandle != nullptr) {
        m_bottomResizeHandle->setGeometry(corner, height - edge, width - (corner * 2), edge);
        m_bottomResizeHandle->raise();
    }
    if (m_topLeftResizeHandle != nullptr) {
        m_topLeftResizeHandle->setGeometry(0, 0, corner, corner);
        m_topLeftResizeHandle->raise();
    }
    if (m_topRightResizeHandle != nullptr) {
        m_topRightResizeHandle->setGeometry(width - corner, 0, corner, corner);
        m_topRightResizeHandle->raise();
    }
    if (m_bottomLeftResizeHandle != nullptr) {
        m_bottomLeftResizeHandle->setGeometry(0, height - corner, corner, corner);
        m_bottomLeftResizeHandle->raise();
    }
    if (m_bottomRightResizeHandle != nullptr) {
        m_bottomRightResizeHandle->setGeometry(width - corner, height - corner, corner, corner);
        m_bottomRightResizeHandle->raise();
    }
}

void MainWindow::refreshTabHeaderStyles()
{
    for (int index = 0; index < m_stateTabBar->count(); ++index) {
        QWidget *tabHeader = m_stateTabBar->tabButton(index, QTabBar::LeftSide);
        if (tabHeader == nullptr) {
            continue;
        }

        auto *titleLabel = tabHeader->findChild<QLabel *>(QStringLiteral("StateTabTitle"));
        auto *closeButton = tabHeader->findChild<QToolButton *>(QStringLiteral("StateTabCloseButton"));
        const bool selected = index == m_stateTabBar->currentIndex();

        if (titleLabel != nullptr) {
            titleLabel->setStyleSheet(selected
                                              ? QStringLiteral("color: white; font-weight: 600;")
                                              : QStringLiteral("color: #7b8798; font-weight: 600;"));
        }

        if (closeButton != nullptr) {
            closeButton->setIcon(loadIcon(selected
                                                  ? QStringLiteral(":/assets/tab_close_active.png")
                                                  : QStringLiteral(":/assets/tab_close.png")));
        }
    }
}

MainWindow::ResizeRegion MainWindow::resizeRegionForWidget(const QObject *watched) const
{
    if (watched == nullptr) {
        return ResizeRegion::None;
    }

    const QVariant value = watched->property("resizeRegion");
    if (!value.isValid()) {
        return ResizeRegion::None;
    }

    return static_cast<ResizeRegion>(value.toInt());
}

QRect MainWindow::resizedGeometryForGlobalPos(const QPoint &globalPos) const
{
    int left = m_resizeStartGeometry.left();
    int top = m_resizeStartGeometry.top();
    int right = m_resizeStartGeometry.right();
    int bottom = m_resizeStartGeometry.bottom();

    const int deltaX = globalPos.x() - m_resizeStartGlobalPos.x();
    const int deltaY = globalPos.y() - m_resizeStartGlobalPos.y();
    const int minRight = left + minimumWidth() - 1;
    const int minBottom = top + minimumHeight() - 1;

    switch (m_activeResizeRegion) {
    case ResizeRegion::Left:
    case ResizeRegion::TopLeft:
    case ResizeRegion::BottomLeft:
        left = qMin(m_resizeStartGeometry.left() + deltaX, m_resizeStartGeometry.right() - minimumWidth() + 1);
        break;
    default:
        break;
    }

    switch (m_activeResizeRegion) {
    case ResizeRegion::Right:
    case ResizeRegion::TopRight:
    case ResizeRegion::BottomRight:
        right = qMax(m_resizeStartGeometry.right() + deltaX, minRight);
        break;
    default:
        break;
    }

    switch (m_activeResizeRegion) {
    case ResizeRegion::Top:
    case ResizeRegion::TopLeft:
    case ResizeRegion::TopRight:
        top = qMin(m_resizeStartGeometry.top() + deltaY, m_resizeStartGeometry.bottom() - minimumHeight() + 1);
        break;
    default:
        break;
    }

    switch (m_activeResizeRegion) {
    case ResizeRegion::Bottom:
    case ResizeRegion::BottomLeft:
    case ResizeRegion::BottomRight:
        bottom = qMax(m_resizeStartGeometry.bottom() + deltaY, minBottom);
        break;
    default:
        break;
    }

    return QRect(QPoint(left, top), QPoint(right, bottom));
}

Qt::CursorShape MainWindow::cursorForResizeRegion(ResizeRegion region) const
{
    switch (region) {
    case ResizeRegion::Left:
    case ResizeRegion::Right:
        return Qt::SizeHorCursor;
    case ResizeRegion::Top:
    case ResizeRegion::Bottom:
        return Qt::SizeVerCursor;
    case ResizeRegion::TopLeft:
    case ResizeRegion::BottomRight:
        return Qt::SizeFDiagCursor;
    case ResizeRegion::TopRight:
    case ResizeRegion::BottomLeft:
        return Qt::SizeBDiagCursor;
    case ResizeRegion::None:
    default:
        return Qt::ArrowCursor;
    }
}
