#include "canvas.h"

#include "solver.h"

#include <QDragEnterEvent>
#include <QGraphicsScene>
#include <QGuiApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QWheelEvent>
#include <QtMath>
#include <cmath>
#include <functional>

namespace {
const int HX[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
const int HY[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
const double kRotHandlePx = 24;

const Canvas::ToolInfo kTools[] = {
    {"Select", "V", "Select: click a shape (selects its whole group; Alt+click picks one member). Shift/Ctrl+click adds/removes. "
                    "Drag empty space to box-select. Square handles resize (Shift or 'Lock aspect' keeps proportions), the round handle rotates."},
    {"Warp", "W", "Warp: select a shape, then drag its orange corner handles to skew, taper or distort it. Reset in Properties."},
    {"Rectangle", "R", "Rectangle: drag to draw (Shift = square), or click for a default size."},
    {"Ellipse", "E", "Ellipse: drag to draw (Shift = circle). Radius X/Y and 'Circle' lock are in Properties."},
    {"Triangle", "T", "Triangle: drag to draw."},
    {"Polygon", "P", "Polygon: drag to draw. Number of sides in Properties."},
    {"Star", "S", "Star: drag to draw. Points and inner ratio in Properties."},
    {"Diamond", "D", "Diamond: regular polygon with every pair of corners connected. Number of corners in Properties."},
    {"Hourglass", "H", "Hourglass: drag to draw. 'Inner ratio' sets the waist width."},
    {"Eye", "Y", "Eye: almond shape with an iris. 'Inner ratio' sets the iris size (0 = none)."},
    {"Line", "L", "Line: drag from start to end. Ctrl = 15° steps, Ortho = horizontal/vertical, hold Shift = no snapping. Ends snap to shape points and, with "
                  "'Sticky ends', glue to the shape so the line stretches when it moves."},
    {"Arrow", "A", "Arrow: like Line, with a head at the end."},
    {"Freehand", "F", "Freehand: drag to draw a free shape (smoothed). End near the start to close it. The tool stays active for more strokes."},
    {"Polyline", "B", "Polyline: click points joined by straight lines. Double-click or Enter finishes, click the first point to close, "
                      "Backspace removes the last point, Esc cancels. Points snap (Shift = free, Ctrl = 15° steps)."},
    {"Curve", "Z", "Curve: click points; a smooth curve passes through them. Double-click or Enter finishes, click the first point "
                   "to close, Backspace removes the last point, Esc cancels. Drag the points of a selected path to reshape it."},
    {"Text", "X", "Text: click to place, then type. Font, size, bold, italic and alignment are in Properties."},
    {"Speech bubble", "G", "Speech bubble: click to place a text in a bubble, then type it. Drag the yellow handle to point the tail at "
                           "the speaker. Style (speech, thought, shout, box) and colours in Properties."},
    {"Point", "O", "Point: click to place a helper point (snaps to shape points). Helper points show while editing but are never exported."},
    {"Connect", "C", "Connect: click a point on the first shape, then a point on the second shape; the second shape moves so the "
                     "two points meet. Corners, midpoints, centres and edges snap (markers show them). "
                     "Click the same spot twice to join overlapping shapes. Hold Shift for a free (unsnapped) point. Type: 'Connect as'."},
    {"Pin", "I", "Pin: click where shapes overlap to connect them at exactly that point (nothing moves). 'Pin joins' in the toolbar: "
                 "top two shapes, or all shapes under the spot (Ctrl+click = the other one). Type from 'Connect as'; the lower shape "
                 "becomes the parent. Snaps like the other tools."},
    {"Midpoint", "M", "Midpoint: click two points; a helper point is placed half-way."},
    {"Perpendicular", "U", "Perpendicular: click a line or a shape edge, then a start point, then the end point."},
    {"Parallel", "J", "Parallel: click a line or a shape edge, then a start point, then the end point."},
    {"Measure", "Q", "Measure: click two points to show distance and angle."},
};
static_assert(sizeof(kTools) / sizeof(kTools[0]) == Canvas::ToolCount, "tool table");

double dist(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

QPointF closestOnSeg(QPointF p, QPointF a, QPointF b)
{
    const QPointF ab = b - a;
    const double l2 = QPointF::dotProduct(ab, ab);
    const double t = l2 > 0 ? qBound(0.0, QPointF::dotProduct(p - a, ab) / l2, 1.0) : 0.0;
    return a + ab * t;
}

// Outline polygons of a shape in world coordinates (used for edge snapping / reference edges).
QList<QPolygonF> worldOutlines(const ::Shape& s)
{
    QList<QPolygonF> polys;
    if (s.isLinear()) polys << (QPolygonF() << QPointF(-s.w / 2, 0) << QPointF(s.w / 2, 0));
    else if (s.type == ShapeType::Text || s.type == ShapeType::Image) polys << QPolygonF(s.localRect());
    else polys = shapePath(s).toSubpathPolygons();
    const QTransform t = s.transform();
    for (QPolygonF& p : polys) p = t.map(p);
    return polys;
}

double segDist(QPointF p, QPointF a, QPointF b)
{
    const QPointF ab = b - a;
    const double l2 = QPointF::dotProduct(ab, ab);
    const double t = l2 > 0 ? qBound(0.0, QPointF::dotProduct(p - a, ab) / l2, 1.0) : 0.0;
    return dist(p, a + ab * t);
}

QPointF snapAngle(QPointF from, QPointF to, double stepDeg)
{
    const QPointF d = to - from;
    const double l = std::hypot(d.x(), d.y());
    const double st = qDegreesToRadians(stepDeg);
    const double a = std::round(std::atan2(d.y(), d.x()) / st) * st;
    return from + QPointF(std::cos(a) * l, std::sin(a) * l);
}

ShapeType toolShape(Canvas::Tool t)
{
    switch (t) {
    case Canvas::ToolEllipse: return ShapeType::Ellipse;
    case Canvas::ToolTriangle: return ShapeType::Triangle;
    case Canvas::ToolPolygon: return ShapeType::Polygon;
    case Canvas::ToolStar: return ShapeType::Star;
    case Canvas::ToolDiamond: return ShapeType::Diamond;
    case Canvas::ToolHourglass: return ShapeType::Hourglass;
    case Canvas::ToolEye: return ShapeType::Eye;
    case Canvas::ToolLine: return ShapeType::Line;
    case Canvas::ToolArrow: return ShapeType::Arrow;
    case Canvas::ToolText: return ShapeType::Text;
    case Canvas::ToolPoint: return ShapeType::Point;
    default: return ShapeType::Rect;
    }
}

const QPixmap& checker()
{
    static QPixmap pm = [] {
        QPixmap p(16, 16);
        p.fill(Qt::white);
        QPainter g(&p);
        g.fillRect(0, 0, 8, 8, QColor(204, 204, 204));
        g.fillRect(8, 8, 8, 8, QColor(204, 204, 204));
        return p;
    }();
    return pm;
}

QColor jointColor(JointType t)
{
    switch (t) {
    case JointType::Hinge: return QColor(255, 140, 0);
    case JointType::Free: return QColor(40, 190, 90);
    case JointType::Weld: return QColor(220, 50, 50);
    default: return QColor(170, 80, 230);
    }
}
}  // namespace

const Canvas::ToolInfo& Canvas::toolInfo(int t) { return kTools[qBound(0, t, int(ToolCount) - 1)]; }

// ================================================================ ShapeItem

ShapeItem::ShapeItem(Canvas* c, const ::Shape& s) : m_canvas(c)
{
    setFlag(ItemIsSelectable);
    setShape(s, true);
}

void ShapeItem::setShape(const ::Shape& s, bool force)
{
    if (!force && s == m_s) return;
    prepareGeometryChange();
    m_s = s;
    setPos(s.pos);
    setTransform(s.localTransform());
    setOpacity(s.type == ShapeType::Point ? 1.0 : s.opacity);
    setCacheMode(s.type == ShapeType::Image ? DeviceCoordinateCache : NoCache);

    const double hw = s.strokeWidth / 2 + 1;
    switch (s.type) {
    case ShapeType::Line:
    case ShapeType::Arrow: {
        QPainterPathStroker st;
        st.setWidth(qMax(10.0, s.strokeWidth + 6 + (s.lineStyle == 1 ? 2 * s.amplitude : 0)));
        m_hit = st.createStroke(shapePath(s));
        const double ah = s.type == ShapeType::Arrow ? arrowHeadLength(s) * 0.6 : 0;
        m_bounds = m_hit.boundingRect().united(QRectF(-s.w / 2, -ah, s.w, 2 * ah)).adjusted(-hw, -hw, hw, hw);
        break;
    }
    case ShapeType::Path:
        if (s.closed) {
            m_hit = shapePath(s);
        } else {
            QPainterPathStroker st;
            st.setWidth(qMax(10.0, s.strokeWidth + 6));
            m_hit = st.createStroke(shapePath(s));
        }
        m_bounds = m_hit.boundingRect().united(shapePath(s).boundingRect()).adjusted(-hw, -hw, hw, hw);
        break;
    case ShapeType::Point: {
        const double r = qMax(6.0, s.w / 2);
        m_hit = QPainterPath();
        m_hit.addEllipse(QPointF(0, 0), r, r);
        m_bounds = m_hit.boundingRect().adjusted(-2, -2, 2, 2);
        break;
    }
    case ShapeType::Text:
    case ShapeType::Image:
        m_hit = QPainterPath();
        m_hit.addRect(s.localRect());
        if (s.type == ShapeType::Text && s.bubble) m_hit = m_hit.united(bubblePath(s));
        m_bounds = m_hit.boundingRect().adjusted(-hw, -hw, hw, hw);
        break;
    default:
        m_hit = shapePath(s);
        m_bounds = m_hit.boundingRect().adjusted(-hw, -hw, hw, hw);
    }
    update();
}

void ShapeItem::paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*)
{
    if (m_s.type == ShapeType::Image) {
        const QRectF r = m_s.localRect();
        const QPixmap* pm = m_canvas->pixmap(m_s.imageKey);
        if (pm && !pm->isNull())
            p->drawPixmap(r, *pm, QRectF(pm->rect()));
        else
            p->fillRect(r, QColor(210, 210, 210));
        if (m_s.strokeWidth > 0 && m_s.stroke.alpha() > 0) {
            p->setPen(QPen(m_s.stroke, m_s.strokeWidth));
            p->setBrush(Qt::NoBrush);
            p->drawRect(r);
        }
        return;
    }
    paintShapeLocal(p, m_s, nullptr);
}

// ================================================================ Canvas

Canvas::Canvas(QWidget* parent) : QGraphicsView(parent), m_scene(new QGraphicsScene(this))
{
    setScene(m_scene);
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    setViewportUpdateMode(FullViewportUpdate);
    setCacheMode(CacheBackground);
    setDragMode(RubberBandDrag);
    setRubberBandSelectionMode(Qt::IntersectsItemShape);
    setTransformationAnchor(AnchorUnderMouse);
    setResizeAnchor(AnchorViewCenter);
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    connect(m_scene, &QGraphicsScene::selectionChanged, this, [this] {
        if (m_syncing) return;
        if (!m_scene->selectedItems().isEmpty()) m_selJoint = -1;
        emit selectionChanged();
        viewport()->update();
    });
    m_springTimer.setInterval(16);
    connect(&m_springTimer, &QTimer::timeout, this, &Canvas::springTick);
}

// Delete the scene while the canvas is still intact and silent: removing items
// clears the selection, and those notifications must not reach anyone during teardown.
Canvas::~Canvas()
{
    m_springTimer.stop();
    blockSignals(true);
    m_scene->blockSignals(true);
    m_scene->disconnect(this);
    m_items.clear();
    delete m_scene;
    m_scene = nullptr;
}

void Canvas::setDocument(Document* d)
{
    m_doc = d;
    m_frame = 0;
    resetScene();
    sync();
}

void Canvas::resetScene()
{
    if (!m_doc) return;
    const double w = m_doc->width, h = m_doc->height, m = qMax(1000.0, qMax(w, h));
    m_scene->setSceneRect(-m, -m, w + 2 * m, h + 2 * m);
    invalidateBackground();
}

void Canvas::setDisplayFrame(const Frame* f)
{
    m_hasDisplay = f != nullptr;
    m_display = f ? *f : Frame();
    sync();
}

void Canvas::setOnionFrame(const Frame* f)
{
    m_hasOnion = f != nullptr;
    m_onionFrame = f ? *f : Frame();
    if (onion) invalidateBackground();
}

void Canvas::setFrameIndex(int i)
{
    m_frame = qBound(0, i, m_doc->frames.size() - 1);
    resetConstruction();
    invalidateBackground();
    sync();
}

void Canvas::sync()
{
    if (!m_doc) return;
    m_frame = qBound(0, m_frame, m_doc->frames.size() - 1);
    const QVector<int> before = selectedIds();
    m_syncing = true;
    const Frame& f = cframe();
    QSet<int> alive;
    alive.reserve(f.shapes.size());
    for (int i = 0; i < f.shapes.size(); ++i) {
        const ::Shape& s = f.shapes.at(i);
        alive.insert(s.id);
        ShapeItem*& it = m_items[s.id];
        if (!it) {
            it = new ShapeItem(this, s);
            m_scene->addItem(it);
        } else {
            it->setShape(s);
        }
        const double z = s.type == ShapeType::Point ? 1e6 + i : i;  // helper points always on top
        if (it->zValue() != z) it->setZValue(z);
    }
    for (auto it = m_items.begin(); it != m_items.end();) {
        if (!alive.contains(it.key())) {
            delete it.value();
            it = m_items.erase(it);
        } else {
            ++it;
        }
    }
    if (m_selJoint >= 0 && f.jointIndex(m_selJoint) < 0) m_selJoint = -1;
    m_syncing = false;
    if (selectedIds() != before) emit selectionChanged();
    viewport()->update();
}

QVector<int> Canvas::selectedIds() const
{
    QSet<int> sel;
    for (QGraphicsItem* gi : m_scene->selectedItems())
        if (auto* si = qgraphicsitem_cast<ShapeItem*>(gi)) sel.insert(si->id());
    QVector<int> out;
    if (sel.isEmpty() || !m_doc) return out;
    for (const ::Shape& s : cframe().shapes)
        if (sel.contains(s.id)) out << s.id;
    return out;
}

void Canvas::setSelectedIds(const QVector<int>& ids)
{
    m_syncing = true;
    m_scene->clearSelection();
    for (int id : ids)
        if (ShapeItem* it = m_items.value(id)) it->setSelected(true);
    m_syncing = false;
    if (!ids.isEmpty()) m_selJoint = -1;
    emit selectionChanged();
    viewport()->update();
}

void Canvas::setSelectedJoint(int id)
{
    m_syncing = true;
    m_scene->clearSelection();
    m_syncing = false;
    m_selJoint = id;
    emit selectionChanged();
    viewport()->update();
}

void Canvas::expandToGroups()
{
    QSet<int> groups;
    for (int id : selectedIds())
        if (const ShapeItem* it = m_items.value(id))
            if (it->shapeData().group) groups.insert(it->shapeData().group);
    if (groups.isEmpty()) return;
    m_syncing = true;
    for (ShapeItem* it : std::as_const(m_items))
        if (groups.contains(it->shapeData().group)) it->setSelected(true);
    m_syncing = false;
    emit selectionChanged();
}

void Canvas::setTool(Tool t)
{
    finishSpring(true);
    m_tool = t;
    resetConstruction();
    viewport()->setCursor(t == ToolSelect || t == ToolWarp ? Qt::ArrowCursor : Qt::CrossCursor);
    emit toolChanged(t);
    emit prompt(promptText());
    viewport()->update();
}

void Canvas::resetConstruction()
{
    m_steps.clear();
    m_haveRef = false;
    m_connectFrom = 0;
    m_pathPts.clear();
}

QString Canvas::promptText() const
{
    switch (m_tool) {
    case ToolMidpoint:
        return m_steps.isEmpty() ? tr("Midpoint: click the first point.") : tr("Midpoint: click the second point.");
    case ToolMeasure:
        return m_steps.isEmpty() ? tr("Measure: click the first point.") : tr("Measure: click the second point.");
    case ToolPerpendicular:
    case ToolParallel:
        if (!m_haveRef) return tr("%1: click a line or a shape edge to use as reference.").arg(toolInfo(m_tool).name);
        if (m_steps.isEmpty()) return tr("%1: click the start point (Esc cancels).").arg(toolInfo(m_tool).name);
        return tr("%1: click the end point (Esc cancels).").arg(toolInfo(m_tool).name);
    case ToolPolyline:
    case ToolCurve:
        if (m_pathPts.isEmpty()) return tr(toolInfo(m_tool).help);
        return tr("%1: %2 point(s). Click the next point; double-click or Enter finishes; click the first point to close; "
                  "Backspace undoes a point; Esc cancels.").arg(toolInfo(m_tool).name).arg(m_pathPts.size());
    case ToolConnect:
        if (m_connectFrom)
            return tr("Connect: now click the point on the SECOND shape; it moves so the points meet. "
                      "Click the same spot again to join overlapping shapes. Esc cancels.");
        return tr(toolInfo(m_tool).help);
    default: return tr(toolInfo(m_tool).help);
    }
}

void Canvas::setReadOnly(bool ro)
{
    finishSpring(true);
    m_readOnly = ro;
    m_drag = DNone;
    invalidateBackground();
}

void Canvas::zoomBy(double f)
{
    const double cur = viewScale();
    const double n = qBound(0.02, cur * f, 64.0);
    scale(n / cur, n / cur);
    emit zoomChanged(n);
}

void Canvas::fitToCanvas()
{
    if (!m_doc) return;
    fitInView(QRectF(0, 0, m_doc->width, m_doc->height).adjusted(-30, -30, 30, 30), Qt::KeepAspectRatio);
    emit zoomChanged(viewScale());
}

const QPixmap* Canvas::pixmap(const QString& key)
{
    auto it = m_pix.find(key);
    if (it != m_pix.end()) return &it.value();
    auto im = m_doc->images.constFind(key);
    if (im == m_doc->images.constEnd()) return nullptr;
    return &m_pix.insert(key, QPixmap::fromImage(im.value())).value();
}

// ---------------------------------------------------------------- point picking

QPointF Canvas::snapPoint(QPointF p) const
{
    if (!snap || gridSize <= 0) return p;
    return QPointF(std::round(p.x() / gridSize) * gridSize, std::round(p.y() / gridSize) * gridSize);
}

// Priority: exact points (corner / midpoint / centre) within 9 px, then the
// nearest point on an edge within 6 px, then grid, then the free position.
QPointF Canvas::pickPoint(QPointF sp, int excludeId, int* shapeId, bool* onFeature, SnapKind* kind) const
{
    if (shapeId) *shapeId = 0;
    if (onFeature) *onFeature = false;
    if (kind) *kind = SnapKind::Free;
    if (QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier) {  // hold Shift: free point, no snapping
        if (shapeId)
            if (ShapeItem* it = topItemAt(sp, excludeId)) *shapeId = it->id();
        return sp;
    }
    if (snapPoints) {
        const double tol = 9 / viewScale();
        double best = tol;
        QPointF bp;
        int bid = 0;
        SnapKind bk = SnapKind::Free;
        const auto near = m_scene->items(QRectF(sp.x() - tol, sp.y() - tol, 2 * tol, 2 * tol), Qt::IntersectsItemBoundingRect);
        for (QGraphicsItem* gi : near) {
            auto* si = qgraphicsitem_cast<ShapeItem*>(gi);
            if (!si || si->id() == excludeId) continue;
            const ::Shape& s = si->shapeData();
            for (const SnapPoint& f : ::snapPoints(s)) {
                const QPointF w = s.toWorld(f.local);
                const double d = dist(w, sp);
                if (d < best) {
                    best = d;
                    bp = w;
                    bid = s.id;
                    bk = f.kind;
                }
            }
        }
        if (!bid) {
            double be = 6 / viewScale();
            for (QGraphicsItem* gi : near) {
                auto* si = qgraphicsitem_cast<ShapeItem*>(gi);
                if (!si || si->id() == excludeId || si->shapeData().type == ShapeType::Point) continue;
                for (const QPolygonF& wp : worldOutlines(si->shapeData()))
                    for (int i = 0; i + 1 < wp.size(); ++i) {
                        const QPointF q = closestOnSeg(sp, wp[i], wp[i + 1]);
                        const double d = dist(q, sp);
                        if (d < be) {
                            be = d;
                            bp = q;
                            bid = si->id();
                            bk = SnapKind::Edge;
                        }
                    }
            }
        }
        if (bid) {
            if (shapeId) *shapeId = bid;
            if (onFeature) *onFeature = true;
            if (kind) *kind = bk;
            return bp;
        }
    }
    if (shapeId)
        if (ShapeItem* it = topItemAt(sp, excludeId)) *shapeId = it->id();
    if (kind && snap) *kind = SnapKind::Grid;
    return snapPoint(sp);
}

QPointF Canvas::constrainLine(QPointF from, QPointF to, bool step15) const
{
    if (ortho) return snapAngle(from, to, 90);
    if (step15) return snapAngle(from, to, 15);
    return to;
}

bool Canvas::edgeDirectionAt(QPointF sp, QPointF* dir) const
{
    const double tol = 40 / viewScale();
    double best = tol;
    bool found = false;
    const auto near = m_scene->items(QRectF(sp.x() - tol, sp.y() - tol, 2 * tol, 2 * tol), Qt::IntersectsItemBoundingRect);
    for (QGraphicsItem* gi : near) {
        auto* si = qgraphicsitem_cast<ShapeItem*>(gi);
        if (!si || si->shapeData().type == ShapeType::Point) continue;
        for (const QPolygonF& wp : worldOutlines(si->shapeData())) {
            for (int i = 0; i + 1 < wp.size(); ++i) {
                const double d = segDist(sp, wp[i], wp[i + 1]);
                if (d < best && dist(wp[i], wp[i + 1]) > 1e-6) {
                    best = d;
                    *dir = wp[i + 1] - wp[i];
                    found = true;
                }
            }
        }
    }
    return found;
}

bool Canvas::toolPicksPoints() const
{
    switch (m_tool) {
    case ToolConnect: case ToolPin: case ToolLine: case ToolArrow: case ToolPoint: case ToolPolyline: case ToolCurve:
    case ToolMidpoint: case ToolPerpendicular: case ToolParallel: case ToolMeasure: return true;
    default: return false;
    }
}

QVector<ShapeItem*> Canvas::shapeItemsAt(QPointF sp) const
{
    QVector<ShapeItem*> out;
    const auto list = m_scene->items(sp, Qt::IntersectsItemShape, Qt::DescendingOrder, transform());
    for (QGraphicsItem* gi : list)
        if (auto* si = qgraphicsitem_cast<ShapeItem*>(gi)) out << si;
    return out;
}

ShapeItem* Canvas::topItemAt(QPointF sp, int excludeId) const
{
    for (ShapeItem* it : shapeItemsAt(sp))
        if (it->id() != excludeId) return it;
    return nullptr;
}

int Canvas::hitHandle(QPointF sp) const
{
    const QVector<int> ids = selectedIds();
    if (ids.size() != 1) return -1;
    const ShapeItem* it = m_items.value(ids.first());
    if (!it) return -1;
    const ::Shape& s = it->shapeData();
    if (s.type == ShapeType::Point) return -1;
    const double sc = viewScale(), tol = 7 / sc;
    if (s.type == ShapeType::Text && s.bubble && m_tool == ToolSelect &&
        dist(sp, s.toWorld(QPointF(s.tail.x() * s.w, s.tail.y() * s.h))) < tol)
        return 99;  // bubble tail
    if (s.type == ShapeType::Path && m_tool == ToolSelect && s.pts.size() <= 300) {  // path points
        const QVector<QPointF> lp = pathPointsLocal(s);
        for (int i = 0; i < lp.size(); ++i)
            if (dist(sp, s.toWorld(lp[i])) < tol) return 100 + i;
    }
    if (s.isLinear()) {
        if (dist(sp, s.toWorld({-s.w / 2, 0})) < tol) return 9;
        if (dist(sp, s.toWorld({s.w / 2, 0})) < tol) return 10;
        return -1;
    }
    if (m_tool == ToolWarp) {
        for (int k = 0; k < 4; ++k)
            if (dist(sp, s.toWorld(s.cornerBase(k))) < tol) return 11 + k;
        return -1;
    }
    if (dist(sp, s.toWorld({0, -s.h / 2 - kRotHandlePx / sc})) < tol) return 8;
    for (int k = 0; k < 8; ++k)
        if (dist(sp, s.toWorld({HX[k] * s.w / 2, HY[k] * s.h / 2})) < tol) return k;
    return -1;
}

int Canvas::hitJoint(QPointF sp) const
{
    const double tol = 8 / viewScale();
    for (const Joint& j : cframe().joints) {
        const ShapeItem* p = m_items.value(j.parent);
        if (p && dist(sp, Solver::anchorWorld(p->shapeData(), j.anchorP)) < tol) return j.id;
    }
    return -1;
}

QString Canvas::dragHint(int id) const
{
    const Frame& f = cframe();
    const ::Shape* s = f.find(id);
    if (!s) return {};
    auto name = [&f](int sid) {
        const ::Shape* x = f.find(sid);
        if (!x) return QStringLiteral("?");
        return x->name.isEmpty() ? QStringLiteral("%1 #%2").arg(shapeTypeName(x->type)).arg(sid) : x->name;
    };
    const int n = Solver::component(f, id).size();
    if (n < 2) return tr("%1: not connected. Drag to move it.").arg(name(id));
    const Solver::DragInfo di = Solver::dragInfo(f, id);
    if (di.joint < 0)
        return di.root == id ? tr("%1 is the ROOT (★) of an assembly of %2 shapes: dragging it moves everything.").arg(name(id)).arg(n)
                             : tr("%1 is welded to the root %2: dragging moves the whole assembly (%3 shapes).").arg(name(id), name(di.root)).arg(n);
    const Joint& j = f.joints.at(di.joint);
    const QString who = di.root == id ? name(id) : tr("%1 (welded to %2)").arg(name(id), name(di.root));
    switch (j.type) {
    case JointType::Hinge:
        return tr("%1 hangs on a hinge from %2: dragging ROTATES it around the pin. To move everything drag the root (★), "
                  "or press Ctrl+L first.").arg(who, name(j.parent));
    case JointType::Free:
        return tr("%1 has a free pin to %2: dragging moves it, and %2 swings along.").arg(who, name(j.parent));
    case JointType::Elastic:
        return tr("%1 is elastic to %2: dragging stretches the band, it springs back when released.").arg(who, name(j.parent));
    default: return {};
    }
}

void Canvas::updateHoverCursor(QPointF sp)
{
    if (m_spaceDown) return;
    if (m_tool == ToolSelect && !m_readOnly) {  // tell what a drag would do
        const ShapeItem* it = topItemAt(sp);
        const int id = it ? it->id() : 0;
        if (id != m_hoverShape) {
            m_hoverShape = id;
            emit prompt(id ? dragHint(id) : promptText());
        }
    }
    const bool sel = m_tool == ToolSelect || m_tool == ToolWarp;
    Qt::CursorShape c = sel ? Qt::ArrowCursor : Qt::CrossCursor;
    if (sel && !m_readOnly) {
        const int h = hitHandle(sp);
        if (h == 8) c = Qt::PointingHandCursor;
        else if (h >= 0) c = Qt::SizeAllCursor;
        else if (showJoints && hitJoint(sp) >= 0) c = Qt::PointingHandCursor;
    }
    viewport()->setCursor(c);
}

// ---------------------------------------------------------------- elastic springback

void Canvas::startSpring(const QString& text, const QVector<int>& pinned)
{
    if (!Solver::hasElasticGap(cframe())) {
        emit edited(text);
        return;
    }
    m_springEdit = text;
    m_springPinned = pinned;
    m_springVel.clear();
    m_springTicks = 0;
    m_springTimer.start();
}

void Canvas::springTick()
{
    const bool moving = Solver::springStep(frame(), m_springPinned, m_springVel, 1.0 / 60.0);
    sync();
    emit liveChanged();
    if (!moving || ++m_springTicks > 400) finishSpring(true);
}

void Canvas::finishSpring(bool emitEdited)
{
    if (!m_springTimer.isActive()) return;
    m_springTimer.stop();
    QHash<int, double> seeds;
    for (int id : std::as_const(m_springPinned))
        if (cframe().indexOf(id) >= 0) seeds.insert(id, 0.0);
    if (!seeds.isEmpty()) Solver::propagate(frame(), seeds);  // close any remaining gap exactly
    sync();
    if (emitEdited) emit edited(m_springEdit);
}

// ---------------------------------------------------------------- mouse

void Canvas::mousePressEvent(QMouseEvent* e)
{
    const QPointF sp = mapToScene(e->position().toPoint());
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && m_spaceDown)) {
        m_drag = DPan;
        m_panLast = e->position();
        viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() != Qt::LeftButton || m_readOnly || !m_doc) return;
    finishSpring(true);
    setFocus();
    m_pressScene = m_lastScene = sp;
    m_pressPos = e->position();
    m_moved = false;
    m_rotAccum = 0;

