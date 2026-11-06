#include "model.h"

#include <QBuffer>
#include <QFile>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QSaveFile>
#include <QSet>
#include <QtMath>
#include <cmath>

namespace {
const char* const kShapeKeys[] = {"rect", "ellipse", "triangle", "polygon", "star",  "line",  "arrow",
                                  "text", "image",   "hourglass", "diamond", "eye", "point"};
const char* const kJointKeys[] = {"hinge", "free", "weld", "elastic"};

QJsonObject linkToJson(const EndLink& l)
{
    QJsonObject o;
    o["t"] = l.target;
    o["x"] = l.anchor.x();
    o["y"] = l.anchor.y();
    return o;
}
EndLink linkFromJson(const QJsonValue& v)
{
    const QJsonObject o = v.toObject();
    EndLink l;
    l.target = o.value("t").toInt();
    l.anchor = QPointF(o.value("x").toDouble(), o.value("y").toDouble());
    return l;
}

QString colorStr(const QColor& c) { return c.name(QColor::HexArgb); }
QColor colorFrom(const QJsonValue& v, const QColor& def)
{
    if (!v.isString()) return def;
    QColor c = QColor::fromString(v.toString());
    return c.isValid() ? c : def;
}

QJsonObject shapeToJson(const Shape& s)
{
    QJsonObject o;
    o["id"] = s.id;
    o["type"] = kShapeKeys[int(s.type)];
    if (!s.name.isEmpty()) o["name"] = s.name;
    o["x"] = s.pos.x();
    o["y"] = s.pos.y();
    o["rot"] = s.rot;
    o["w"] = s.w;
    o["h"] = s.h;
    o["fill"] = colorStr(s.fill);
    o["stroke"] = colorStr(s.stroke);
    o["sw"] = s.strokeWidth;
    o["op"] = s.opacity;
    if (s.type == ShapeType::Rect) o["cr"] = s.cornerRadius;
    o["sides"] = s.sides;
    o["inner"] = s.innerRatio;
    if (s.group) o["grp"] = s.group;
    if (s.type == ShapeType::Text) {
        o["text"] = s.text;
        o["font"] = s.fontFamily;
        o["fs"] = s.fontSize;
        o["bold"] = s.bold;
        o["it"] = s.italic;
        o["al"] = s.align;
    }
    if (s.type == ShapeType::Image) o["img"] = s.imageKey;
    if (s.lockAspect) o["lock"] = true;
    if (s.squash != 1.0) {
        o["sq"] = s.squash;
        o["sqa"] = s.squashAngle;
    }
    if (s.hasWarp()) {
        QJsonArray wa;
        for (const QPointF& p : s.warp) wa << p.x() << p.y();
        o["warp"] = wa;
    }
    if (s.isLinear()) {
        o["ls"] = s.lineStyle;
        o["amp"] = s.amplitude;
        if (s.ends[0].target) o["endA"] = linkToJson(s.ends[0]);
        if (s.ends[1].target) o["endB"] = linkToJson(s.ends[1]);
    }
    if (s.type == ShapeType::Eye) o["acc"] = colorStr(s.accent);
    return o;
}

Shape shapeFromJson(const QJsonObject& o)
{
    Shape s;
    s.id = o.value("id").toInt();
    const QString t = o.value("type").toString();
    for (int i = 0; i < kShapeTypeCount; ++i)
        if (t == QLatin1String(kShapeKeys[i])) s.type = ShapeType(i);
    s.name = o.value("name").toString();
    s.pos = QPointF(o.value("x").toDouble(), o.value("y").toDouble());
    s.rot = o.value("rot").toDouble();
    s.w = o.value("w").toDouble(100);
    s.h = o.value("h").toDouble(100);
    s.fill = colorFrom(o.value("fill"), s.fill);
    s.stroke = colorFrom(o.value("stroke"), s.stroke);
    s.strokeWidth = o.value("sw").toDouble(2);
    s.opacity = o.value("op").toDouble(1);
    s.cornerRadius = o.value("cr").toDouble(0);
    s.sides = o.value("sides").toInt(5);
    s.innerRatio = o.value("inner").toDouble(0.45);
    s.text = o.value("text").toString();
    s.fontFamily = o.value("font").toString();
    s.fontSize = o.value("fs").toDouble(32);
    s.bold = o.value("bold").toBool();
    s.italic = o.value("it").toBool();
    s.align = o.value("al").toInt(0);
    s.group = o.value("grp").toInt(0);
    s.imageKey = o.value("img").toString();
    s.lockAspect = o.value("lock").toBool();
    s.squash = qBound(0.05, o.value("sq").toDouble(1), 20.0);
    s.squashAngle = o.value("sqa").toDouble();
    const QJsonArray wa = o.value("warp").toArray();
    if (wa.size() == 8)
        for (int k = 0; k < 4; ++k) s.warp[k] = QPointF(wa[2 * k].toDouble(), wa[2 * k + 1].toDouble());
    s.lineStyle = o.value("ls").toInt(0);
    s.amplitude = o.value("amp").toDouble(8);
    s.accent = colorFrom(o.value("acc"), s.accent);
    s.ends[0] = linkFromJson(o.value("endA"));
    s.ends[1] = linkFromJson(o.value("endB"));
    return s;
}

QJsonObject jointToJson(const Joint& j)
{
    QJsonObject o;
    o["id"] = j.id;
    o["p"] = j.parent;
    o["c"] = j.child;
    o["type"] = kJointKeys[int(j.type)];
    o["apx"] = j.anchorP.x();
    o["apy"] = j.anchorP.y();
    o["acx"] = j.anchorC.x();
    o["acy"] = j.anchorC.y();
    o["rest"] = j.restAngle;
    o["lim"] = j.limited;
    o["min"] = j.minAngle;
    o["max"] = j.maxAngle;
    if (j.type == JointType::Elastic) o["k"] = j.stiffness;
    return o;
}

Joint jointFromJson(const QJsonObject& o)
{
    Joint j;
    j.id = o.value("id").toInt();
    j.parent = o.value("p").toInt();
    j.child = o.value("c").toInt();
    const QString t = o.value("type").toString();
    for (int i = 0; i < 4; ++i)
        if (t == QLatin1String(kJointKeys[i])) j.type = JointType(i);
    j.stiffness = qBound(0.05, o.value("k").toDouble(0.3), 1.0);
    j.anchorP = QPointF(o.value("apx").toDouble(), o.value("apy").toDouble());
    j.anchorC = QPointF(o.value("acx").toDouble(), o.value("acy").toDouble());
    j.restAngle = o.value("rest").toDouble();
    j.limited = o.value("lim").toBool();
    j.minAngle = o.value("min").toDouble(-45);
    j.maxAngle = o.value("max").toDouble(45);
    return j;
}
}  // namespace

