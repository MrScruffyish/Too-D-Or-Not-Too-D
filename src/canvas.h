// The editing view. Scene items are a cache of the model: Canvas::sync()
// creates/updates/deletes ShapeItems by shape id. All edits go to the model
// (Document) and then sync() is called; the items never own geometry.
#pragma once

#include "model.h"

#include <QGraphicsItem>
#include <QGraphicsView>
#include <QHash>
#include <QPixmap>
#include <QTimer>

class Canvas;
class QGraphicsScene;

class ShapeItem : public QGraphicsItem {
public:
    enum { Type = UserType + 1 };
    ShapeItem(Canvas* canvas, const ::Shape& s);
    void setShape(const ::Shape& s, bool force = false);
    const ::Shape& shapeData() const { return m_s; }
    int id() const { return m_s.id; }

    int type() const override { return Type; }
    QRectF boundingRect() const override { return m_bounds; }
    QPainterPath shape() const override { return m_hit; }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override;

private:
    Canvas* m_canvas;
    ::Shape m_s;
    QRectF m_bounds;
    QPainterPath m_hit;
};

class Canvas : public QGraphicsView {
    Q_OBJECT
public:
    enum Tool {
        ToolSelect, ToolWarp,
        ToolRect, ToolEllipse, ToolTriangle, ToolPolygon, ToolStar, ToolDiamond, ToolHourglass, ToolEye,
        ToolLine, ToolArrow, ToolText, ToolPoint,
        ToolConnect, ToolMidpoint, ToolPerpendicular, ToolParallel, ToolMeasure,
        ToolCount
    };
    struct ToolInfo {
        const char* name;
        const char* key;
        const char* help;  // shown in the info panel
    };
    static const ToolInfo& toolInfo(int t);

    explicit Canvas(QWidget* parent = nullptr);

    void setDocument(Document* d);
    void setFrameIndex(int i);
    int frameIndex() const { return m_frame; }
    void sync();
    void resetScene();          // after canvas size/background changes
    void invalidateBackground() { resetCachedContent(); viewport()->update(); }

    QVector<int> selectedIds() const;  // in z order
    void setSelectedIds(const QVector<int>& ids);
    int selectedJoint() const { return m_selJoint; }
    void setSelectedJoint(int id);

    void setTool(Tool t);
    Tool tool() const { return m_tool; }
    void setReadOnly(bool ro);
    void zoomBy(double f);
    void fitToCanvas();
    const QPixmap* pixmap(const QString& key);

    bool isSpringing() const { return m_springTimer.isActive(); }
    void finishSpring(bool emitEdited);  // settle an elastic springback immediately

    // Options
    ::Shape style;  // template for new shapes
    JointType newJointType = JointType::Hinge;
    bool snap = false;          // grid
    bool snapPoints = true;     // corners / midpoints / centres of shapes
    bool ortho = false;         // lines horizontal / vertical only
    bool stickyEnds = true;     // line ends glue to the shape they are drawn on
    bool showGrid = false;
    double gridSize = 20;
    bool onion = false;
    bool showJoints = true;
    void setShowGrid(bool on) { showGrid = on; invalidateBackground(); }
    void setOnion(bool on) { onion = on; invalidateBackground(); }

signals:
    void edited(const QString& what);    // an interaction finished; commit to undo
    void liveChanged();                  // geometry changing during a drag
    void selectionChanged();
    void toolChanged(int tool);
    void textEditRequested(int shapeId);
    void imageDropped(const QString& path, QPointF scenePos);
    void libraryDropped(const QString& itemPath, QPointF scenePos);
    void message(const QString& text);   // log line in the info panel
    void prompt(const QString& text);    // "what to do next" in the info panel
    void zoomChanged(double scale);
    void cursorMoved(QPointF scenePos);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void drawBackground(QPainter* p, const QRectF& rect) override;
    void drawForeground(QPainter* p, const QRectF& rect) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    enum Drag { DNone, DMove, DResize, DLineEnd, DRotate, DWarp, DCreate, DCreateText, DConnect, DJoint, DPan, DRubber };

    Frame& frame() { return m_doc->frames[m_frame]; }
    const Frame& cframe() const { return m_doc->frames.at(m_frame); }
    double viewScale() const { return transform().m11(); }
    QPointF snapPoint(QPointF p) const;
    // grid snap, or the nearest feature point of a shape within a few pixels
    QPointF pickPoint(QPointF sp, int excludeId = -1, int* shapeId = nullptr, bool* onFeature = nullptr,
                      SnapKind* kind = nullptr) const;
    QPointF constrainLine(QPointF from, QPointF to, bool step15) const;
    bool edgeDirectionAt(QPointF sp, QPointF* dir) const;
    bool toolPicksPoints() const;
    ShapeItem* topItemAt(QPointF sp, int excludeId = -1) const;
    QVector<ShapeItem*> shapeItemsAt(QPointF sp) const;
    int hitHandle(QPointF sp) const;   // 0-7 resize, 8 rotate, 9/10 line ends, 11-14 warp corners
    int hitJoint(QPointF sp) const;
    void doResize(QPointF sp, bool keepAspect);
    void finishConnect(int a, QPointF pa, int b, QPointF pb);
    void glueEnd(::Shape& line, int end, QPointF world);
    void constructionClick(QPointF sp);
    void resetConstruction();
    QString promptText() const;
    void updateHoverCursor(QPointF sp);
    void refreshHover();  // recompute the snap marker at the cursor (e.g. when Shift changes)
    void expandToGroups();
    void startSpring(const QString& editText, const QVector<int>& pinned);
    void springTick();

    QGraphicsScene* m_scene;
    Document* m_doc = nullptr;
    int m_frame = 0;
    QHash<int, ShapeItem*> m_items;
    QHash<QString, QPixmap> m_pix;
    bool m_syncing = false;
    bool m_readOnly = false;
    bool m_spaceDown = false;
    Tool m_tool = ToolSelect;
    int m_selJoint = -1;

    Drag m_drag = DNone;
    int m_dragId = -1;
    int m_handle = -1;
    bool m_moved = false;
    QPointF m_pressScene, m_lastScene, m_grabLocal, m_pivot, m_createStart;
    QPointF m_pressPos, m_panLast;
    double m_rotAccum = 0;
    ::Shape m_orig;
    int m_createLinkTarget = 0;
    QPointF m_createLinkPoint;

    // hover feedback for point-picking tools
    bool m_hoverValid = false, m_hoverFeature = false;
    SnapKind m_hoverKind = SnapKind::Free;
    QPointF m_hover;

    // connect tool: first picked point (click, then click the second)
    int m_connectFrom = 0;
    QPointF m_connectPoint;

    // construction tools (midpoint, perpendicular, parallel, measure)
    QVector<QPointF> m_steps;
    bool m_haveRef = false;
    QPointF m_refDir;

    // elastic springback
    QTimer m_springTimer;
    QVector<int> m_springPinned;
    QHash<int, QPointF> m_springVel;
    QString m_springEdit;
    int m_springTicks = 0;
};
