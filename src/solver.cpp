#include "solver.h"

#include <QtMath>
#include <cmath>
#include <functional>
#include <utility>

namespace Solver {

namespace {
QPointF rotateVec(QPointF v, double deg)
{
    const double r = qDegreesToRadians(deg), c = std::cos(r), s = std::sin(r);
    return QPointF(v.x() * c - v.y() * s, v.x() * s + v.y() * c);
}
double len(QPointF v) { return std::hypot(v.x(), v.y()); }

double clampHinge(const Frame& f, const DragInfo& di, double d)
{
    if (di.joint < 0) return d;
    const Joint& j = f.joints.at(di.joint);
    if (j.type != JointType::Hinge || !j.limited) return d;
    const Shape* r = f.find(di.root);
    const Shape* p = f.find(j.parent);
    if (!r || !p) return d;
    const double lo = qMin(j.minAngle, j.maxAngle), hi = qMax(j.minAngle, j.maxAngle);
    const double rel = normDeg(r->rot - p->rot - j.restAngle);
    return qBound(lo, rel + d, hi) - rel;
}

struct Spring {
    QHash<int, QPointF>* vel = nullptr;
    double dt = 0;
};

// Breadth-first constraint propagation from the seeds. Shapes in `fixed` are
// never moved. Elastic edges: exact (lag=false), partial (lag=true) or
// integrated as a damped spring (spring.vel != nullptr).
void propagateCore(Frame& f, const QHash<int, double>& seeds, bool lag, const QSet<int>* fixed,
                   const Spring& spring = {})
{
    if (f.joints.isEmpty()) return;
    QHash<int, int> idx;
    idx.reserve(f.shapes.size());
    for (int i = 0; i < f.shapes.size(); ++i) idx.insert(f.shapes.at(i).id, i);
    QHash<int, QVector<int>> adj;
    const Joint* J = f.joints.constData();
    for (int i = 0; i < f.joints.size(); ++i) {
        adj[J[i].parent].append(i);
        adj[J[i].child].append(i);
    }

    QHash<int, double> dth = seeds;
    QVector<int> queue;
    queue.reserve(f.shapes.size());
    for (auto it = seeds.cbegin(); it != seeds.cend(); ++it) queue.append(it.key());
    QSet<int> visited(queue.cbegin(), queue.cend());
    if (fixed) visited.unite(*fixed);

    Shape* S = f.shapes.data();
    for (int qi = 0; qi < queue.size(); ++qi) {
        const int u = queue.at(qi);
        const int iu = idx.value(u, -1);
        if (iu < 0) continue;
        const Shape& su = S[iu];
        const auto edges = adj.value(u);
        for (int ji : edges) {
            const Joint& j = J[ji];
            const bool uIsParent = j.parent == u;
            const int v = uIsParent ? j.child : j.parent;
            if (visited.contains(v)) continue;
            const int iv = idx.value(v, -1);
            if (iv < 0) continue;
            Shape& sv = S[iv];
            const QPointF target = anchorWorld(su, uIsParent ? j.anchorP : j.anchorC);
            const QPointF va = uIsParent ? j.anchorC : j.anchorP;
            double d = 0;
            if (j.type == JointType::Elastic) {
                const QPointF gap = target - anchorWorld(sv, va);
                QPointF mv = gap;
                if (spring.vel) {
                    QPointF& vel = (*spring.vel)[j.id];
                    const double k = 160.0 * (0.25 + j.stiffness);
                    const double c = 2.0 * 0.28 * std::sqrt(k);  // under-damped: a little bounce
                    vel += (gap * k - vel * c) * spring.dt;
                    mv = vel * spring.dt;
                    if (len(gap) < 0.3 && len(vel) < 4) {
                        mv = gap;
                        vel = QPointF();
                    }
                } else if (lag) {
                    mv = gap * j.stiffness;
                }
                sv.pos += mv;
            } else {
                switch (j.type) {
                case JointType::Weld: d = dth.value(u); break;
                case JointType::Hinge: d = uIsParent ? dth.value(u) : 0.0; break;
                case JointType::Free: {
                    const QPointF a = anchorWorld(sv, va);
                    const QPointF ca = sv.pos - a, cp = sv.pos - target;
                    if (len(ca) > 1e-3 && len(cp) > 1e-3) d = normDeg(angleDeg(cp) - angleDeg(ca));
                    break;
                }
                default: break;
                }
                sv.rot += d;
                sv.pos += target - anchorWorld(sv, va);
            }
            visited.insert(v);
            dth.insert(v, d);
            queue.append(v);
        }
    }
}

// Re-glue connectors, then let shapes jointed to a connector follow it
// (without moving the connector's targets or the original seeds).
void settleStretch(Frame& f, const QList<int>& seeds)
{
    const QVector<int> changed = updateStretch(f);
    if (changed.isEmpty() || f.joints.isEmpty()) return;
    QSet<int> fixed(seeds.cbegin(), seeds.cend());
    for (const Shape& s : std::as_const(f.shapes))
        for (const EndLink& l : s.ends)
            if (l.target) fixed.insert(l.target);
    QHash<int, double> sd;
    for (int id : changed) {
        fixed.remove(id);
        sd.insert(id, 0.0);
    }
    propagateCore(f, sd, false, &fixed);
}

QColor lerpColor(const QColor& a, const QColor& b, double t)
{
    auto l = [t](int x, int y) { return qBound(0, qRound(x + (y - x) * t), 255); };
    return QColor(l(a.red(), b.red()), l(a.green(), b.green()), l(a.blue(), b.blue()),
                  l(a.alpha(), b.alpha()));
}

Shape lerpShape(const Shape& a, const Shape& b, double t)
{
    auto l = [t](double x, double y) { return x + (y - x) * t; };
    Shape s = t < 0.5 ? a : b;  // discrete properties switch half-way
    s.id = a.id;
    s.type = a.type;
    s.pos = QPointF(l(a.pos.x(), b.pos.x()), l(a.pos.y(), b.pos.y()));
    s.rot = l(a.rot, b.rot);
    s.w = l(a.w, b.w);
    s.h = l(a.h, b.h);
    s.fill = lerpColor(a.fill, b.fill, t);
    s.stroke = lerpColor(a.stroke, b.stroke, t);
    s.accent = lerpColor(a.accent, b.accent, t);
    s.bubbleFill = lerpColor(a.bubbleFill, b.bubbleFill, t);
    s.tail = a.tail + (b.tail - a.tail) * t;
    s.strokeWidth = l(a.strokeWidth, b.strokeWidth);
    s.opacity = l(a.opacity, b.opacity);
    s.cornerRadius = l(a.cornerRadius, b.cornerRadius);
    s.innerRatio = l(a.innerRatio, b.innerRatio);
    s.fontSize = l(a.fontSize, b.fontSize);
    s.sides = qRound(l(a.sides, b.sides));
    s.amplitude = l(a.amplitude, b.amplitude);
    s.squash = l(a.squash, b.squash);
    s.squashAngle = (a.squash == 1.0) ? b.squashAngle : (b.squash == 1.0) ? a.squashAngle : l(a.squashAngle, b.squashAngle);
    for (int k = 0; k < 4; ++k) s.warp[k] = a.warp[k] + (b.warp[k] - a.warp[k]) * t;
    if (a.pts.size() == b.pts.size() && !a.pts.isEmpty()) {  // same node count: morph the path
        s.pts = a.pts;
        for (int i = 0; i < s.pts.size(); ++i) s.pts[i] = a.pts[i] + (b.pts[i] - a.pts[i]) * t;
    }
    return s;
}
}  // namespace

QPointF anchorWorld(const Shape& s, QPointF n) { return s.toWorld(QPointF(n.x() * s.w, n.y() * s.h)); }

QPointF normalizeLocal(const Shape& s, QPointF l)
{
    return QPointF(s.w > 1e-6 ? l.x() / s.w : 0.0, s.h > 1e-6 ? l.y() / s.h : 0.0);
}

double normDeg(double a)
{
    a = std::fmod(a + 180.0, 360.0);
    if (a <= 0) a += 360.0;
    return a - 180.0;
}

double angleDeg(QPointF v) { return qRadiansToDegrees(std::atan2(v.y(), v.x())); }

void propagate(Frame& f, const QHash<int, double>& seeds, bool elasticLag)
{
    propagateCore(f, seeds, elasticLag, nullptr);
    settleStretch(f, seeds.keys());
}

QVector<int> updateStretch(Frame& f)
{
    QVector<int> changed;
    bool any = false;
    for (const Shape& s : std::as_const(f.shapes))
        if (s.hasLinks()) {
            any = true;
            break;
        }
    if (!any) return changed;
    QHash<int, int> idx;
    for (int i = 0; i < f.shapes.size(); ++i) idx.insert(f.shapes.at(i).id, i);
    Shape* S = f.shapes.data();
    for (int pass = 0; pass < 3; ++pass) {  // connectors glued to connectors settle in a few passes
        bool moved = false;
        for (int i = 0; i < f.shapes.size(); ++i) {
            Shape& s = S[i];
            if (!s.hasLinks()) continue;
            QPointF e[2] = {s.toWorld({-s.w / 2, 0}), s.toWorld({s.w / 2, 0})};
            for (int k = 0; k < 2; ++k) {
                const int ti = s.ends[k].target ? idx.value(s.ends[k].target, -1) : -1;
                if (ti >= 0 && ti != i) e[k] = anchorWorld(S[ti], s.ends[k].anchor);
            }
            const QPointF pos = (e[0] + e[1]) / 2;
            const double w = qMax(1.0, len(e[1] - e[0]));
            const double rot = s.rot + normDeg(angleDeg(e[1] - e[0]) - s.rot);
            if (len(pos - s.pos) > 1e-7 || std::abs(w - s.w) > 1e-7 || std::abs(rot - s.rot) > 1e-7) {
                s.pos = pos;
                s.w = w;
                s.rot = rot;
                moved = true;
                if (!changed.contains(s.id)) changed << s.id;
            }
        }
        if (!moved) break;
    }
    return changed;
}

void enforce(Frame& f, int id)
{
    const int ji = f.parentJointIndex(id);
    if (ji >= 0) {
        const Joint j = f.joints.at(ji);
        Shape* s = f.find(id);
        const Shape* p = f.find(j.parent);
        if (s && p) s->pos += anchorWorld(*p, j.anchorP) - anchorWorld(*s, j.anchorC);
    }
    propagate(f, {{id, 0.0}});
}

DragInfo dragInfo(const Frame& f, int id)
{
    DragInfo di;
    int cur = id;
    for (int guard = 0; guard < 100000; ++guard) {
        const int ji = f.parentJointIndex(cur);
        if (ji < 0) break;
        const Joint& j = f.joints.at(ji);
        if (j.type == JointType::Weld) {
            cur = j.parent;
            continue;
        }
        di.joint = ji;
        di.root = cur;
        di.hinge = j.type == JointType::Hinge;
        if (const Shape* p = f.find(j.parent)) di.pivot = anchorWorld(*p, j.anchorP);
        break;
    }
    if (di.root < 0) di.root = cur;
    return di;
}

void rotateShape(Frame& f, int id, double d, bool lag)
{
    const DragInfo di = dragInfo(f, id);
    const Shape* cs = std::as_const(f).find(id);
    if (!cs) return;
    // rotate about the pin for hinge/free, about the own centre otherwise (elastic stretches instead)
    const bool pinned = di.joint >= 0 && f.joints.at(di.joint).type != JointType::Elastic;
    const QPointF pivot = pinned ? di.pivot : cs->pos;
    d = clampHinge(f, di, d);
    if (std::abs(d) < 1e-9) return;
    Shape* s = f.find(id);
    s->rot += d;
    s->pos = pivot + rotateVec(s->pos - pivot, d);
    propagate(f, {{id, d}}, lag);
}

void dragShape(Frame& f, int id, QPointF grabLocal, QPointF mouse, bool lag)
{
    const DragInfo di = dragInfo(f, id);
    const Shape* cs = std::as_const(f).find(id);
    if (!cs) return;
    const QPointF g = cs->toWorld(grabLocal);
    if (di.hinge) {
        rotateShape(f, id, normDeg(angleDeg(mouse - di.pivot) - angleDeg(g - di.pivot)), lag);
    } else {
        f.find(id)->pos += mouse - g;
        propagate(f, {{id, 0.0}}, lag);
    }
}

void translateShapes(Frame& f, const QVector<int>& ids, QPointF delta)
{
    QHash<int, double> seeds;
    for (int id : ids)
        if (Shape* s = f.find(id)) {
            s->pos += delta;
            seeds.insert(id, 0.0);
        }
    propagate(f, seeds);
}

bool hasElasticGap(const Frame& f, double tol)
{
    for (const Joint& j : f.joints) {
        if (j.type != JointType::Elastic) continue;
        const Shape* p = f.find(j.parent);
        const Shape* c = f.find(j.child);
        if (p && c && len(anchorWorld(*p, j.anchorP) - anchorWorld(*c, j.anchorC)) > tol) return true;
    }
    return false;
}

bool springStep(Frame& f, const QVector<int>& pinnedIn, QHash<int, QPointF>& vel, double dt)
{
    QHash<int, double> seeds;
    for (int id : pinnedIn)
        if (f.indexOf(id) >= 0) seeds.insert(id, 0.0);
    if (seeds.isEmpty())  // nothing pinned: hold the parent side of each joint
        for (const Joint& j : std::as_const(f.joints))
            if (j.type == JointType::Elastic) seeds.insert(j.parent, 0.0);
    propagateCore(f, seeds, false, nullptr, Spring{&vel, dt});
    settleStretch(f, seeds.keys());
    for (auto it = vel.cbegin(); it != vel.cend(); ++it)
        if (len(it.value()) > 4) return true;
    return hasElasticGap(f, 0.3);
}

QSet<int> component(const Frame& f, int id)
{
    QSet<int> seen{id};
    QVector<int> q{id};
    for (int i = 0; i < q.size(); ++i) {
        for (const Joint& j : f.joints) {
            int other = -1;
            if (j.parent == q[i]) other = j.child;
            else if (j.child == q[i]) other = j.parent;
            if (other >= 0 && !seen.contains(other)) {
                seen.insert(other);
                q.append(other);
            }
        }
    }
    return seen;
}

QSet<int> assembly(const Frame& f, const QSet<int>& ids)
{
    QHash<int, QVector<int>> adj;
    for (const Joint& j : f.joints) {
        adj[j.parent] << j.child;
        adj[j.child] << j.parent;
    }
    for (const Shape& s : f.shapes)
        for (const EndLink& l : s.ends)
            if (l.target) {
                adj[s.id] << l.target;
                adj[l.target] << s.id;
            }
    QSet<int> seen = ids;
    QVector<int> q(ids.cbegin(), ids.cend());
    for (int i = 0; i < q.size(); ++i)
        for (int o : adj.value(q[i]))
            if (!seen.contains(o)) {
                seen.insert(o);
                q << o;
            }
    return seen;
}

void reroot(Frame& f, int id)
{
    QVector<int> path;
    int cur = id;
    while (path.size() < 100000) {
        const int ji = f.parentJointIndex(cur);
        if (ji < 0) break;
        path << ji;
        cur = f.joints.at(ji).parent;
    }
    for (int ji : path) {
        Joint& j = f.joints[ji];
        std::swap(j.parent, j.child);
        std::swap(j.anchorP, j.anchorC);
        j.restAngle = -j.restAngle;
        const double mn = j.minAngle;
        j.minAngle = -j.maxAngle;
        j.maxAngle = -mn;
    }
}

bool connectShapes(Frame& f, int a, QPointF pa, int b, QPointF pb, JointType type, int jointId,
                   QString* why)
{
    auto fail = [why](const QString& m) {
        if (why) *why = m;
        return false;
    };
    if (a == b) return fail(QObject::tr("Cannot connect a shape to itself."));
    const Shape* sa = std::as_const(f).find(a);
    const Shape* sb = std::as_const(f).find(b);
    if (!sa || !sb) return fail(QObject::tr("Shape not found."));
    const QSet<int> compA = component(f, a);
    if (compA.contains(b))
        return fail(QObject::tr("These shapes are already connected (directly or via others). "
                                "Loops are not supported."));
    const QSet<int> compB = component(f, b);
    int rootB = b;  // top of b's assembly before connecting
    for (int ji = f.parentJointIndex(rootB), guard = 0; ji >= 0 && guard < 100000; ji = f.parentJointIndex(rootB), ++guard)
        rootB = f.joints.at(ji).parent;
    reroot(f, b);  // the child must not have another parent
    Joint j;
    j.id = jointId;
    j.parent = a;
    j.child = b;
    j.type = type;
    j.anchorP = normalizeLocal(*sa, sa->toLocal(pa));
    j.anchorC = normalizeLocal(*sb, sb->toLocal(pb));
    j.restAngle = sb->rot - sa->rot;
    f.joints.append(j);
    enforce(f, b);  // snap the child's point onto the parent's point
    // The larger assembly keeps its top (root): attaching a small part to a figure
    // must not make the small part the parent of the whole figure.
    if (compB.size() > compA.size()) reroot(f, rootB);
    return true;
}

void removeJointsOf(Frame& f, const QSet<int>& ids)
{
    f.joints.removeIf([&](const Joint& j) { return ids.contains(j.parent) || ids.contains(j.child); });
    for (int i = 0; i < f.shapes.size(); ++i) {
        const Shape& cs = f.shapes.at(i);
        if (!cs.hasLinks()) continue;
        if (ids.contains(cs.ends[0].target) || ids.contains(cs.ends[1].target)) {
            Shape& s = f.shapes[i];
            for (EndLink& l : s.ends)
                if (ids.contains(l.target)) l = EndLink();
        }
    }
}

double ease(double t, int mode)
{
    switch (mode) {
    case 1: return t * t * (3 - 2 * t);
    case 2: return t * t;
    case 3: return 1 - (1 - t) * (1 - t);
    case 4: return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
    case 5: {  // back-out: overshoot ~10% and settle
        const double c1 = 1.70158, c3 = c1 + 1, u = t - 1;
        return 1 + c3 * u * u * u + c1 * u * u;
    }
    case 6: {  // bounce-out
        const double n1 = 7.5625, d1 = 2.75;
        if (t < 1 / d1) return n1 * t * t;
        if (t < 2 / d1) { t -= 1.5 / d1; return n1 * t * t + 0.75; }
        if (t < 2.5 / d1) { t -= 2.25 / d1; return n1 * t * t + 0.9375; }
        t -= 2.625 / d1;
        return n1 * t * t + 0.984375;
    }
    default: return t;
    }
}

QStringList easeNames()
{
    return {QObject::tr("Linear (constant speed)"), QObject::tr("Ease in-out (accelerate, then decelerate)"),
            QObject::tr("Ease in (accelerate)"), QObject::tr("Ease out (decelerate)"),
            QObject::tr("Strong ease in-out"), QObject::tr("Overshoot (goes past, settles back)"),
            QObject::tr("Bounce at the end")};
}

QStringList pathNames()
{
    return {QObject::tr("Straight"), QObject::tr("Arc (curves sideways)"), QObject::tr("Thrown (gravity parabola)"),
            QObject::tr("Smooth through keyframes (spline)")};
}

Frame tween(const Frame& A, const Frame& B, double t, const TweenOptions& opt)
{
    Frame out;
    out.durationMs = A.durationMs;
    QHash<int, int> bi, oi;
    for (int i = 0; i < B.shapes.size(); ++i) bi.insert(B.shapes.at(i).id, i);
    QSet<int> jointed;
    for (const Joint& j : A.joints) jointed << j.parent << j.child;

    for (const Shape& sa : A.shapes) {
        const int k = bi.value(sa.id, -1);
        Shape s = k >= 0 ? lerpShape(sa, B.shapes.at(k), t) : sa;
        if (k >= 0 && opt.path == 3) {  // Catmull-Rom through the keyframe before a, a, b, and the one after b
            const QPointF p1 = sa.pos, p2 = B.shapes.at(k).pos;
            const Shape* s0 = opt.before ? opt.before->find(sa.id) : nullptr;
            const Shape* s3 = opt.after ? opt.after->find(sa.id) : nullptr;
            const QPointF p0 = s0 ? s0->pos : p1, p3 = s3 ? s3->pos : p2;
            const double t2 = t * t, t3 = t2 * t;
            s.pos = 0.5 * (2 * p1 + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (3 * p1 - p0 - 3 * p2 + p3) * t3);
        } else if (k >= 0 && opt.path != 0 && opt.arc != 0) {
            const QPointF pa = sa.pos, pb = B.shapes.at(k).pos;
            if (opt.path == 2) {  // thrown: constant horizontal speed, parabolic height (screen y grows downwards)
                const double u = opt.rawT >= 0 ? opt.rawT : t;
                s.pos = pa + (pb - pa) * u + QPointF(0, -opt.arc * 4 * u * (1 - u));
            } else {               // arc: bulge perpendicular to the straight path
                const QPointF d = pb - pa;
                const double l = len(d);
                if (l > 1e-6) s.pos += QPointF(-d.y() / l, d.x() / l) * (-opt.arc * 4 * t * (1 - t));
            }
        }
        if (k < 0) {
            s.opacity *= (1 - t);  // disappearing: fade out
        } else if (opt.squash > 0 && !jointed.contains(s.id) && !s.isLinear() && !s.hasLinks()) {
            // motion stretch: elongate along the direction of travel, area preserved
            const QPointF d = B.shapes.at(k).pos - sa.pos;
            const double speed = len(d) * opt.motionStep;  // px per frame
            const double st = 1.0 + opt.squash * qMin(1.0, speed / 40.0);
            if (st > 1.001) {
                s.squash *= st;
                s.squashAngle = angleDeg(d) - s.rot;
            }
        }
        oi.insert(s.id, out.shapes.size());
        out.shapes.append(s);
    }
    for (const Shape& sb : B.shapes) {
        if (oi.contains(sb.id)) continue;
        Shape s = sb;
        s.opacity *= t;  // appearing: fade in
        oi.insert(s.id, out.shapes.size());
        out.shapes.append(s);
    }
    out.joints = A.joints;

    // Forward kinematics over joints present in both frames.
    QHash<int, const Joint*> bJ;
    for (const Joint& j : B.joints) bJ.insert(j.id, &j);
    QHash<int, int> childJoint;  // child id -> index into A.joints
    for (int i = 0; i < A.joints.size(); ++i) {
        const Joint& ja = A.joints.at(i);
        const Joint* jb = bJ.value(ja.id);
        if (jb && jb->parent == ja.parent && jb->child == ja.child) childJoint.insert(ja.child, i);
    }

    QSet<int> done;
    std::function<void(int)> resolve = [&](int id) {
        if (done.contains(id)) return;
        done.insert(id);
        auto it = childJoint.constFind(id);
        if (it == childJoint.constEnd()) return;
        const int jIdx = it.value();
        const Joint& ja = A.joints.at(jIdx);
        const Joint& jb = *bJ.value(ja.id);
        resolve(ja.parent);
        const Shape *pa = A.find(ja.parent), *ca = A.find(id), *pb = B.find(ja.parent), *cb = B.find(id);
        const int pi = oi.value(ja.parent, -1), ci = oi.value(id, -1);
        if (!pa || !ca || !pb || !cb || pi < 0 || ci < 0) return;
        const double relA = ca->rot - pa->rot, relB = cb->rot - pb->rot;
        const Shape& P = out.shapes.at(pi);
        Shape& C = out.shapes[ci];
        C.rot = P.rot + relA + (relB - relA) * t;
        const QPointF aP = ja.anchorP + (jb.anchorP - ja.anchorP) * t;
        const QPointF aC = ja.anchorC + (jb.anchorC - ja.anchorC) * t;
        C.pos += anchorWorld(P, aP) - anchorWorld(C, aC);
        out.joints[jIdx].anchorP = aP;
        out.joints[jIdx].anchorC = aC;
    };
    if (!childJoint.isEmpty())
        for (int i = 0; i < out.shapes.size(); ++i) resolve(out.shapes.at(i).id);
    updateStretch(out);  // connectors follow their (interpolated) targets
    return out;
}

QVector<Frame> makeTweens(const Frame& a, const Frame& b, int n, int em, double squash, int durationMs, int path, double arc,
                          const Frame* before, const Frame* after)
{
    QVector<Frame> out;
    out.reserve(n);
    for (int k = 1; k <= n; ++k) {
        TweenOptions opt;
        opt.squash = squash;
        opt.motionStep = ease(qMin(1.0, (k + 0.5) / (n + 1)), em) - ease(qMax(0.0, (k - 0.5) / (n + 1)), em);
        opt.path = path;
        opt.arc = arc;
        opt.rawT = double(k) / (n + 1);
        opt.before = before;
        opt.after = after;
        Frame t = tween(a, b, ease(opt.rawT, em), opt);
        t.durationMs = durationMs;
        t.tweenCount = 0;
        out << t;
    }
    return out;
}

QVector<Frame> tweenFrames(const QVector<Frame>& frames, int i)
{
    if (i < 0 || i >= frames.size() || !frames.at(i).isTween()) return {};
    int a = i - 1, b = i + 1;
    while (a >= 0 && frames.at(a).isTween()) --a;
    while (b < frames.size() && frames.at(b).isTween()) ++b;
    if (a < 0 || b >= frames.size()) return {};
    int a0 = a - 1, b1 = b + 1;  // keyframes beyond, for the smooth path
    while (a0 >= 0 && frames.at(a0).isTween()) --a0;
    while (b1 < frames.size() && frames.at(b1).isTween()) ++b1;
    const Frame& t = frames.at(i);
    return makeTweens(frames.at(a), frames.at(b), t.tweenCount, t.tweenEase, t.tweenSquash, t.durationMs, t.tweenPath, t.tweenArc,
                      a0 >= 0 ? &frames.at(a0) : nullptr, b1 < frames.size() ? &frames.at(b1) : nullptr);
}

QVector<Frame> expandFrames(const QVector<Frame>& frames)
{
    QVector<Frame> out;
    out.reserve(frames.size());
    for (int i = 0; i < frames.size(); ++i) {
        if (frames.at(i).isTween()) out += tweenFrames(frames, i);
        else out << frames.at(i);
    }
    return out;
}

int frameSpanMs(const QVector<Frame>& frames, int i)
{
    const Frame& f = frames.at(i);
    return f.isTween() ? f.tweenCount * f.durationMs : f.durationMs;
}

}  // namespace Solver