// ---------------------------------------------------------------- Shape

bool Shape::hasWarp() const
{
    for (const QPointF& p : warp)
        if (!p.isNull()) return true;
    return false;
}

QPointF Shape::cornerBase(int k) const
{
    static const int sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
    return QPointF(sx[k] * w / 2, sy[k] * h / 2);
}

// Row-vector convention: A * B applies A first.
static QTransform squashRotation(const Shape& s)
{
    QTransform t;
    if (s.squash != 1.0 && s.squash > 0) {
        QTransform a, b;
        a.rotate(-s.squashAngle);
        b.rotate(s.squashAngle);
        t = a * QTransform::fromScale(s.squash, 1.0 / s.squash) * b;
    }
    QTransform r;
    r.rotate(s.rot);
    return t * r;
}

QTransform Shape::localTransform() const
{
    QTransform t;
    if (hasWarp() && w > 1e-6 && h > 1e-6) {
        QPolygonF from, to;
        for (int k = 0; k < 4; ++k) {
            from << cornerBase(k);
            to << cornerBase(k) + QPointF(warp[k].x() * w, warp[k].y() * h);
        }
        QTransform q;
        if (QTransform::quadToQuad(from, to, q)) t = q;
    }
    return t * squashRotation(*this);
}

QTransform Shape::outerTransform() const
{
    return squashRotation(*this) * QTransform::fromTranslate(pos.x(), pos.y());
}