    if (m_tool == ToolSelect || m_tool == ToolWarp) {
        const int h = hitHandle(sp);
        if (h >= 0) {
            m_dragId = selectedIds().constFirst();
            m_handle = h;
            if (h == 99) {
                m_drag = DTail;
                m_orig = *cframe().find(m_dragId);
                return;
            }
            if (h >= 100) {
                m_drag = DNode;
                m_handle = h - 100;
                m_orig = *cframe().find(m_dragId);
                return;
            }
            if (h == 8) {
                m_drag = DRotate;
                const Solver::DragInfo di = Solver::dragInfo(cframe(), m_dragId);
                m_orig = m_items.value(m_dragId)->shapeData();
                m_pivot = di.joint >= 0 && cframe().joints.at(di.joint).type != JointType::Elastic ? di.pivot : m_orig.pos;
            } else if (h >= 11) {
                m_drag = DWarp;
                m_handle = h - 11;
            } else if (h >= 9) {
                m_drag = DLineEnd;
                frame().find(m_dragId)->ends[h - 9] = EndLink();  // unglue while dragging
            } else {
                m_drag = DResize;
            }
            m_orig = *cframe().find(m_dragId);
            return;
        }
        if (showJoints) {
            const int jid = hitJoint(sp);
            if (jid >= 0) {
                setSelectedJoint(jid);
                m_drag = DJoint;
                m_dragId = jid;
                return;
            }
        }
        if (ShapeItem* it = topItemAt(sp)) {
            const bool toggle = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
            const bool single = e->modifiers() & Qt::AltModifier;
            QVector<ShapeItem*> members{it};
            const int g = it->shapeData().group;
            if (g && !single)
                for (ShapeItem* o : std::as_const(m_items))
                    if (o != it && o->shapeData().group == g) members << o;
            if (toggle) {
                const bool on = !it->isSelected();
                for (ShapeItem* m : members) m->setSelected(on);
                if (!on) {
                    m_drag = DNone;
                    return;
                }
            } else if (!it->isSelected() || (single && selectedIds().size() > 1 && g)) {
                m_scene->clearSelection();
                for (ShapeItem* m : members) m->setSelected(true);
            }
            m_drag = DMove;
            m_dragId = it->id();
            m_orig = it->shapeData();
            m_grabLocal = m_orig.toLocal(sp);
            return;
        }
        if (m_selJoint >= 0) {
            m_selJoint = -1;
            emit selectionChanged();
        }
        m_drag = DRubber;
        QGraphicsView::mousePressEvent(e);
        return;
    }

