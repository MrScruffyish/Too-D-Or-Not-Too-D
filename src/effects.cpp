#include "effects.h"

#include "model.h"

#include <QPainter>
#include <QRadialGradient>
#include <QtMath>

#include <cmath>

namespace {

enum Preset { Explosion, Fire, Smoke, Shockwave, Confetti, Sparkles, StarBurst };

const char* const kNames[] = {"Explosion", "Fire", "Smoke", "Shockwave (radiating)", "Confetti", "Magic sparkles",
                              "Star burst (impact)"};

constexpr double kTau = 6.28318530717958647693;

// Stateless hash: same seed/particle/channel always yields the same value.
// This is what makes effects reproducible in tweens, undo and exported frames.
double rnd(int seed, int i, int ch)
{
    quint32 x = quint32(seed) * 747796405u + quint32(i + 1) * 2891336453u + quint32(ch + 1) * 277803737u;
    x ^= x >> 16;
    x *= 2246822519u;
    x ^= x >> 13;
    x *= 3266489917u;
    x ^= x >> 16;
    return (x & 0x00ffffff) / double(0x01000000);
}

double frac(double x) { return x - std::floor(x); }
double sat(double x) { return qBound(0.0, x, 1.0); }
double smooth(double x) { x = sat(x); return x * x * (3 - 2 * x); }

QColor withAlpha(const QColor& c, int a)
{
    QColor r = c;
    r.setAlpha(qBound(0, a, 255));
    return r;
}

QColor mix(const QColor& a, const QColor& b, double t)
{
    return QColor(qRound(a.red() + (b.red() - a.red()) * t), qRound(a.green() + (b.green() - a.green()) * t),
                  qRound(a.blue() + (b.blue() - a.blue()) * t),
                  qRound(a.alpha() + (b.alpha() - a.alpha()) * t));
}

int particles(const Shape& s) { return qBound(4, qRound(s.sides * s.effectDensity), 600); }
int seedOf(const Shape& s) { return s.effectSeed; }
double spread(const Shape& s) { return 0.7 + 0.6 * qBound(0.0, s.innerRatio, 1.0); }

void softBall(QPainter* p, QPointF c, double r, double ry, const QColor& col, int a)
{
    QRadialGradient g(c, r);
    g.setColorAt(0, withAlpha(col, a));
    g.setColorAt(0.55, withAlpha(col, int(a * 0.45)));
    g.setColorAt(1, withAlpha(col, 0));
    p->setPen(Qt::NoPen);
    p->setBrush(g);
    p->drawEllipse(QRectF(c.x() - r, c.y() - r * ry, 2 * r, 2 * r * ry));
}

void star4(QPainter* p, QPointF c, double r)
{
    QPainterPath path;
    path.moveTo(c.x(), c.y() - r);
    path.quadTo(c.x() + r * 0.14, c.y() - r * 0.14, c.x() + r, c.y());
    path.quadTo(c.x() + r * 0.14, c.y() + r * 0.14, c.x(), c.y() + r);
    path.quadTo(c.x() - r * 0.14, c.y() + r * 0.14, c.x() - r, c.y());
    path.quadTo(c.x() - r * 0.14, c.y() - r * 0.14, c.x(), c.y() - r);
    p->drawPath(path);
}

void starN(QPainter* p, QPointF c, double r, int points, double inner, double rot)
{
    QPolygonF poly;
    for (int i = 0; i < points * 2; ++i) {
        const double a = rot - M_PI / 2 + M_PI * i / points;
        const double k = (i & 1) ? inner : 1.0;
        poly << QPointF(c.x() + std::cos(a) * r * k, c.y() + std::sin(a) * r * k);
    }
    p->drawPolygon(poly);
}

QPointF sparkAt(const Shape& s, int i, double t, double R, double ay)
{
    const double a = kTau * rnd(seedOf(s), i, 0);
    const double speed = R * (0.65 + 1.45 * rnd(seedOf(s), i, 1)) * spread(s);
    const double u = 1 - std::pow(1 - sat(t), 2);  // fast burst, then drag
    return QPointF(std::cos(a) * speed * u, std::sin(a) * speed * u * ay + R * 0.6 * t * t);
}

void explosion(QPainter* p, const Shape& s, double t)
{
    const double R = qMin(s.w, s.h) * 0.5;
    const double ay = s.h / qMax(1.0, s.w);
    const int n = particles(s);

    // White flash and fireball.
    const double k = sat(t / 0.4);
    if (k < 1) {
        const double r = R * (0.25 + k) * spread(s);
        QRadialGradient g(QPointF(0, 0), r);
        const int a = qRound(245 * (1 - k) * (1 - k));
        g.setColorAt(0, withAlpha(QColor(255, 255, 230), a));
        g.setColorAt(0.35, withAlpha(s.fill, int(a * 0.95)));
        g.setColorAt(1, withAlpha(s.fill, 0));
        p->setPen(Qt::NoPen);
        p->setBrush(g);
        p->drawEllipse(QRectF(-r, -r * ay, 2 * r, 2 * r * ay));
    }

    // Expanding blast ring.
    {
        const double u = sat(t / 0.55);
        const double r = R * (0.15 + 1.5 * u) * spread(s);
        p->setPen(QPen(withAlpha(s.accent, qRound(180 * (1 - u) * (1 - u))), qMax(0.8, 6 * (1 - u))));
        p->setBrush(Qt::NoBrush);
        p->drawEllipse(QRectF(-r, -r * ay, 2 * r, 2 * r * ay));
    }

    // Sparks with short motion trails.
    for (int i = 0; i < n; ++i) {
        const QPointF q = sparkAt(s, i, t, R, ay);
        const QPointF tail = sparkAt(s, i, qMax(0.0, t - 0.07), R, ay);
        const int a = qRound(235 * std::pow(1 - t, 1.4) * (0.55 + 0.45 * rnd(seedOf(s), i, 2)));
        if (a <= 0) continue;
        const double thick = 1 + 2.4 * rnd(seedOf(s), i, 3);
        p->setPen(QPen(withAlpha(mix(s.accent, Qt::white, rnd(seedOf(s), i, 4) * 0.6), a), thick,
                       Qt::SolidLine, Qt::RoundCap));
        p->setBrush(Qt::NoBrush);
        p->drawLine(q, tail);
        p->setPen(Qt::NoPen);
        p->setBrush(withAlpha(s.accent, a));
        p->drawEllipse(q, thick * 0.75, thick * 0.75);
    }

    // Rising debris smoke.
    const QColor smoke = s.stroke.alpha() > 0 ? s.stroke : QColor(120, 120, 130);
    for (int i = 0; i < n / 3; ++i) {
        const double u = sat((t - 0.22) / 0.78);
        if (u <= 0 || u >= 1) continue;
        const double x = (rnd(seedOf(s), i, 5) - 0.5) * s.w * 0.7 + std::sin(u * 5 + i) * s.w * 0.04;
        const QPointF c(x, -R * 0.2 - u * s.h * 0.7 * spread(s));
        softBall(p, c, R * (0.12 + 0.4 * u) * (0.7 + 0.6 * rnd(seedOf(s), i, 6)), ay, smoke,
                 qRound(70 * std::sin(M_PI * u)));
    }
}

void fire(QPainter* p, const Shape& s, double t)
{
    const int n = particles(s);
    const int seed = seedOf(s);

    for (int i = 0; i < n; ++i) {
        const double u = frac(t * 0.85 + rnd(seed, i, 0) * 0.6);
        const double a = qRound(210 * smooth(qMin(u * 7, (1 - u) * 3.5)));
        if (a <= 0) continue;
        const double x = (rnd(seed, i, 1) - 0.5) * s.w * 0.75 + std::sin(u * 6 + i * 1.7) * s.w * 0.06 * spread(s);
        const double y = s.h * 0.5 - u * s.h * (0.7 + 0.4 * rnd(seed, i, 2));
        const double hh = s.h * (0.17 + 0.2 * rnd(seed, i, 3)) * (1 - 0.35 * u);
        const double ww = s.w * (0.035 + 0.045 * rnd(seed, i, 4));
        const double lean = std::sin(u * 5 + i) * ww * 0.9;

        QPainterPath flame;
        flame.moveTo(x - ww, y + hh * 0.5);
        flame.quadTo(x - ww * 0.7, y - hh * 0.15, x + lean, y - hh * 0.6);
        flame.quadTo(x + ww * 0.7, y - hh * 0.15, x + ww, y + hh * 0.5);
        flame.closeSubpath();
        p->setPen(Qt::NoPen);
        p->setBrush(withAlpha(mix(s.fill, s.accent, rnd(seed, i, 5) * 0.85), a));
        p->drawPath(flame);
    }
    // A few bright embers above the flames.
    for (int i = 0; i < n / 5; ++i) {
        const double u = frac(t * 1.3 + rnd(seed, i, 6));
        const QPointF q((rnd(seed, i, 7) - 0.5) * s.w * 0.8 + std::sin(u * 8 + i) * s.w * 0.05,
                        s.h * 0.45 - u * s.h * 1.2);
        p->setBrush(withAlpha(s.accent, qRound(220 * (1 - u))));
        p->drawEllipse(q, 2.0 + rnd(seed, i, 8), 2.0 + rnd(seed, i, 8));
    }
}

void smoke(QPainter* p, const Shape& s, double t)
{
    const int n = particles(s);
    const int seed = seedOf(s);
    const double ay = s.h / qMax(1.0, s.w);
    for (int i = 0; i < n; ++i) {
        const double u = frac(t * 0.8 + rnd(seed, i, 0) * 0.85);
        const double x = (rnd(seed, i, 1) - 0.5) * s.w * 0.65 +
                         std::sin(u * 4 + i * 1.3) * s.w * 0.1 * spread(s) * u;
        const QPointF c(x, s.h * 0.5 - u * s.h * (0.85 + 0.3 * rnd(seed, i, 2)));
        const double r = s.w * (0.05 + 0.09 * rnd(seed, i, 3)) * (0.5 + 1.8 * u);
        softBall(p, c, r, ay, mix(s.fill, s.accent, rnd(seed, i, 4) * 0.5), qRound(105 * smooth(qMin(u * 5, (1 - u) * 2.5))));
    }
}

void shockwave(QPainter* p, const Shape& s, double t)
{
    const double R = qMin(s.w, s.h) * 0.5;
    const double ay = s.h / qMax(1.0, s.w);
    const int rings = qBound(1, particles(s) / 8, 10);
    for (int k = 0; k < rings; ++k) {
        const double u = sat(t * 1.25 - k * 0.16);
        if (u <= 0) continue;
        const double r = R * (0.1 + 1.45 * u) * spread(s);
        const int a = qRound(200 * std::pow(1 - u, 1.6));
        p->setPen(QPen(withAlpha(k % 2 ? s.accent : s.fill, a), qMax(1.0, 7 * (1 - u)), Qt::SolidLine));
        p->setBrush(Qt::NoBrush);
        p->drawEllipse(QRectF(-r, -r * ay, 2 * r, 2 * r * ay));
    }
    // Radiating spokes, fading as the wave travels outwards.
    const int n = particles(s);
    for (int i = 0; i < n; ++i) {
        const double ang = kTau * rnd(seedOf(s), i, 0);
        const double inner = R * (0.12 + 1.05 * t) * spread(s);
        const double outer = inner + R * 0.22 * (1 - t) * (0.5 + rnd(seedOf(s), i, 1));
        const int a = qRound(170 * std::pow(1 - t, 2) * (0.4 + 0.6 * rnd(seedOf(s), i, 2)));
        p->setPen(QPen(withAlpha(mix(s.accent, Qt::white, rnd(seedOf(s), i, 3) * 0.5), a), 1 + 2 * rnd(seedOf(s), i, 4)));
        p->drawLine(QPointF(std::cos(ang) * inner, std::sin(ang) * inner * ay),
                    QPointF(std::cos(ang) * outer, std::sin(ang) * outer * ay));
    }
    if (t < 0.25) softBall(p, QPointF(0, 0), R * (0.3 + t), ay, Qt::white, qRound(160 * (1 - 4 * t)));
}

void confetti(QPainter* p, const Shape& s, double t)
{
    const int n = particles(s);
    const int seed = seedOf(s);
    for (int i = 0; i < n; ++i) {
        const double x0 = (rnd(seed, i, 0) - 0.5) * s.w * 0.5;
        const double vx = (rnd(seed, i, 1) - 0.5) * s.w * 0.9 * spread(s);
        const double x = x0 + vx * t + std::sin(t * 7 + i) * s.w * 0.03;
        const double y = -s.h * 0.55 + s.h * (1.1 * t * t + 0.35 * t);
        const double sz = (3 + 5 * rnd(seed, i, 2)) * qMax(0.4, s.w / 200.0);
        const double spin = (rnd(seed, i, 3) - 0.5) * 1400 * t + i;
        const int a = qRound(255 * (1 - smooth((t - 0.75) / 0.25)));
        if (a <= 0) continue;
        p->save();
        p->translate(x, y);
        p->rotate(spin);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor::fromHsvF(frac(rnd(seed, i, 4) + rnd(seed, i, 5) * 0.2), 0.75, 1.0, a / 255.0));
        p->drawRect(QRectF(-sz, -sz * 0.45, 2 * sz, sz * 0.9));
        p->restore();
    }
}