QTransform Shape::transform() const { return localTransform() * QTransform::fromTranslate(pos.x(), pos.y()); }

QPointF Shape::toWorld(QPointF l) const
{
    if (deformed()) return transform().map(l);
    const double r = qDegreesToRadians(rot), c = std::cos(r), s = std::sin(r);
    return pos + QPointF(l.x() * c - l.y() * s, l.x() * s + l.y() * c);
}

QPointF Shape::toLocal(QPointF w) const
{
    if (deformed()) return transform().inverted().map(w);
    const double r = qDegreesToRadians(rot), c = std::cos(r), s = std::sin(r);
    const QPointF v = w - pos;
    return QPointF(v.x() * c + v.y() * s, -v.x() * s + v.y() * c);
}

bool operator==(const Shape& a, const Shape& b)
{
    for (int k = 0; k < 4; ++k)
        if (a.warp[k] != b.warp[k]) return false;
    for (int k = 0; k < 2; ++k)
        if (a.ends[k].target != b.ends[k].target || a.ends[k].anchor != b.ends[k].anchor) return false;
    return a.id == b.id && a.type == b.type && a.pos == b.pos && a.rot == b.rot && a.w == b.w &&
           a.h == b.h && a.fill == b.fill && a.stroke == b.stroke &&
           a.strokeWidth == b.strokeWidth && a.opacity == b.opacity &&
           a.cornerRadius == b.cornerRadius && a.sides == b.sides &&
           a.innerRatio == b.innerRatio && a.fontSize == b.fontSize && a.bold == b.bold &&
           a.italic == b.italic && a.align == b.align && a.group == b.group &&
           a.lockAspect == b.lockAspect && a.squash == b.squash && a.squashAngle == b.squashAngle &&
           a.lineStyle == b.lineStyle && a.amplitude == b.amplitude && a.accent == b.accent &&
           a.name == b.name && a.text == b.text && a.fontFamily == b.fontFamily &&
           a.imageKey == b.imageKey;
}

// ---------------------------------------------------------------- Frame

int Frame::indexOf(int id) const
{
    for (int i = 0; i < shapes.size(); ++i)
        if (shapes.at(i).id == id) return i;
    return -1;
}
Shape* Frame::find(int id)
{
    const int i = indexOf(id);
    return i < 0 ? nullptr : &shapes[i];
}
const Shape* Frame::find(int id) const
{
    const int i = indexOf(id);
    return i < 0 ? nullptr : &shapes.at(i);
}
int Frame::jointIndex(int id) const
{
    for (int i = 0; i < joints.size(); ++i)
        if (joints.at(i).id == id) return i;
    return -1;
}
int Frame::parentJointIndex(int child) const
{
    for (int i = 0; i < joints.size(); ++i)
        if (joints.at(i).child == child) return i;
    return -1;
}

// ---------------------------------------------------------------- Document I/O

QJsonObject Document::toJson() const
{
    QJsonObject o;
    o["format"] = "tood";
    o["version"] = 2;
    o["width"] = width;
    o["height"] = height;
    o["bg"] = colorStr(background);
    o["fps"] = fps;
    o["nextId"] = nextId;
    o["activeChapter"] = activeChapter;

    QSet<QString> usedImages;
    QJsonArray ca;
    for (int c = 0; c < chapters.size(); ++c) {
        QJsonArray fa;
        for (const Frame& f : chapterFrames(c)) {
            QJsonObject fo;
            fo["dur"] = f.durationMs;
            QJsonArray sa, ja;
            for (const Shape& s : f.shapes) {
                sa.append(shapeToJson(s));
                if (s.type == ShapeType::Image) usedImages.insert(s.imageKey);
            }
            for (const Joint& j : f.joints) ja.append(jointToJson(j));
            fo["shapes"] = sa;
            fo["joints"] = ja;
            fa.append(fo);
        }
        QJsonObject co;
        co["name"] = chapters.at(c).name;
        co["frames"] = fa;
        ca.append(co);
    }
    o["chapters"] = ca;

    QJsonObject io;  // only images still referenced (undo history may hold more in memory)
    for (const QString& key : usedImages) {
        auto it = images.constFind(key);
        if (it == images.constEnd()) continue;
        QByteArray ba;
        QBuffer buf(&ba);
        buf.open(QIODevice::WriteOnly);
        it.value().save(&buf, "PNG");
        io[key] = QString::fromLatin1(ba.toBase64());
    }
    o["images"] = io;
    return o;
}