    switch (m_tool) {
    case ToolConnect: {
        int id = 0;
        SnapKind k = SnapKind::Free;
        if (!m_connectFrom) {
            const QPointF p = pickPoint(sp, -1, &id, nullptr, &k);
            if (!id) {
                emit message(tr("Connect: click on (or at a snap point of) the first shape."));
                return;
            }
            m_connectFrom = id;
            m_connectPoint = p;
            if (const ShapeItem* it = m_items.value(id))
                emit message(tr("First point: %1 of %2").arg(k == SnapKind::Free ? tr("free point") : snapKindName(k),
                                                             shapeTypeName(it->shapeData().type)));
        } else {
            const QPointF p = pickPoint(sp, m_connectFrom, &id, nullptr, &k);
            if (!id) {
                emit message(tr("Connect: click on a different shape for the second point (Esc cancels)."));
                return;
            }
            const int a = m_connectFrom;
            const QPointF pa = m_connectPoint;
            m_connectFrom = 0;
            finishConnect(a, pa, id, p);
        }
        emit prompt(promptText());
        viewport()->update();
        return;
    }
    case ToolPin: pinAt(sp, e->modifiers() & Qt::ControlModifier); return;
    case ToolFreehand:
        m_drawPts = {sp};
        m_drag = DFreehand;
        return;
    case ToolPolyline:
    case ToolCurve: pathClick(sp, e->modifiers()); return;
    case ToolText:
    case ToolBubble: m_drag = DCreateText; return;
    case ToolMidpoint:
    case ToolPerpendicular:
    case ToolParallel:
    case ToolMeasure: constructionClick(sp); return;
    case ToolPoint: {
        ::Shape s;
        s.id = m_doc->newId();
        s.type = ShapeType::Point;
        s.w = s.h = 10;
        s.pos = pickPoint(sp);
        frame().shapes.append(s);
        sync();
        emit message(tr("Point at (%1, %2)").arg(s.pos.x(), 0, 'f', 1).arg(s.pos.y(), 0, 'f', 1));
        emit edited(tr("Add point"));
        return;
    }
    default: break;
    }

