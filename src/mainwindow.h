#pragma once

#include "model.h"

#include <QHash>
#include <QIcon>
#include <QMainWindow>
#include <QTimer>
#include <functional>

class Canvas;
class ColorButton;
class PaletteWidget;
class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QUndoStack;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
    void applySnapshot(const Document& d, int frame);  // used by undo commands
    bool openPath(const QString& path);
    // "Create new with size …" / "Open …". Returns false if cancelled.
    bool showStartDialog(bool startup);

protected:
    void closeEvent(QCloseEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    void buildActions();
    void buildDocks();
    void buildPropertyPanel(QWidget* parent);
    void buildMenus();
    void buildToolbars();
    QAction* act(const QString& text, const QKeySequence& ks, std::function<void()> fn, const QIcon& icon = QIcon());

    Frame& curFrame() { return m_doc.frames[m_cur]; }
    int mid(int field) const { return (m_selEpoch % 100000) * 64 + field; }
    void commit(const QString& text, int mergeId = -1);
    void setCurrentFrame(int i, bool fromTimeline = false);  // fromTimeline: keep a multi-selection made in the strip
    void refreshTimeline();
    void renderThumbs();
    void refreshProps();
    void updateTitle();
    bool maybeSave();
    QVector<int> selectedFrames() const;
    QString baseName() const;
    void log(const QString& text);
    void setPrompt(const QString& text);
    void addRecentFile(const QString& path);
    void rebuildRecentMenu();

    // file
    void newDoc();
    void openDoc();
    bool saveDoc();
    bool saveDocAs();
    void optimizeImages();
    void insertImage(const QString& path = QString(), QPointF at = QPointF(qQNaN(), qQNaN()));
    void exportFrame();
    void exportAnimation();
    void docSettings();
    // edit
    void copySel();
    void cutSel();
    void paste();
    void duplicateSel();
    void pasteShapes(const QVector<Shape>& shapes, const QVector<Joint>& joints, bool forceNewIds, bool nudge = true);
    // chapters
    void refreshChapters();
    void switchChapter(int c);
    void chapterNew();
    void chapterDuplicate();
    void chapterRename();
    void chapterDelete();
    void chapterMove(int dir);
    void chapterImport();
    void chapterExport();
    // library (saved objects, stored as small .tood files in the user's data folder)
    void refreshLibrary();
    void libraryAdd();
    void libraryInsert(const QString& path, QPointF at = QPointF(qQNaN(), qQNaN()));
    void libraryRename();
    void libraryDelete();
    void deleteSel();
    void reorder(int mode);
    void disconnectSel();
    void groupSel();
    void ungroupSel();
    void selectConnected();
    void refreshStructure();  // connection tree of the selected shape (Properties)
    void editText(int id);
    void editSingle(const std::function<void(Frame&, int, Shape&)>& fn, const QString& text, int field);
    void editAll(const std::function<void(Shape&)>& fn, const QString& text, int field);
    void editJoint(const std::function<void(Frame&, Joint&)>& fn, const QString& text, int field);
    void setStyleFill(const QColor& c);
    void setStyleStroke(const QColor& c);
    // frames
    void frameNewCopy();
    void frameNewBlank();
    void frameDuplicateN();
    void frameDelete();
    void frameMove(int dir);
    void frameTween();
    void setFrameDurations(int ms, bool all);
    void togglePlay();
    bool requireKeyframe();     // false (and a message) if the current frame is a dynamic tween
    void updateCanvasFrame();   // what the canvas shows for the current frame (+ onion skin)
    void tweenBake();
    void editTween(const std::function<void(Frame&)>& fn, const QString& text, int field);
    void playStep();

    Document m_doc, m_committed;
    int m_cur = 0, m_committedFrame = 0;
    QUndoStack* m_undo;
    QString m_path;
    Canvas* m_canvas;

    // timeline
    QListWidget* m_timeline = nullptr;
    QSpinBox* m_dur = nullptr;
    QCheckBox* m_loop = nullptr;
    QLabel* m_timeInfo = nullptr;
    bool m_updatingTimeline = false;
    struct Thumb {
        QVector<Shape> ref;  // keeps the data alive so the key pointer stays unique
        QIcon icon;
    };
    QHash<const void*, Thumb> m_thumbs;
    QTimer m_thumbTimer, m_playTimer;
    bool m_playing = false;
    QVector<Frame> m_playSeq;   // expanded frames while playing
    QVector<int> m_playMap;     // m_playSeq index -> frame index in the strip
    int m_playPos = 0;
    struct TweenThumb {
        QVector<Shape> a, b;    // keep keyframe data alive so the key pointers stay unique
        QIcon icon;
    };
    QHash<QString, TweenThumb> m_tweenThumbs;

    // info panel
    QLabel* m_prompt = nullptr;
    QPlainTextEdit* m_log = nullptr;
    PaletteWidget* m_palette = nullptr;

    // properties
    bool m_updatingProps = false;
    int m_selEpoch = 0;
    QLabel* m_hint = nullptr;
    QGroupBox* m_shapeBox = nullptr;
    QFormLayout* m_form = nullptr;
    QLabel *m_typeLabel = nullptr, *m_groupLabel = nullptr, *m_endsLabel = nullptr;
    QWidget* m_endsRow = nullptr;
    QLineEdit* m_name = nullptr;
    QDoubleSpinBox *m_x = nullptr, *m_y = nullptr, *m_w = nullptr, *m_h = nullptr, *m_rx = nullptr, *m_ry = nullptr;
    QDoubleSpinBox *m_rot = nullptr, *m_sq = nullptr, *m_sqa = nullptr;
    QDoubleSpinBox *m_sw = nullptr, *m_op = nullptr, *m_cr = nullptr, *m_inner = nullptr, *m_fs = nullptr, *m_amp = nullptr;
    QCheckBox *m_lock = nullptr, *m_bold = nullptr, *m_italic = nullptr;
    QPushButton* m_resetWarp = nullptr;
    QSpinBox* m_sides = nullptr;
    QComboBox *m_lineStyle = nullptr, *m_align = nullptr, *m_pathCurve = nullptr;
    QCheckBox* m_pathClosed = nullptr;
    QComboBox* m_bubble = nullptr;
    ColorButton* m_bubbleFill = nullptr;
    ColorButton *m_fill = nullptr, *m_stroke = nullptr, *m_accent = nullptr;
    QLineEdit* m_text = nullptr;
    QFontComboBox* m_font = nullptr;
    QGroupBox* m_jointBox = nullptr;
    QGroupBox* m_structBox = nullptr;
    class QTreeWidget* m_tree = nullptr;
    QGroupBox* m_tweenBox = nullptr;
    QSpinBox *m_twCount = nullptr, *m_twSquash = nullptr;
    QComboBox *m_twEase = nullptr, *m_twPath = nullptr;
    QSpinBox* m_twArc = nullptr;
    QLabel* m_twInfo = nullptr;
    QComboBox* m_jType = nullptr;
    QCheckBox* m_jLim = nullptr;
    QDoubleSpinBox *m_jMin = nullptr, *m_jMax = nullptr, *m_jStiff = nullptr;
    QLabel* m_jInfo = nullptr;
    QFormLayout* m_jForm = nullptr;

    // toolbar style widgets
    ColorButton *m_styleFill = nullptr, *m_styleStroke = nullptr;
    QDoubleSpinBox* m_styleWidth = nullptr;
    QComboBox* m_jointCombo = nullptr;
    QLabel *m_zoomLabel = nullptr, *m_posLabel = nullptr;
    QMenu* m_recentMenu = nullptr;

    // chapters / navigation
    QComboBox* m_chapterCombo = nullptr;
    QSpinBox* m_goto = nullptr;
    QAction *m_actChNew = nullptr, *m_actChDup = nullptr, *m_actChRename = nullptr, *m_actChDel = nullptr;
    QAction *m_actChLeft = nullptr, *m_actChRight = nullptr, *m_actChImport = nullptr, *m_actChExport = nullptr;
    QAction *m_actChPrev = nullptr, *m_actChNext = nullptr;
    // library
    QListWidget* m_library = nullptr;
    QAction *m_actLibAdd = nullptr, *m_actLibInsert = nullptr;

    QVector<QAction*> m_toolActs;
    QAction *m_actUndo, *m_actRedo, *m_actPlay;
    QAction *m_actFrameNew, *m_actFrameBlank, *m_actFrameDup, *m_actFrameDel;
    QAction *m_actFrameLeft, *m_actFrameRight, *m_actTween, *m_actPrev, *m_actNext;
    QAction *m_actNew, *m_actOpen, *m_actSave, *m_actSaveAs, *m_actImage, *m_actExportFrame, *m_actExportAnim, *m_actDocSettings, *m_actQuit;
    QAction *m_actCut, *m_actCopy, *m_actPaste, *m_actDup, *m_actDel, *m_actSelAll, *m_actSelConnected, *m_actDisconnect;
    QAction *m_actGroup, *m_actUngroup;
    QAction *m_actFwd, *m_actBack, *m_actFront, *m_actBottom;
    QAction* m_actMotion = nullptr;
    QAction *m_actZoomIn, *m_actZoomOut, *m_actFit, *m_actGrid, *m_actSnap, *m_actSnapPts, *m_actOrtho, *m_actSticky, *m_actOnion, *m_actJoints;

    bool m_shownOnce = false;
    QVector<Shape> m_clipShapes;
    QVector<Joint> m_clipJoints;
};