static QVector<Frame> framesFromJson(const QJsonArray& arr, int& maxId)
{
    QVector<Frame> out;
    for (const QJsonValue& fv : arr) {
        const QJsonObject fo = fv.toObject();
        Frame f;
        f.durationMs = qMax(10, fo.value("dur").toInt(100));
        for (const QJsonValue& sv : fo.value("shapes").toArray()) {
            f.shapes.append(shapeFromJson(sv.toObject()));
            maxId = qMax(maxId, qMax(f.shapes.constLast().id, f.shapes.constLast().group));
        }
        for (const QJsonValue& jv : fo.value("joints").toArray()) {
            Joint j = jointFromJson(jv.toObject());
            if (f.indexOf(j.parent) < 0 || f.indexOf(j.child) < 0) continue;
            f.joints.append(j);
            maxId = qMax(maxId, j.id);
        }
        out.append(f);
    }
    if (out.isEmpty()) out.append(Frame());
    return out;
}

bool Document::fromJson(const QJsonObject& o, Document& d, QString* err)
{
    if (o.value("format").toString() != QLatin1String("tood")) {
        if (err) *err = QObject::tr("Not a TooD document.");
        return false;
    }
    d = Document();
    d.chapters.clear();
    d.width = qMax(1, o.value("width").toInt(800));
    d.height = qMax(1, o.value("height").toInt(600));
    d.background = colorFrom(o.value("bg"), Qt::white);
    d.fps = qBound(1, o.value("fps").toInt(12), 120);

    const QJsonObject io = o.value("images").toObject();
    for (auto it = io.begin(); it != io.end(); ++it) {
        QImage im;
        im.loadFromData(QByteArray::fromBase64(it.value().toString().toLatin1()), "PNG");
        d.images.insert(it.key(), im.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }

    int maxId = 0;
    if (o.contains("chapters")) {
        for (const QJsonValue& cv : o.value("chapters").toArray()) {
            const QJsonObject co = cv.toObject();
            d.chapters.append(Chapter{co.value("name").toString(), framesFromJson(co.value("frames").toArray(), maxId)});
        }
    } else {  // version 1: a single list of frames
        d.chapters.append(Chapter{QObject::tr("Chapter 1"), framesFromJson(o.value("frames").toArray(), maxId)});
    }
    if (d.chapters.isEmpty()) d.chapters.append(Chapter{QObject::tr("Chapter 1"), {Frame()}});
    d.activeChapter = qBound(0, o.value("activeChapter").toInt(0), int(d.chapters.size()) - 1);
    d.frames = d.chapters[d.activeChapter].frames;
    d.chapters[d.activeChapter].frames.clear();
    d.nextId = qMax(o.value("nextId").toInt(1), maxId + 1);
    return true;
}

bool Document::save(const QString& path, QString* err) const
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    f.write(QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
    if (!f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}

bool Document::load(const QString& path, Document& out, QString* err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument jd = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError) {
        if (err) *err = pe.errorString();
        return false;
    }
    return fromJson(jd.object(), out, err);
}

// ---------------------------------------------------------------- Rendering

QString shapeTypeName(ShapeType t)
{
    static const char* n[] = {"Rectangle", "Ellipse", "Triangle", "Polygon", "Star",  "Line", "Arrow",
                              "Text",      "Image",   "Hourglass", "Diamond", "Eye",  "Point"};
    return QString::fromLatin1(n[int(t)]);
}

QString jointTypeName(JointType t)
{
    static const char* n[] = {"Hinge (rotate only)", "Free (swing)", "Weld (rigid)", "Elastic (rubber band)"};
    return QString::fromLatin1(n[int(t)]);
}

QFont shapeFont(const Shape& s)
{
    QFont f(s.fontFamily.isEmpty() ? QStringLiteral("Sans Serif") : s.fontFamily);
    f.setPixelSize(qMax(1, qRound(s.fontSize)));
    f.setBold(s.bold);
    f.setItalic(s.italic);
    return f;
}

void fitTextShape(Shape& s)
{
    const QFontMetricsF fm(shapeFont(s));
    const QRectF br = fm.boundingRect(QRectF(0, 0, 1e5, 1e5), Qt::AlignCenter,
                                      s.text.isEmpty() ? QStringLiteral(" ") : s.text);
    s.w = br.width() + 12;
    s.h = br.height() + 6;
}

double arrowHeadLength(const Shape& s) { return qMin(s.w * 0.5, qMax(10.0, s.strokeWidth * 4)); }

QPainterPath shapePath(const Shape& s)
{
    QPainterPath p;
    const QRectF r = s.localRect();
    switch (s.type) {
    case ShapeType::Rect:
        if (s.cornerRadius > 0)
            p.addRoundedRect(r, s.cornerRadius, s.cornerRadius);
        else
            p.addRect(r);
        break;
    case ShapeType::Ellipse: p.addEllipse(r); break;
    case ShapeType::Triangle:
        p.moveTo(0, r.top());
        p.lineTo(r.right(), r.bottom());
        p.lineTo(r.left(), r.bottom());
        p.closeSubpath();
        break;
    case ShapeType::Polygon:
    case ShapeType::Star: {
        const int n = qMax(3, s.sides);
        const bool star = s.type == ShapeType::Star;
        const int cnt = star ? 2 * n : n;
        QPolygonF poly;
        poly.reserve(cnt);
        for (int i = 0; i < cnt; ++i) {
            const double a = -M_PI / 2 + 2 * M_PI * i / cnt;
            const double k = (star && (i & 1)) ? s.innerRatio : 1.0;
            poly << QPointF(std::cos(a) * s.w / 2 * k, std::sin(a) * s.h / 2 * k);
        }
        p.addPolygon(poly);
        p.closeSubpath();
        break;
    }
    case ShapeType::Hourglass: {
        const double iw = s.innerRatio * s.w / 2;  // waist half-width
        p.moveTo(r.topLeft());
        p.lineTo(r.topRight());
        p.lineTo(iw, 0);
        p.lineTo(r.bottomRight());
        p.lineTo(r.bottomLeft());
        p.lineTo(-iw, 0);
        p.closeSubpath();
        break;
    }
    case ShapeType::Diamond: {  // regular n-gon with every pair of corners connected
        const int n = qBound(3, s.sides, 40);
        QPolygonF v;
        for (int i = 0; i < n; ++i) {
            const double a = -M_PI / 2 + 2 * M_PI * i / n;
            v << QPointF(std::cos(a) * s.w / 2, std::sin(a) * s.h / 2);
        }
        p.addPolygon(v);
        p.closeSubpath();
        for (int i = 0; i < n; ++i)
            for (int j = i + 2; j < n; ++j) {
                if (i == 0 && j == n - 1) continue;
                p.moveTo(v[i]);
                p.lineTo(v[j]);
            }
        break;
    }
    case ShapeType::Eye:  // almond: two quadratic arcs between the corners
        p.moveTo(-s.w / 2, 0);
        p.quadTo(0, -s.h, s.w / 2, 0);
        p.quadTo(0, s.h, -s.w / 2, 0);
        p.closeSubpath();
        break;
    case ShapeType::Point: p.addEllipse(r); break;
    case ShapeType::Line:
    case ShapeType::Arrow: return linePath(s, -s.w / 2, s.w / 2);
    default: p.addRect(r);
    }
    return p;
}

// Spring: the number of coils is fixed, so stretching the line widens the zigzag.
QPainterPath linePath(const Shape& s, double x0, double x1)
{
    QPainterPath p;
    p.moveTo(x0, 0);
    const double L = x1 - x0;
    if (s.lineStyle == 1 && L > 4) {
        const double lead = qMin(12.0, L * 0.1);
        const int n = qBound(1, s.sides, 200) * 2;
        const double step = (L - 2 * lead) / n;
        p.lineTo(x0 + lead, 0);
        for (int i = 0; i < n; ++i)
            p.lineTo(x0 + lead + (i + 0.5) * step, (i & 1) ? s.amplitude : -s.amplitude);
        p.lineTo(x1 - lead, 0);
    }
    p.lineTo(x1, 0);
    return p;
}

void paintShapeLocal(QPainter* p, const Shape& s, const QImage* img)
{
    QPen pen(Qt::NoPen);
    if (s.strokeWidth > 0 && s.stroke.alpha() > 0)
        pen = QPen(s.stroke, s.strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    const QBrush brush = s.fill.alpha() > 0 ? QBrush(s.fill) : QBrush(Qt::NoBrush);
    const QRectF r = s.localRect();

    switch (s.type) {
    case ShapeType::Image:
        if (img && !img->isNull())
            p->drawImage(r, *img);
        else
            p->fillRect(r, QColor(210, 210, 210));
        if (pen.style() != Qt::NoPen) {
            p->setPen(pen);
            p->setBrush(Qt::NoBrush);
            p->drawRect(r);
        }
        break;
    case ShapeType::Text:
        p->save();
        p->setClipRect(r, Qt::IntersectClip);
        p->setFont(shapeFont(s));
        p->setPen(s.fill);
        p->drawText(r, (s.align == 1 ? Qt::AlignLeft : s.align == 2 ? Qt::AlignRight : Qt::AlignHCenter) |
                           Qt::AlignVCenter | Qt::TextWordWrap, s.text);
        p->restore();
        break;
    case ShapeType::Line:
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        p->drawPath(linePath(s, -s.w / 2, s.w / 2));
        break;
    case ShapeType::Arrow: {
        const double hl = arrowHeadLength(s), hw = hl * 0.6;
        QPen lp = pen;
        lp.setCapStyle(Qt::FlatCap);
        p->setPen(lp);
        p->setBrush(Qt::NoBrush);
        p->drawPath(linePath(s, -s.w / 2, s.w / 2 - hl * 0.8));
        QPolygonF head;
        head << QPointF(s.w / 2, 0) << QPointF(s.w / 2 - hl, -hw) << QPointF(s.w / 2 - hl, hw);
        p->setPen(Qt::NoPen);
        p->setBrush(s.stroke);
        p->drawPolygon(head);
        break;
    }
    case ShapeType::Point:
        p->setPen(QPen(QColor(255, 255, 255), 1.5));
        p->setBrush(QColor(230, 0, 140));
        p->drawEllipse(r);
        break;
    case ShapeType::Eye: {
        const QPainterPath path = shapePath(s);
        p->setPen(pen);
        p->setBrush(brush);
        p->drawPath(path);
        if (s.innerRatio > 0) {  // iris
            const double ir = s.innerRatio * s.h / 2;
            p->save();
            p->setClipPath(path);
            p->setPen(Qt::NoPen);
            p->setBrush(s.accent);
            p->drawEllipse(QPointF(0, 0), ir, ir);
            p->restore();
        }
        break;
    }
    default:
        p->setPen(pen);
        p->setBrush(brush);
        p->drawPath(shapePath(s));
    }
}

QString snapKindName(SnapKind k)
{
    switch (k) {
    case SnapKind::Corner: return QObject::tr("corner");
    case SnapKind::Midpoint: return QObject::tr("midpoint");
    case SnapKind::Centre: return QObject::tr("centre");
    case SnapKind::Edge: return QObject::tr("on edge");
    case SnapKind::Grid: return QObject::tr("grid");
    default: return QString();
    }
}

QVector<SnapPoint> snapPoints(const Shape& s)
{
    QVector<SnapPoint> v;
    const double w = s.w / 2, h = s.h / 2;
    auto add = [&v](double x, double y, SnapKind k) { v.append(SnapPoint{QPointF(x, y), k}); };
    auto corners = [&](double x, double y) {
        add(-x, -y, SnapKind::Corner); add(x, -y, SnapKind::Corner);
        add(x, y, SnapKind::Corner);   add(-x, y, SnapKind::Corner);
    };
    switch (s.type) {
    case ShapeType::Line:
    case ShapeType::Arrow:
        add(-w, 0, SnapKind::Corner);
        add(w, 0, SnapKind::Corner);
        add(0, 0, SnapKind::Midpoint);
        return v;  // the centre of a line is its midpoint
    case ShapeType::Point: add(0, 0, SnapKind::Centre); return v;
    case ShapeType::Ellipse:  // quadrant points
        add(0, -h, SnapKind::Midpoint); add(w, 0, SnapKind::Midpoint);
        add(0, h, SnapKind::Midpoint);  add(-w, 0, SnapKind::Midpoint);
        break;
    case ShapeType::Eye:
        add(-w, 0, SnapKind::Corner); add(w, 0, SnapKind::Corner);
        add(0, -h / 2, SnapKind::Midpoint); add(0, h / 2, SnapKind::Midpoint);
        break;
    case ShapeType::Triangle:
        add(0, -h, SnapKind::Corner); add(w, h, SnapKind::Corner); add(-w, h, SnapKind::Corner);
        add(w / 2, 0, SnapKind::Midpoint); add(-w / 2, 0, SnapKind::Midpoint); add(0, h, SnapKind::Midpoint);
        break;
    case ShapeType::Hourglass:
        corners(w, h);
        add(0, -h, SnapKind::Midpoint); add(0, h, SnapKind::Midpoint);
        add(s.innerRatio * w, 0, SnapKind::Corner); add(-s.innerRatio * w, 0, SnapKind::Corner);
        break;
    case ShapeType::Polygon:
    case ShapeType::Star:
    case ShapeType::Diamond: {
        const int n = qBound(3, s.sides, 64);
        const bool star = s.type == ShapeType::Star;
        const int cnt = star ? 2 * n : n;
        QPolygonF poly;
        for (int i = 0; i < cnt; ++i) {
            const double a = -M_PI / 2 + 2 * M_PI * i / cnt;
            const double k = (star && (i & 1)) ? s.innerRatio : 1.0;
            poly << QPointF(std::cos(a) * w * k, std::sin(a) * h * k);
        }
        for (const QPointF& p : poly) add(p.x(), p.y(), SnapKind::Corner);
        if (!star)
            for (int i = 0; i < n; ++i) {
                const QPointF m = (poly[i] + poly[(i + 1) % n]) / 2;
                add(m.x(), m.y(), SnapKind::Midpoint);
            }
        break;
    }
    default:  // rect, text, image
        corners(w, h);
        add(0, -h, SnapKind::Midpoint); add(w, 0, SnapKind::Midpoint);
        add(0, h, SnapKind::Midpoint);  add(-w, 0, SnapKind::Midpoint);
    }
    add(0, 0, SnapKind::Centre);
    return v;
}

void renderFrame(QPainter* p, const Document& d, const Frame& f, bool background)
{
    if (background) p->fillRect(QRectF(0, 0, d.width, d.height), d.background);
    const double base = p->opacity();
    for (const Shape& s : f.shapes) {
        if (s.opacity <= 0 || s.type == ShapeType::Point) continue;
        p->save();
        p->setTransform(s.transform(), true);
        p->setOpacity(base * s.opacity);
        const QImage* img = nullptr;
        if (s.type == ShapeType::Image) {
            auto it = d.images.constFind(s.imageKey);
            if (it != d.images.constEnd()) img = &it.value();
        }
        paintShapeLocal(p, s, img);
        p->restore();
    }
}

QImage renderFrameImage(const Document& d, const Frame& f, double scale, bool background,
                        const QColor& base)
{
    const QSize sz(qMax(1, qRound(d.width * scale)), qMax(1, qRound(d.height * scale)));
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(base);
    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform |
                     QPainter::TextAntialiasing);
    p.scale(scale, scale);
    renderFrame(&p, d, f, background);
    p.end();
    return img;
}