    ::Shape s = style;
    s.id = m_doc->newId();
    s.type = toolShape(m_tool);
    s.name.clear();
    s.text.clear();
    s.imageKey.clear();
    s.group = 0;
    s.rot = 0;
    s.lockAspect = false;
    s.squash = 1;
    for (QPointF& w : s.warp) w = QPointF();
    for (EndLink& l : s.ends) l = EndLink();
    if (s.type == ShapeType::Hourglass) s.innerRatio = 0.12;
    if (s.type == ShapeType::Eye) {
        s.innerRatio = 0.5;
        s.fill = Qt::white;
    }
    if (s.type == ShapeType::Diamond) s.sides = 8;
    m_createLinkTarget = 0;
    if (s.isLinear()) {
        int tid = 0;
        m_createStart = pickPoint(sp, -1, &tid);
        if (stickyEnds && tid) {
            m_createLinkTarget = tid;
            m_createLinkPoint = m_createStart;
        }
        if (s.strokeWidth <= 0) s.strokeWidth = 3;
        if (s.stroke.alpha() == 0) s.stroke = Qt::black;
    } else {
        m_createStart = snapPoint(sp);
    }
    s.pos = m_createStart;
    s.w = 1;
    s.h = s.isLinear() ? 0 : 1;
    frame().shapes.append(s);
    m_dragId = s.id;
    m_drag = DCreate;
    sync();
    setSelectedIds({s.id});
}

void Canvas::constructionClick(QPointF sp)
{
    auto addLine = [this](QPointF a, QPointF b) {
        ::Shape s = style;
        s.id = m_doc->newId();
        s.type = ShapeType::Line;
        s.name.clear();
        s.group = 0;
        s.squash = 1;
        for (QPointF& w : s.warp) w = QPointF();
        for (EndLink& l : s.ends) l = EndLink();
        if (s.strokeWidth <= 0) s.strokeWidth = 2;
        if (s.stroke.alpha() == 0) s.stroke = Qt::black;
        s.pos = (a + b) / 2;
        s.w = qMax(1.0, dist(a, b));
        s.h = 0;
        s.rot = Solver::angleDeg(b - a);
        frame().shapes.append(s);
        return s.id;
    };
    switch (m_tool) {
    case ToolMidpoint:
    case ToolMeasure: {
        m_steps << pickPoint(sp);
        if (m_steps.size() < 2) break;
        const QPointF a = m_steps[0], b = m_steps[1];
        m_steps.clear();
        if (m_tool == ToolMidpoint) {
            ::Shape s;
            s.id = m_doc->newId();
            s.type = ShapeType::Point;
            s.w = s.h = 10;
            s.pos = (a + b) / 2;
            frame().shapes.append(s);
            sync();
            emit message(tr("Midpoint at (%1, %2)").arg(s.pos.x(), 0, 'f', 1).arg(s.pos.y(), 0, 'f', 1));
            emit edited(tr("Midpoint"));
        } else {
            const QPointF d = b - a;
            emit message(tr("Distance %1 px · angle %2° · dx %3 · dy %4")
                             .arg(dist(a, b), 0, 'f', 1)
                             .arg(-Solver::angleDeg(d), 0, 'f', 1)
                             .arg(d.x(), 0, 'f', 1)
                             .arg(d.y(), 0, 'f', 1));
        }
        break;
    }
    case ToolPerpendicular:
    case ToolParallel: {
        if (!m_haveRef) {
            QPointF d;
            if (!edgeDirectionAt(sp, &d)) {
                emit message(tr("No line or edge there. Click on a line or close to the edge of a shape."));
                break;
            }
            const double l = std::hypot(d.x(), d.y());
            d /= l;
            m_refDir = m_tool == ToolPerpendicular ? QPointF(-d.y(), d.x()) : d;
            m_haveRef = true;
        } else if (m_steps.isEmpty()) {
            m_steps << pickPoint(sp);
        } else {
            const QPointF a = m_steps[0];
            const QPointF b = a + m_refDir * QPointF::dotProduct(pickPoint(sp) - a, m_refDir);
            m_steps.clear();
            if (dist(a, b) > 1) {
                const int id = addLine(a, b);
                sync();
                setSelectedIds({id});
                emit edited(m_tool == ToolPerpendicular ? tr("Perpendicular line") : tr("Parallel line"));
            }
            m_haveRef = false;
        }
        break;
    }
    default: break;
    }
    emit prompt(promptText());
    viewport()->update();
}

