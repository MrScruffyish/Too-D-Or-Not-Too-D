// Document model: plain value types, independent of the scene/view.
// All containers are Qt implicitly-shared (copy-on-write), so copying a whole
// Document for undo snapshots or duplicating frames is O(1) until modified.
#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <QVector>

class QPainter;

// Point = construction/helper point: visible while editing, never exported.
// Path = arbitrary polyline / smooth curve (freehand, polyline and curve tools).
enum class ShapeType { Rect, Ellipse, Triangle, Polygon, Star, Line, Arrow, Text, Image,
                       Hourglass, Diamond, Eye, Point, Path };
constexpr int kShapeTypeCount = 14;

// Hinge:   child can only rotate around the pin; dragging the parent carries the child.
// Free:    pin stays connected, both sides move freely and the other side swings to trail.
// Weld:    rigid, the two shapes move and rotate as one.
// Elastic: rubber band; the far side lags behind while dragging and springs back on release.
enum class JointType { Hinge, Free, Weld, Elastic };

// A Line/Arrow end glued to a point on another shape (connector that stretches).
struct EndLink {
    int target = 0;   // shape id, 0 = free end
    QPointF anchor;   // on the target, local coords normalised by (w, h)
};

struct Shape {
    int id = 0;
    ShapeType type = ShapeType::Rect;
    QString name;
    QPointF pos;          // centre, world coordinates
    double rot = 0;       // degrees, clockwise (Qt convention), unwrapped
    double w = 100, h = 100;
    QColor fill = QColor(80, 140, 220);
    QColor stroke = QColor(30, 30, 40);
    double strokeWidth = 2;
    double opacity = 1;
    double cornerRadius = 0;   // Rect
    int sides = 5;             // Polygon / Star
    double innerRatio = 0.45;  // Star
    QString text;              // Text
    QString fontFamily;
    double fontSize = 32;
    bool bold = false;
    bool italic = false;
    int align = 0;             // Text: 0 centre, 1 left, 2 right
    int group = 0;             // shapes with the same non-zero group are selected together
    QString imageKey;          // Image -> Document::images
    bool lockAspect = false;   // keep w:h when resizing (Ellipse: circle)
    double squash = 1;         // stretch factor along squashAngle, 1/squash across (area kept)
    double squashAngle = 0;    // degrees, relative to the shape
    QPointF warp[4];           // free-form corner offsets TL,TR,BR,BL, normalised by (w, h)
    int lineStyle = 0;         // Line/Arrow: 0 straight, 1 spring (sides = coils)
    double amplitude = 8;      // spring half-width
    QColor accent = QColor(35, 30, 30);  // Eye: pupil colour
    QVector<QPointF> pts;      // Path: points in local coords normalised by (w, h), so resizing scales them
    bool closed = false;       // Path: closed (fillable) or open
    int curve = 0;             // Path: 0 straight segments, 1 smooth curve through the points
    int bubble = 0;            // Text: 0 none, 1 speech, 2 thought, 3 shout, 4 box (caption)
    QPointF tail = QPointF(-0.25, 1.05);  // Text bubble: tail tip, local coords normalised by (w, h)
    QColor bubbleFill = Qt::white;
    EndLink tailLink;          // Text bubble: tail tip glued to a point on another shape (optional)
    EndLink ends[2];           // Line/Arrow: start (-w/2) and end (+w/2)

    bool hasWarp() const;
    bool deformed() const { return squash != 1.0 || hasWarp(); }
    bool hasLinks() const { return ends[0].target || ends[1].target; }
    QTransform localTransform() const;  // warp, squash, rotation (item transform, no translation)
    QTransform outerTransform() const;  // squash, rotation, translation (everything except warp)
    QTransform transform() const;       // local -> world
    QPointF toWorld(QPointF local) const;
    QPointF toLocal(QPointF world) const;
    QRectF localRect() const { return QRectF(-w / 2, -h / 2, w, h); }
    QPointF cornerBase(int k) const;    // undeformed corner k (TL,TR,BR,BL), local
    bool isLinear() const { return type == ShapeType::Line || type == ShapeType::Arrow; }
};
bool operator==(const Shape& a, const Shape& b);
inline bool operator!=(const Shape& a, const Shape& b) { return !(a == b); }

// Joints form a forest: every shape has at most one parent joint, no loops.
struct Joint {
    int id = 0;
    int parent = 0, child = 0;
    JointType type = JointType::Hinge;
    QPointF anchorP, anchorC;  // anchor in each shape's local coords, normalised by (w, h)
    double restAngle = 0;      // child.rot - parent.rot when created
    bool limited = false;      // Hinge angle limits, relative to restAngle
    double minAngle = -45, maxAngle = 45;
    double stiffness = 0.3;    // Elastic: 0.05 (very stretchy) .. 1 (stiff)
};