void sparkles(QPainter* p, const Shape& s, double t)
{
    const int n = particles(s);
    const int seed = seedOf(s);
    const double R = qMin(s.w, s.h) * 0.42;
    const double ay = s.h / qMax(1.0, s.w);
    p->setPen(Qt::NoPen);
    for (int i = 0; i < n; ++i) {
        const double ang = kTau * rnd(seed, i, 0) + t * kTau * 0.25;
        const double rad = R * (0.15 + 0.85 * rnd(seed, i, 1));
        const QPointF c(std::cos(ang) * rad, std::sin(ang) * rad * ay);
        const double twinkle = 0.5 + 0.5 * std::sin(kTau * (t * 2.5 + rnd(seed, i, 2)));
        const double r = (2 + 5 * rnd(seed, i, 3)) * (0.35 + 0.65 * twinkle) * qMax(0.4, s.w / 200.0);
        softBall(p, c, r * 2.2, 1, mix(s.fill, s.accent, rnd(seed, i, 4)), qRound(50 * twinkle));
        p->setBrush(withAlpha(mix(s.accent, Qt::white, 0.5 + 0.5 * twinkle), qRound(230 * twinkle)));
        star4(p, c, r);
    }
}

// Impact: a hard flash, then stars shooting out of the hit point while they
// pulse (grow/shrink) and spin, like a comic-book "clang!" or "pow!".
void starBurst(QPainter* p, const Shape& s, double t)
{
    const int seed = seedOf(s);
    const int n = particles(s);
    const double R = qMin(s.w, s.h) * 0.5;
    const double ay = s.h / qMax(1.0, s.w);
    const double scale = qMax(0.4, s.w / 200.0);

    // Hit flash at the point of impact.
    if (t < 0.35) {
        const double k = sat(t / 0.35);
        const double r = R * (0.25 + 0.55 * k);
        QRadialGradient g(QPointF(0, 0), r);
        const int a = qRound(255 * (1 - k) * (1 - k));
        g.setColorAt(0, withAlpha(Qt::white, a));
        g.setColorAt(0.4, withAlpha(s.fill, qRound(a * 0.9)));
        g.setColorAt(1, withAlpha(s.fill, 0));
        p->setPen(Qt::NoPen);
        p->setBrush(g);
        p->drawEllipse(QRectF(-r, -r * ay, 2 * r, 2 * r * ay));
    }
    // Expanding impact ring.
    {
        const double u = sat(t / 0.6);
        const double r = R * (0.12 + 1.1 * u) * spread(s);
        p->setPen(QPen(withAlpha(s.accent, qRound(190 * (1 - u))), qMax(1.0, 5 * (1 - u))));
        p->setBrush(Qt::NoBrush);
        p->drawEllipse(QRectF(-r, -r * ay, 2 * r, 2 * r * ay));
    }

    for (int i = 0; i < n; ++i) {
        const double ang = kTau * rnd(seed, i, 0) + (rnd(seed, i, 1) - 0.5) * 0.35;
        const double speed = R * (0.7 + 0.9 * rnd(seed, i, 2)) * spread(s);
        const double travel = 1 - std::pow(1 - sat(t), 2.2);  // fast out, then slow
        const QPointF c(std::cos(ang) * speed * travel, std::sin(ang) * speed * travel * ay + R * 0.25 * t * t);
        const double pulse = 0.55 + 0.45 * std::sin(kTau * (t * (2.5 + rnd(seed, i, 3) * 2.5) + rnd(seed, i, 4)));
        const double r = (6 + 9 * rnd(seed, i, 5)) * (0.45 + 0.55 * pulse) * scale * std::pow(1 - t, 0.6);
        if (r < 0.4) continue;
        const int a = qRound(255 * std::pow(1 - t, 0.8));
        const double spin = rnd(seed, i, 6) * kTau + t * (rnd(seed, i, 7) - 0.5) * 6;
        softBall(p, c, r * 1.9, 1, s.fill, qRound(45 * pulse * (1 - t)));
        p->setPen(Qt::NoPen);
        p->setBrush(withAlpha(mix(s.accent, Qt::white, rnd(seed, i, 8) * 0.5), a));
        starN(p, c, r, 4 + (i % 2), 0.38, spin);
    }
    // Small sharp rays around the impact.
    p->setPen(QPen(withAlpha(Qt::white, qRound(200 * (1 - t) * (1 - t))), 2 * scale, Qt::SolidLine, Qt::RoundCap));
    for (int i = 0; i < n / 2; ++i) {
        const double ang = kTau * rnd(seed, i, 9);
        const double r0 = R * (0.2 + 0.8 * t) * spread(s);
        p->drawLine(QPointF(std::cos(ang) * r0, std::sin(ang) * r0 * ay),
                    QPointF(std::cos(ang) * (r0 + R * 0.18 * (1 - t)), std::sin(ang) * (r0 + R * 0.18 * (1 - t)) * ay));
    }
}

}  // namespace

QStringList effectNames()
{
    QStringList names;
    for (const char* n : kNames) names << QString::fromLatin1(n);
    return names;
}

int effectCount() { return int(sizeof(kNames) / sizeof(kNames[0])); }

bool effectLoops(int preset) { return preset == Fire || preset == Smoke || preset == Sparkles; }

void paintEffect(QPainter* p, const Shape& s)
{
    const int preset = qBound(0, s.effectPreset, effectCount() - 1);
    const double t = effectLoops(preset) ? frac(s.effectTime) : sat(s.effectTime);
    switch (preset) {
    case Explosion: explosion(p, s, t); break;
    case Fire: fire(p, s, t); break;
    case Smoke: smoke(p, s, t); break;
    case Shockwave: shockwave(p, s, t); break;
    case Confetti: confetti(p, s, t); break;
    case Sparkles: sparkles(p, s, t); break;
    case StarBurst: starBurst(p, s, t); break;
    }
}