void Canvas::mouseMoveEvent(QMouseEvent* e)
{
    const QPointF sp = mapToScene(e->position().toPoint());
    emit cursorMoved(sp);
    if (m_drag == DPan) {
        const QPointF d = e->position() - m_panLast;
        m_panLast = e->position();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - qRound(d.x()));
        verticalScrollBar()->setValue(verticalScrollBar()->value() - qRound(d.y()));
        return;
    }
    if (m_drag == DRubber) {
        QGraphicsView::mouseMoveEvent(e);
        return;
    }
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const bool ctrl = e->modifiers() & Qt::ControlModifier;  // 15° steps for lines (Shift = free point)
    if (toolPicksPoints() || m_drag == DLineEnd) {
        const int excl = (m_drag == DLineEnd || m_drag == DCreate) ? m_dragId : (m_tool == ToolConnect ? m_connectFrom : -1);
        m_hover = pickPoint(sp, excl ? excl : -1, nullptr, &m_hoverFeature, &m_hoverKind);
        m_hoverValid = true;
        viewport()->update();
    } else {
        m_hoverValid = false;
    }
    if (m_drag == DNone) {
        updateHoverCursor(sp);
        return;
    }
    if (!m_moved && (e->position() - m_pressPos).manhattanLength() < 3) return;
    m_moved = true;

    Frame& f = frame();
    switch (m_drag) {
    case DMove: {
        const QVector<int> ids = selectedIds();
        const QPointF unsnapped = m_orig.pos + (sp - m_pressScene);
        if (ids.size() > 1) {
            const ::Shape* d = std::as_const(f).find(m_dragId);
            if (d) Solver::translateShapes(f, ids, snapPoint(unsnapped) - d->pos);
        } else {
            QPointF mouse = sp;
            if (snap && !Solver::dragInfo(f, m_dragId).hinge) mouse += snapPoint(unsnapped) - unsnapped;
            Solver::dragShape(f, m_dragId, m_grabLocal, mouse, true);
        }
        break;
    }
    case DResize: doResize(sp, shift || m_orig.lockAspect); break;
    case DLineEnd: {
        ::Shape* s = f.find(m_dragId);
        if (!s) break;
        const bool left = m_handle == 9;
        const QPointF fixed = m_orig.toWorld(QPointF(left ? m_orig.w / 2 : -m_orig.w / 2, 0));
        const QPointF moving = constrainLine(fixed, pickPoint(sp, m_dragId), ctrl);
        const QPointF a = left ? moving : fixed, b = left ? fixed : moving;
        s->pos = (a + b) / 2;
        s->w = qMax(1.0, dist(a, b));
        s->rot = Solver::angleDeg(b - a);
        Solver::enforce(f, m_dragId);
        break;
    }
    case DRotate: {
        const ::Shape* s = std::as_const(f).find(m_dragId);
        if (!s) break;
        m_rotAccum += Solver::normDeg(Solver::angleDeg(sp - m_pivot) - Solver::angleDeg(m_lastScene - m_pivot));
        double target = m_orig.rot + m_rotAccum;
        if (shift) target = std::round(target / 15) * 15;
        Solver::rotateShape(f, m_dragId, target - s->rot, true);
        break;
    }
    case DFreehand:
        if (dist(sp, m_drawPts.constLast()) > 1.5 / viewScale()) m_drawPts << sp;
        viewport()->update();
        return;
    case DTail: {
        ::Shape* s = f.find(m_dragId);
        if (!s || s->w <= 0 || s->h <= 0) break;
        const QPointF l = s->toLocal(pickPoint(sp, m_dragId));
        s->tail = QPointF(l.x() / s->w, l.y() / s->h);
        break;
    }
    case DNode: {
        ::Shape* s = f.find(m_dragId);
        if (!s) break;
        QVector<QPointF> lp = pathPointsLocal(*s);
        if (m_handle >= lp.size()) break;
        lp[m_handle] = s->toLocal(pickPoint(sp, m_dragId));
        setPathPointsLocal(*s, lp);
        Solver::enforce(f, m_dragId);
        break;
    }
    case DWarp: {
        ::Shape* s = f.find(m_dragId);
        if (!s || s->w <= 0 || s->h <= 0) break;
        const QPointF q = m_orig.outerTransform().inverted().map(snapPoint(sp));
        const QPointF base = s->cornerBase(m_handle);
        s->warp[m_handle] = QPointF((q.x() - base.x()) / s->w, (q.y() - base.y()) / s->h);
        Solver::enforce(f, m_dragId);
        break;
    }
    case DCreate: {
        ::Shape* s = f.find(m_dragId);
        if (!s) break;
        if (s->isLinear()) {
            const QPointF a = m_createStart, b = constrainLine(a, pickPoint(sp, m_dragId), ctrl);
            s->pos = (a + b) / 2;
            s->w = qMax(1.0, dist(a, b));
            s->rot = Solver::angleDeg(b - a);
        } else {
            QPointF d = snapPoint(sp) - m_createStart;
            if (shift) {
                const double m = qMax(std::abs(d.x()), std::abs(d.y()));
                d = QPointF(d.x() < 0 ? -m : m, d.y() < 0 ? -m : m);
            }
            const QRectF r = QRectF(m_createStart, m_createStart + d).normalized();
            s->pos = r.center();
            s->w = qMax(1.0, r.width());
            s->h = qMax(1.0, r.height());
        }
        break;
    }
    case DJoint: {
        const int ji = f.jointIndex(m_dragId);
        if (ji < 0) break;
        Joint& j = f.joints[ji];
        const ShapeItem* p = m_items.value(j.parent);
        const ShapeItem* c = m_items.value(j.child);
        if (!p || !c) break;
        const QPointF w = pickPoint(sp);
        j.anchorP = Solver::normalizeLocal(p->shapeData(), p->shapeData().toLocal(w));
        j.anchorC = Solver::normalizeLocal(c->shapeData(), c->shapeData().toLocal(w));
        break;
    }
    default: break;
    }
    m_lastScene = sp;
    sync();
    emit liveChanged();
}

void Canvas::glueEnd(::Shape& line, int end, QPointF world)
{
    int tid = 0;
    pickPoint(world, line.id, &tid);
    if (!tid) tid = topItemAt(world, line.id) ? topItemAt(world, line.id)->id() : 0;
    const ShapeItem* t = m_items.value(tid);
    if (!t || t->shapeData().isLinear()) return;  // only glue to solid shapes
    line.ends[end].target = tid;
    line.ends[end].anchor = Solver::normalizeLocal(t->shapeData(), t->shapeData().toLocal(world));
}

void Canvas::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_drag == DPan) {
        m_drag = DNone;
        viewport()->setCursor(m_spaceDown ? Qt::OpenHandCursor : Qt::ArrowCursor);
        return;
    }
    if (m_drag == DRubber) {
        QGraphicsView::mouseReleaseEvent(e);
        m_drag = DNone;
        expandToGroups();
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    const Drag d = m_drag;
    m_drag = DNone;
    switch (d) {
    case DMove:
        if (m_moved) startSpring(tr("Move"), selectedIds());
        break;
    case DRotate:
        if (m_moved) startSpring(tr("Rotate"), {m_dragId});
        break;
    case DResize:
        if (m_moved) emit edited(tr("Resize"));
        break;
    case DWarp:
        if (m_moved) emit edited(tr("Warp"));
        break;
    case DNode:
        if (m_moved) emit edited(tr("Move path point"));
        break;
    case DTail:
        if (m_moved) emit edited(tr("Move bubble tail"));
        break;
    case DFreehand: {
        QVector<QPointF> pts = m_drawPts;
        m_drawPts.clear();
        if (pts.size() < 3) break;
        // simplify (Ramer-Douglas-Peucker), then close if it ends near the start
        const double tol = 1.5 / viewScale();
        std::function<void(int, int, QVector<bool>&)> rdp = [&](int a, int b, QVector<bool>& keep) {
            double best = 0;
            int idx = -1;
            for (int i = a + 1; i < b; ++i) {
                const double d = segDist(pts[i], pts[a], pts[b]);
                if (d > best) {
                    best = d;
                    idx = i;
                }
            }
            if (idx >= 0 && best > tol) {
                keep[idx] = true;
                rdp(a, idx, keep);
                rdp(idx, b, keep);
            }
        };
        QVector<bool> keep(pts.size(), false);
        keep.first() = keep.last() = true;
        rdp(0, pts.size() - 1, keep);
        QVector<QPointF> simple;
        for (int i = 0; i < pts.size(); ++i)
            if (keep[i]) simple << pts[i];
        const bool closed = simple.size() > 3 && dist(simple.first(), simple.last()) < 12 / viewScale();
        if (closed) simple.removeLast();
        finishPath(simple, closed, true);
        break;
    }
    case DLineEnd: {
        ::Shape* s = frame().find(m_dragId);
        if (!s) break;
        const int end = m_handle - 9;
        bool glued = false;
        if (stickyEnds) {
            glueEnd(*s, end, s->toWorld(QPointF(end ? s->w / 2 : -s->w / 2, 0)));
            glued = s->ends[end].target != 0;
        }
        Solver::propagate(frame(), {{m_dragId, 0.0}});
        sync();
        if (m_moved || glued != (m_orig.ends[end].target != 0))
            emit edited(glued ? tr("Glue line end") : tr("Move line end"));
        break;
    }
    case DJoint:
        if (m_moved) emit edited(tr("Move pivot"));
        break;
    case DCreate: {
        ::Shape* s = frame().find(m_dragId);
        if (!s) break;
        if (!m_moved) {
            s->rot = 0;
            if (s->isLinear()) s->w = 120;
            else s->w = s->h = 100;
        } else if (s->isLinear() && stickyEnds) {
            if (m_createLinkTarget) glueEnd(*s, 0, s->toWorld(QPointF(-s->w / 2, 0)));
            glueEnd(*s, 1, s->toWorld(QPointF(s->w / 2, 0)));
            if (s->hasLinks()) emit message(tr("Line glued to shape(s); it will stretch when they move."));
        }
        const QString name = shapeTypeName(s->type);
        sync();
        setTool(ToolSelect);
        emit edited(tr("Add %1").arg(name));
        break;
    }
    case DCreateText: {
        ::Shape s = style;
        s.id = m_doc->newId();
        s.type = ShapeType::Text;
        s.name.clear();
        s.imageKey.clear();
        s.group = 0;
        s.squash = 1;
        for (QPointF& w : s.warp) w = QPointF();
        for (EndLink& l : s.ends) l = EndLink();
        s.text = tr("Text");
        s.fill = style.stroke.alpha() > 0 ? style.stroke : QColor(Qt::black);
        s.strokeWidth = 0;
        s.bubble = 0;
        if (m_tool == ToolBubble) {
            s.bubble = 1;
            s.text = tr("Hello!");
            s.fill = Qt::black;
            s.stroke = style.stroke.alpha() > 0 ? style.stroke : QColor(Qt::black);
            s.strokeWidth = qMax(2.0, style.strokeWidth);
            s.bubbleFill = Qt::white;
            s.tail = QPointF(-0.25, 1.05);
        }
        s.rot = 0;
        s.pos = snapPoint(m_pressScene);
        fitTextShape(s);
        frame().shapes.append(s);
        sync();
        setSelectedIds({s.id});
        setTool(ToolSelect);
        emit edited(tr("Add text"));
        emit textEditRequested(s.id);
        break;
    }
    default: break;
    }
    viewport()->update();
}