struct Frame {
    QVector<Shape> shapes;     // back-to-front (index = z order)
    QVector<Joint> joints;
    int durationMs = 100;     // for a tween frame: duration of EACH generated in-between
    // Dynamic tween: when tweenCount > 0 this frame holds no shapes; it stands for
    // tweenCount in-betweens computed live from the keyframes before and after it.
    int tweenCount = 0;
    int tweenEase = 1;        // Solver::ease mode
    double tweenSquash = 0;   // 0..1
    int tweenPath = 0;        // 0 straight, 1 arc, 2 thrown (gravity)
    double tweenArc = 0;      // arc / throw height in px
    bool isTween() const { return tweenCount > 0; }

    int indexOf(int shapeId) const;
    Shape* find(int shapeId);
    const Shape* find(int shapeId) const;
    int jointIndex(int jointId) const;
    int parentJointIndex(int childId) const;
};

// A project is split into chapters; you edit one chapter at a time and the
// movie export can concatenate all of them.
struct Chapter {
    QString name;
    QVector<Frame> frames;  // stale (empty) for the active chapter, see Document::frames
};

struct Document {
    int width = 800, height = 600;
    QColor background = Qt::white;
    int fps = 12;
    QVector<Frame> frames;          // frames of the ACTIVE chapter (the live copy everything edits)
    QVector<Chapter> chapters;      // all chapters in order
    int activeChapter = 0;
    QHash<QString, QImage> images;  // content-hash -> decoded image, shared by all frames and chapters
    QHash<QString, QByteArray> imageFiles;  // same keys: the encoded file bytes that get saved (original when possible)
    int nextId = 1;

    Document()
    {
        frames.append(Frame());
        chapters.append(Chapter{QStringLiteral("Chapter 1"), {}});
    }
    int newId() { return nextId++; }
    void setImage(const QString& key, const QImage& img, const QByteArray& fileBytes)
    {
        images.insert(key, img.convertToFormat(QImage::Format_ARGB32_Premultiplied));
        imageFiles.insert(key, fileBytes);
    }
    void copyImageFrom(const Document& src, const QString& key)  // no-op if already present
    {
        if (images.contains(key) || !src.images.contains(key)) return;
        images.insert(key, src.images.value(key));
        if (src.imageFiles.contains(key)) imageFiles.insert(key, src.imageFiles.value(key));
    }
    const QVector<Frame>& chapterFrames(int c) const { return c == activeChapter ? frames : chapters.at(c).frames; }
    int totalFrames() const
    {
        int n = 0;
        for (int c = 0; c < chapters.size(); ++c) n += chapterFrames(c).size();
        return n;
    }
    void switchChapter(int c)
    {
        if (c == activeChapter || c < 0 || c >= chapters.size()) return;
        chapters[activeChapter].frames = frames;
        activeChapter = c;
        frames = chapters[c].frames;
        chapters[c].frames.clear();
    }

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& o, Document& out, QString* err);
    bool save(const QString& path, QString* err) const;
    static bool load(const QString& path, Document& out, QString* err);
};

// JPEG (quality 92) for opaque images, PNG when there is transparency.
QByteArray encodeImage(const QImage& img);

QString shapeTypeName(ShapeType t);
QString jointTypeName(JointType t);
QFont shapeFont(const Shape& s);
void fitTextShape(Shape& s);
double arrowHeadLength(const Shape& s);
QPainterPath shapePath(const Shape& s);
QPainterPath linePath(const Shape& s, double x0, double x1);  // straight or spring
// Path through points: straight segments or a smooth Catmull-Rom curve (as cubic Béziers).
QPainterPath pathThrough(const QVector<QPointF>& p, bool closed, bool smooth);
QPainterPath bubblePath(const Shape& s);  // speech bubble outline around a text shape (local coords)
QStringList bubbleNames();
QVector<QPointF> pathPointsLocal(const Shape& s);                 // denormalised (local coords)
void setPathPointsLocal(Shape& s, const QVector<QPointF>& local);  // refits pos/w/h around the points
// Exact points of a shape that point-picking tools snap to (local coordinates).
enum class SnapKind { Corner, Midpoint, Centre, Edge, Grid, Free };
struct SnapPoint {
    QPointF local;
    SnapKind kind;
};
QVector<SnapPoint> snapPoints(const Shape& s);
QString snapKindName(SnapKind k);

// Paints a shape in its local coordinate system (centre at origin, unrotated).
void paintShapeLocal(QPainter* p, const Shape& s, const QImage* img);
// Paints a whole frame in document coordinates.
void renderFrame(QPainter* p, const Document& d, const Frame& f, bool background);
QImage renderFrameImage(const Document& d, const Frame& f, double scale, bool background,
                        const QColor& base = QColor(Qt::transparent));