void Canvas::finishConnect(int a, QPointF pa, int b, QPointF pb)
{
    Frame& f = frame();
    QString why;
    const int jid = m_doc->nextId;
    if (!Solver::connectShapes(f, a, pa, b, pb, newJointType, jid, &why)) {
        emit message(why);
        return;
    }
    m_doc->nextId++;
    sync();
    setSelectedJoint(jid);
    emit message(tr("Connected (%1).").arg(jointTypeName(newJointType)));
    emit edited(tr("Connect"));
}

void Canvas::doResize(QPointF sp, bool keepAspect)
{
    Frame& f = frame();
    ::Shape* s = f.find(m_dragId);
    if (!s) return;
    const ::Shape& o = m_orig;
    const int hx = HX[m_handle], hy = HY[m_handle];
    const QPointF m = o.toLocal(snapPoint(sp));
    const double fx = -hx * o.w / 2, fy = -hy * o.h / 2;
    double nw = hx ? qMax(1.0, hx * (m.x() - fx)) : o.w;
    double nh = hy ? qMax(1.0, hy * (m.y() - fy)) : o.h;
    if (keepAspect && o.w > 0 && o.h > 0) {
        const double k = (hx && hy) ? qMax(nw / o.w, nh / o.h) : hx ? nw / o.w : nh / o.h;
        nw = o.w * k;
        nh = o.h * k;
    }
    const double cx = hx ? fx + hx * nw / 2 : 0, cy = hy ? fy + hy * nh / 2 : 0;
    s->pos = o.toWorld(QPointF(cx, cy));
    s->w = nw;
    s->h = nh;
    Solver::enforce(f, m_dragId);
}

// Connect every shape under the spot to the one directly below it (bottom = parent).
void Canvas::pinAt(QPointF sp, bool invertMode)
{
    const QPointF p = pickPoint(sp);
    QVector<int> stack;  // top first
    for (ShapeItem* it : shapeItemsAt(sp))
        if (it->shapeData().type != ShapeType::Point) stack << it->id();
    if (stack.size() < 2) {
        emit message(tr("Pin: click where at least two shapes overlap."));
        return;
    }
    const bool all = (pinMode == 1) != invertMode;
    if (!all) stack.resize(2);  // just the top shape and the one directly below it
    Frame& f = frame();
    int made = 0, lastJoint = -1;
    QStringList skipped;
    for (int i = stack.size() - 1; i > 0; --i) {
        const int parent = stack[i], child = stack[i - 1];
        QString why;
        const int jid = m_doc->nextId;
        if (Solver::connectShapes(f, parent, p, child, p, newJointType, jid, &why)) {
            m_doc->nextId++;
            lastJoint = jid;
            ++made;
        } else {
            skipped << why;
        }
    }
    sync();
    if (made) {
        setSelectedJoint(lastJoint);
        emit message(tr("Pinned %1 connection(s) (%2) at (%3, %4).%5")
                         .arg(made).arg(jointTypeName(newJointType)).arg(p.x(), 0, 'f', 1).arg(p.y(), 0, 'f', 1)
                         .arg(skipped.isEmpty() ? QString() : tr(" Skipped %1: already connected.").arg(skipped.size())));
        emit edited(tr("Pin"));
    } else {
        emit message(tr("Pin: nothing new to connect here. %1").arg(skipped.value(0)));
    }
    viewport()->update();
}

void Canvas::pathClick(QPointF sp, Qt::KeyboardModifiers mods)
{
    QPointF p = pickPoint(sp);
    if (!m_pathPts.isEmpty()) p = constrainLine(m_pathPts.constLast(), p, mods & Qt::ControlModifier);
    if (m_pathPts.size() >= 3 && dist(p, m_pathPts.first()) < 9 / viewScale()) {  // clicked the first point: close
        finishPath(m_pathPts, true, m_tool == ToolCurve);
        return;
    }
    if (!m_pathPts.isEmpty() && dist(p, m_pathPts.constLast()) < 0.5) return;  // same spot (e.g. double-click)
    m_pathPts << p;
    emit prompt(promptText());
    viewport()->update();
}

void Canvas::finishPath(QVector<QPointF> pts, bool closed, bool smooth)
{
    m_pathPts.clear();
    if (pts.size() < 2) {
        emit prompt(promptText());
        return;
    }
    ::Shape s = style;
    s.id = m_doc->newId();
    s.type = ShapeType::Path;
    s.name.clear();
    s.text.clear();
    s.imageKey.clear();
    s.group = 0;
    s.lockAspect = false;
    s.squash = 1;
    for (QPointF& w : s.warp) w = QPointF();
    for (EndLink& l : s.ends) l = EndLink();
    s.rot = 0;
    s.pos = QPointF();
    s.closed = closed;
    s.curve = smooth ? 1 : 0;
    if (s.strokeWidth <= 0) s.strokeWidth = 2;
    if (s.stroke.alpha() == 0) s.stroke = Qt::black;
    setPathPointsLocal(s, pts);
    frame().shapes.append(s);
    sync();
    setSelectedIds({s.id});
    const bool freehand = m_tool == ToolFreehand;
    if (!freehand) setTool(ToolSelect);
    emit edited(freehand ? tr("Freehand") : closed ? tr("Closed path") : tr("Path"));
    emit message(tr("%1 with %2 points%3. Drag its points (Select tool) to reshape; Closed / Smooth are in Properties.")
                     .arg(freehand ? tr("Freehand stroke") : tr("Path")).arg(pts.size()).arg(closed ? tr(", closed") : QString()));
}

void Canvas::mouseDoubleClickEvent(QMouseEvent* e)
{
    if ((m_tool == ToolPolyline || m_tool == ToolCurve) && !m_pathPts.isEmpty()) {
        finishPath(m_pathPts, false, m_tool == ToolCurve);
        return;
    }
    if (m_readOnly || m_tool != ToolSelect || e->button() != Qt::LeftButton) return;
    if (ShapeItem* it = topItemAt(mapToScene(e->position().toPoint())))
        if (it->shapeData().type == ShapeType::Text) emit textEditRequested(it->id());
}

void Canvas::wheelEvent(QWheelEvent* e)
{
    zoomBy(std::pow(1.0015, e->angleDelta().y()));
    e->accept();
}

void Canvas::refreshHover()
{
    if (!(toolPicksPoints() || m_drag == DLineEnd)) return;
    const QPoint vp = viewport()->mapFromGlobal(QCursor::pos());
    if (!viewport()->rect().contains(vp)) return;
    const int excl = (m_drag == DLineEnd || m_drag == DCreate) ? m_dragId : (m_tool == ToolConnect ? m_connectFrom : -1);
    m_hover = pickPoint(mapToScene(vp), excl ? excl : -1, nullptr, &m_hoverFeature, &m_hoverKind);
    m_hoverValid = true;
    viewport()->update();
}

void Canvas::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Shift) refreshHover();
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        m_spaceDown = true;
        viewport()->setCursor(Qt::OpenHandCursor);
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        if (!m_steps.isEmpty() || m_haveRef || m_connectFrom || !m_pathPts.isEmpty()) {
            resetConstruction();
            emit prompt(promptText());
            viewport()->update();
        } else if (m_tool != ToolSelect) {
            setTool(ToolSelect);
        } else {
            setSelectedIds({});
        }
        return;
    }
    if (!m_pathPts.isEmpty()) {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            finishPath(m_pathPts, false, m_tool == ToolCurve);
            return;
        }
        if (e->key() == Qt::Key_Backspace) {
            m_pathPts.removeLast();
            emit prompt(promptText());
            viewport()->update();
            return;
        }
    }
    if (!m_readOnly) {
        const double st = (e->modifiers() & Qt::ShiftModifier) ? (snap ? gridSize : 10) : 1;
        QPointF d;
        switch (e->key()) {
        case Qt::Key_Left: d = {-st, 0}; break;
        case Qt::Key_Right: d = {st, 0}; break;
        case Qt::Key_Up: d = {0, -st}; break;
        case Qt::Key_Down: d = {0, st}; break;
        default: break;
        }
        const QVector<int> ids = selectedIds();
        if (!d.isNull() && !ids.isEmpty()) {
            finishSpring(true);
            Solver::translateShapes(frame(), ids, d);
            sync();
            emit edited(QStringLiteral("Nudge"));
            return;
        }
    }
    QGraphicsView::keyPressEvent(e);
}

void Canvas::keyReleaseEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Shift) QTimer::singleShot(0, this, &Canvas::refreshHover);  // modifiers update after the event
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        m_spaceDown = false;
        viewport()->setCursor(m_tool == ToolSelect || m_tool == ToolWarp ? Qt::ArrowCursor : Qt::CrossCursor);
        return;
    }
    QGraphicsView::keyReleaseEvent(e);
}

// ---------------------------------------------------------------- drag & drop images

void Canvas::contextMenuEvent(QContextMenuEvent* e)
{
    if (m_readOnly || !m_doc) return;
    const QPointF sp = mapToScene(e->pos());
    if (ShapeItem* it = topItemAt(sp); it && !it->isSelected()) {  // right-click selects (whole group)
        m_scene->clearSelection();
        const int g = it->shapeData().group;
        for (ShapeItem* o : std::as_const(m_items))
            if (o == it || (g && o->shapeData().group == g)) o->setSelected(true);
    }
    emit contextMenuAt(e->globalPos(), sp);
}

static const char* kLibraryMime = "application/x-tood-library";

void Canvas::dragEnterEvent(QDragEnterEvent* e)
{
    if (!m_readOnly && (e->mimeData()->hasUrls() || e->mimeData()->hasFormat(kLibraryMime))) e->acceptProposedAction();
}
void Canvas::dragMoveEvent(QDragMoveEvent* e)
{
    if (!m_readOnly && (e->mimeData()->hasUrls() || e->mimeData()->hasFormat(kLibraryMime))) e->acceptProposedAction();
}
void Canvas::dropEvent(QDropEvent* e)
{
    const QPointF sp = mapToScene(e->position().toPoint());
    if (e->mimeData()->hasFormat(kLibraryMime)) {
        emit libraryDropped(QString::fromUtf8(e->mimeData()->data(kLibraryMime)), sp);
    } else {
        for (const QUrl& u : e->mimeData()->urls())
            if (u.isLocalFile()) emit imageDropped(u.toLocalFile(), sp);
    }
    e->acceptProposedAction();
}

// ---------------------------------------------------------------- painting

void Canvas::drawBackground(QPainter* p, const QRectF& rect)
{
    p->fillRect(rect, QColor(64, 64, 70));
    if (!m_doc) return;
    const double sc = viewScale();
    const QRectF cr(0, 0, m_doc->width, m_doc->height);
    p->fillRect(cr.translated(5 / sc, 5 / sc), QColor(0, 0, 0, 90));
    if (m_doc->background.alpha() < 255) p->fillRect(cr, QBrush(checker()));
    p->fillRect(cr, m_doc->background);

    if (onion && !m_readOnly && m_hasOnion) {
        p->save();
        p->setClipRect(cr);
        p->setOpacity(0.28);
        renderFrame(p, *m_doc, m_onionFrame, false);
        p->restore();
    }
    if (showGrid && gridSize * sc >= 5) {
        const QRectF r = rect.intersected(cr);
        QVector<QLineF> lines;
        for (double x = std::ceil(r.left() / gridSize) * gridSize; x <= r.right(); x += gridSize)
            lines << QLineF(x, r.top(), x, r.bottom());
        for (double y = std::ceil(r.top() / gridSize) * gridSize; y <= r.bottom(); y += gridSize)
            lines << QLineF(r.left(), y, r.right(), y);
        p->setPen(QPen(QColor(0, 0, 0, 38), 0));
        p->drawLines(lines);
    }
}

void Canvas::drawForeground(QPainter* p, const QRectF& rect)
{
    if (!m_doc) return;
    const double sc = viewScale();
    const QRectF cr(0, 0, m_doc->width, m_doc->height);
    {   // dim everything outside the canvas (odd-even fill leaves a hole)
        QPainterPath pp;
        pp.addRect(rect.adjusted(-2, -2, 2, 2));
        pp.addRect(cr);
        p->fillPath(pp, QColor(30, 30, 36, 150));
    }
    p->setRenderHint(QPainter::Antialiasing, true);
    auto cosmetic = [](const QColor& c, double w, Qt::PenStyle st = Qt::SolidLine) {
        QPen pen(c, w, st);
        pen.setCosmetic(true);
        return pen;
    };
    if (showMotion && m_ghosts.size() > 1) {  // motion paths of shapes that move through the tween
        QHash<int, QPolygonF> paths;
        for (const Frame& g : std::as_const(m_ghosts))
            for (const ::Shape& s : g.shapes)
                if (s.type != ShapeType::Point) paths[s.id] << s.pos;
        QSet<int> moving;
        for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
            const QRectF r = it.value().boundingRect();
            if (r.width() + r.height() > 1.0) moving.insert(it.key());
        }
        p->setBrush(Qt::NoBrush);
        p->setPen(cosmetic(QColor(0, 110, 255, 60), 1));
        const int step = qMax(1, int(m_ghosts.size()) / 40);  // at most ~40 ghost outlines
        for (int i = 0; i < m_ghosts.size(); i += step)
            for (const ::Shape& s : m_ghosts.at(i).shapes)
                if (moving.contains(s.id)) p->drawPath(s.transform().map(shapePath(s)));
        for (int id : std::as_const(moving)) {
            const QPolygonF& poly = paths.value(id);
            p->setPen(cosmetic(QColor(255, 110, 0, 210), 1.5, Qt::DashLine));
            p->drawPolyline(poly);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(255, 110, 0, 210));
            for (const QPointF& pt : poly) p->drawEllipse(pt, 2.5 / sc, 2.5 / sc);
            p->setBrush(Qt::NoBrush);
        }
    }
    if (m_readOnly) return;

    // selection outlines (+ dashed box around selected groups)
    const QVector<int> ids = selectedIds();
    QHash<int, QRectF> groupBoxes;
    for (int id : ids) {
        const ShapeItem* it = m_items.value(id);
        if (!it) continue;
        const ::Shape& s = it->shapeData();
        p->setPen(cosmetic(QColor(0, 150, 255), 1.5));
        p->setBrush(Qt::NoBrush);
        if (s.isLinear())
            p->drawLine(s.toWorld({-s.w / 2, 0}), s.toWorld({s.w / 2, 0}));
        else if (s.type == ShapeType::Point)
            p->drawEllipse(s.pos, 8 / sc, 8 / sc);
        else
            p->drawPolygon(s.transform().map(QPolygonF(s.localRect())));
        if (s.group) groupBoxes[s.group] = groupBoxes.value(s.group).united(it->sceneBoundingRect());
    }
    p->setPen(cosmetic(QColor(0, 150, 255), 1, Qt::DashLine));
    for (const QRectF& r : std::as_const(groupBoxes)) p->drawRect(r.adjusted(-4 / sc, -4 / sc, 4 / sc, 4 / sc));

    // handles
    if (ids.size() == 1 && (m_tool == ToolSelect || m_tool == ToolWarp)) {
        if (const ShapeItem* it = m_items.value(ids.first())) {
            const ::Shape& s = it->shapeData();
            const double hs = 8 / sc;
            p->setPen(cosmetic(QColor(0, 120, 215), 1));
            p->setBrush(Qt::white);
            if (s.type == ShapeType::Point) {
            } else if (s.isLinear()) {
                for (double x : {-s.w / 2, s.w / 2}) p->drawEllipse(s.toWorld({x, 0}), hs * 0.6, hs * 0.6);
            } else if (m_tool == ToolWarp) {
                p->setBrush(QColor(255, 150, 0));
                for (int k = 0; k < 4; ++k) {
                    const QPointF c = s.toWorld(s.cornerBase(k));
                    QPolygonF dmd;
                    dmd << c + QPointF(0, -hs * 0.8) << c + QPointF(hs * 0.8, 0) << c + QPointF(0, hs * 0.8) << c + QPointF(-hs * 0.8, 0);
                    p->drawPolygon(dmd);
                }
            } else {
                for (int k = 0; k < 8; ++k) {
                    const QPointF c = s.toWorld({HX[k] * s.w / 2, HY[k] * s.h / 2});
                    p->drawRect(QRectF(c.x() - hs / 2, c.y() - hs / 2, hs, hs));
                }
                const QPointF top = s.toWorld({0, -s.h / 2});
                const QPointF rh = s.toWorld({0, -s.h / 2 - kRotHandlePx / sc});
                p->drawLine(top, rh);
                p->setBrush(QColor(0, 150, 255));
                p->drawEllipse(rh, hs * 0.6, hs * 0.6);
            }
        }
    }

    // tail handle of a selected speech bubble
    if (ids.size() == 1 && m_tool == ToolSelect) {
        if (const ShapeItem* it = m_items.value(ids.first()); it && it->shapeData().type == ShapeType::Text && it->shapeData().bubble) {
            const ::Shape& s = it->shapeData();
            const QPointF c = s.toWorld(QPointF(s.tail.x() * s.w, s.tail.y() * s.h));
            const double r = 5 / sc;
            p->setPen(cosmetic(QColor(60, 60, 60), 1));
            p->setBrush(QColor(255, 210, 0));
            p->drawPolygon(QPolygonF() << c + QPointF(0, -r) << c + QPointF(r, 0) << c + QPointF(0, r) << c + QPointF(-r, 0));
        }
    }
    // points of a selected path (drag to reshape)
    if (ids.size() == 1 && m_tool == ToolSelect) {
        if (const ShapeItem* it = m_items.value(ids.first()); it && it->shapeData().type == ShapeType::Path &&
                                                              it->shapeData().pts.size() <= 300) {
            const ::Shape& s = it->shapeData();
            p->setPen(cosmetic(QColor(0, 120, 215), 1));
            p->setBrush(Qt::white);
            for (const QPointF& lp : pathPointsLocal(s)) p->drawEllipse(s.toWorld(lp), 3.5 / sc, 3.5 / sc);
        }
    }
    // path being drawn
    if (!m_pathPts.isEmpty()) {
        QVector<QPointF> pts = m_pathPts;
        if (m_hoverValid) pts << m_hover;
        p->setPen(cosmetic(QColor(0, 120, 215), 1.5));
        p->setBrush(Qt::NoBrush);
        p->drawPath(pathThrough(pts, false, m_tool == ToolCurve));
        p->setBrush(QColor(0, 120, 215));
        for (const QPointF& q : m_pathPts) p->drawEllipse(q, 3 / sc, 3 / sc);
        if (m_pathPts.size() >= 3) {  // ring on the first point: click it to close
            p->setBrush(Qt::NoBrush);
            p->drawEllipse(m_pathPts.first(), 7 / sc, 7 / sc);
        }
    }
    if (m_drag == DFreehand && m_drawPts.size() > 1) {
        p->setPen(cosmetic(QColor(0, 120, 215), 1.5));
        p->drawPolyline(QPolygonF(m_drawPts));
    }

    QVector<QPair<QPointF, bool>> rootStars;  // drawn last so pin markers never cover them
    bool anyHot = false;
    if (showJoints && !cframe().joints.isEmpty()) {
        // connection lines: parent ··· pin → child (arrow), stronger for the selected assembly; ★ on roots
        QSet<int> hot;
        if (!ids.isEmpty()) hot = Solver::assembly(cframe(), QSet<int>(ids.cbegin(), ids.cend()));
        else if (m_selJoint >= 0)
            if (const int ji = cframe().jointIndex(m_selJoint); ji >= 0) hot = Solver::assembly(cframe(), {cframe().joints.at(ji).child});
        anyHot = !hot.isEmpty();
        QSet<int> parents, children;
        for (const Joint& j : cframe().joints) {
            const ShapeItem* pi = m_items.value(j.parent);
            const ShapeItem* ci = m_items.value(j.child);
            if (!pi || !ci) continue;
            parents << j.parent;
            children << j.child;
            const bool h = hot.contains(j.child);
            QColor c = jointColor(j.type);
            c.setAlpha(h ? 220 : (hot.isEmpty() ? 110 : 45));
            const QPointF w = Solver::anchorWorld(pi->shapeData(), j.anchorP);
            const QPointF pc = pi->shapeData().pos, cc = ci->shapeData().pos;
            p->setBrush(Qt::NoBrush);
            p->setPen(cosmetic(c, h ? 2 : 1, Qt::DotLine));
            p->drawLine(pc, w);
            p->setPen(cosmetic(c, h ? 2 : 1));
            p->drawLine(w, cc);
            const QPointF d = cc - w;
            const double l = std::hypot(d.x(), d.y());
            if (l > 12 / sc) {  // arrowhead at the child
                const QPointF u = d / l, nrm(-u.y(), u.x());
                const double a = (h ? 9 : 7) / sc;
                p->setPen(Qt::NoPen);
                p->setBrush(c);
                p->drawPolygon(QPolygonF() << cc << cc - u * a + nrm * a * 0.5 << cc - u * a - nrm * a * 0.5);
            }
        }
        for (int rid : std::as_const(parents)) {
            if (children.contains(rid)) continue;
            const ShapeItem* ri = m_items.value(rid);
            if (!ri) continue;
            rootStars.append({ri->shapeData().pos + QPointF(-11 / sc, -11 / sc), hot.contains(rid)});
        }
    }
    auto drawStars = [&] {
        for (const auto& rs : std::as_const(rootStars)) {
            const QPointF c = rs.first;
            const bool h = rs.second;
            const double R = (h ? 9 : 7) / sc;
            QPolygonF star;
            for (int k = 0; k < 10; ++k) {
                const double a = -M_PI / 2 + M_PI * k / 5;
                const double r = (k & 1) ? R * 0.45 : R;
                star << c + QPointF(std::cos(a) * r, std::sin(a) * r);
            }
            p->setPen(cosmetic(QColor(90, 60, 0), 1));
            p->setBrush(QColor(255, 200, 0, h || !anyHot ? 240 : 120));
            p->drawPolygon(star);
            if (h) {
                const QPoint vp = mapFromScene(c) + QPoint(11, -9);
                p->save();
                p->resetTransform();
                p->setPen(QColor(120, 80, 0));
                QFont fnt = p->font();
                fnt.setBold(true);
                p->setFont(fnt);
                p->drawText(vp, tr("root"));
                p->restore();
            }
        }
    };

    if (showJoints) {
        // glued connector ends
        p->setPen(cosmetic(Qt::white, 1));
        p->setBrush(QColor(0, 140, 255));
        for (const ShapeItem* it : std::as_const(m_items)) {
            const ::Shape& s = it->shapeData();
            if (!s.hasLinks()) continue;
            for (int k = 0; k < 2; ++k)
                if (s.ends[k].target) {
                    const QPointF c = s.toWorld(QPointF(k ? s.w / 2 : -s.w / 2, 0));
                    p->drawRect(QRectF(c.x() - 3.5 / sc, c.y() - 3.5 / sc, 7 / sc, 7 / sc));
                }
        }
        // joints
        for (const Joint& j : cframe().joints) {
            const ShapeItem* pi = m_items.value(j.parent);
            const ShapeItem* ci = m_items.value(j.child);
            if (!pi || !ci) continue;
            const QPointF w = Solver::anchorWorld(pi->shapeData(), j.anchorP);
            const bool sel = j.id == m_selJoint;
            const double r = (sel ? 7 : 5) / sc;
            const QColor col = jointColor(j.type);
            if (j.type == JointType::Elastic) {  // the stretched band
                const QPointF wc = Solver::anchorWorld(ci->shapeData(), j.anchorC);
                if (dist(w, wc) > 0.5) {
                    p->setPen(cosmetic(col, 2.5));
                    p->drawLine(w, wc);
                    p->setBrush(col);
                    p->drawEllipse(wc, r * 0.7, r * 0.7);
                }
            }
            if (sel) {
                const ::Shape& c = ci->shapeData();
                const ::Shape& par = pi->shapeData();
                if (j.type == JointType::Hinge && j.limited) {
                    const double rel = Solver::normDeg(c.rot - par.rot - j.restAngle);
                    const double dir = Solver::angleDeg(c.pos - w);
                    const double start = dir + (j.minAngle - rel);
                    const double span = j.maxAngle - j.minAngle;
                    const double R = 40 / sc;
                    p->setPen(Qt::NoPen);
                    p->setBrush(QColor(255, 140, 0, 60));
                    p->drawPie(QRectF(w.x() - R, w.y() - R, 2 * R, 2 * R), qRound(-start * 16), qRound(-span * 16));
                }
                p->setPen(cosmetic(col, 1.5, Qt::DashLine));
                p->drawLine(w, c.pos);
                p->drawLine(w, par.pos);
            }
            p->setPen(cosmetic(sel ? QColor(Qt::white) : QColor(20, 20, 20), sel ? 2 : 1));
            p->setBrush(col);
            if (j.type == JointType::Weld)
                p->drawRect(QRectF(w.x() - r, w.y() - r, 2 * r, 2 * r));
            else
                p->drawEllipse(w, r, r);
            if (j.type == JointType::Hinge) {
                p->setPen(Qt::NoPen);
                p->setBrush(Qt::white);
                p->drawEllipse(w, r * 0.35, r * 0.35);
            }
        }
    }

    drawStars();

    // construction previews
    const QPen guide = cosmetic(QColor(230, 0, 140), 1.5, Qt::DashLine);
    if (m_tool == ToolConnect && m_connectFrom) {
        p->setPen(cosmetic(QColor(255, 140, 0), 2, Qt::DashLine));
        p->setBrush(Qt::NoBrush);
        if (m_hoverValid) p->drawLine(m_connectPoint, m_hover);
        p->setBrush(QColor(255, 140, 0));
        p->drawEllipse(m_connectPoint, 5 / sc, 5 / sc);
    }
    // snap markers of the shapes around the cursor, so exact points are visible before clicking
    if (m_hoverValid && snapPoints && !(QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier) &&
        (toolPicksPoints() || m_drag == DLineEnd)) {
        const double R = 70 / sc, m = 3 / sc;
        p->setPen(cosmetic(QColor(230, 0, 140, 170), 1));
        p->setBrush(Qt::NoBrush);
        const auto near = m_scene->items(QRectF(m_hover.x() - R, m_hover.y() - R, 2 * R, 2 * R), Qt::IntersectsItemBoundingRect);
        for (QGraphicsItem* gi : near) {
            auto* si = qgraphicsitem_cast<ShapeItem*>(gi);
            if (!si || si->id() == m_dragId) continue;
            const ::Shape& s = si->shapeData();
            for (const SnapPoint& f : ::snapPoints(s)) {
                const QPointF c = s.toWorld(f.local);
                switch (f.kind) {
                case SnapKind::Corner: p->drawRect(QRectF(c.x() - m, c.y() - m, 2 * m, 2 * m)); break;
                case SnapKind::Midpoint: {
                    QPolygonF tri;
                    tri << c + QPointF(0, -m * 1.3) << c + QPointF(m * 1.2, m) << c + QPointF(-m * 1.2, m);
                    p->drawPolygon(tri);
                    break;
                }
                default:
                    p->drawEllipse(c, m, m);
                    p->drawLine(c - QPointF(m * 1.6, 0), c + QPointF(m * 1.6, 0));
                    p->drawLine(c - QPointF(0, m * 1.6), c + QPointF(0, m * 1.6));
                }
            }
        }
    }
    if (m_hoverValid && m_drag == DNone && !m_steps.isEmpty()) {
        QPointF a = m_steps[0], b = m_hover;
        if (m_haveRef) b = a + m_refDir * QPointF::dotProduct(m_hover - a, m_refDir);
        p->setPen(guide);
        p->drawLine(a, b);
        if (m_tool == ToolMeasure || m_haveRef) {
            const QPoint vp = mapFromScene(b) + QPoint(12, -10);
            p->save();
            p->resetTransform();
            p->setPen(Qt::white);
            p->drawText(vp, QStringLiteral("%1 px").arg(dist(a, b), 0, 'f', 1));
            p->restore();
        }
    }
    if (m_hoverValid && m_haveRef && m_steps.isEmpty()) {  // show reference direction through the cursor
        p->setPen(cosmetic(QColor(230, 0, 140, 120), 1, Qt::DotLine));
        p->drawLine(m_hover - m_refDir * 2000, m_hover + m_refDir * 2000);
    }
    if (m_hoverValid) {
        const double r = 5 / sc;
        p->setPen(cosmetic(QColor(230, 0, 140), 1.5));
        p->setBrush(m_hoverFeature ? QColor(230, 0, 140) : QColor(Qt::transparent));
        if (m_hoverKind == SnapKind::Edge) {
            p->drawLine(m_hover - QPointF(r, r), m_hover + QPointF(r, r));
            p->drawLine(m_hover - QPointF(r, -r), m_hover + QPointF(r, -r));
        } else {
            p->drawRect(QRectF(m_hover.x() - r, m_hover.y() - r, 2 * r, 2 * r));
        }
        const QString label = snapKindName(m_hoverKind);
        if (!label.isEmpty()) {
            const QPoint vp = mapFromScene(m_hover) + QPoint(10, 18);
            p->save();
            p->resetTransform();
            QFont fnt = p->font();
            fnt.setPointSizeF(8.5);
            p->setFont(fnt);
            const QRect br = p->fontMetrics().boundingRect(label).adjusted(-3, -1, 3, 1).translated(vp);
            p->fillRect(br, QColor(230, 0, 140, 210));
            p->setPen(Qt::white);
            p->drawText(br, Qt::AlignCenter, label);
            p->restore();
        }
    }
}
