#include "gifwriter.h"
#include "mainwindow.h"
#include "solver.h"

#include <QApplication>
#include <QDir>
#include <QTimer>
#include <cstdio>

// Headless smoke test: builds a small rig, poses it, tweens, round-trips the
// file format and exports a GIF. Run:  tood --selftest <outdir>
static int selfTest(const QString& dir)
{
    QDir().mkpath(dir);
    Document d;
    Frame& f = d.frames[0];
    auto add = [&](ShapeType t, QPointF p, double w, double h, QColor c) {
        Shape s;
        s.id = d.newId();
        s.type = t;
        s.pos = p;
        s.w = w;
        s.h = h;
        s.fill = c;
        f.shapes << s;
        return s.id;
    };
    const int body = add(ShapeType::Rect, {300, 330}, 80, 160, QColor(80, 140, 220));
    const int arm = add(ShapeType::Rect, {400, 270}, 140, 24, QColor(220, 50, 47));
    const int hand = add(ShapeType::Ellipse, {480, 270}, 36, 36, QColor(250, 210, 40));
    const int tail = add(ShapeType::Rect, {200, 400}, 100, 16, QColor(80, 180, 70));
    QString why;
    bool ok = true;
    ok &= Solver::connectShapes(f, body, {320, 270}, arm, {340, 270}, JointType::Hinge, d.newId(), &why);
    ok &= Solver::connectShapes(f, arm, {460, 270}, hand, {470, 270}, JointType::Weld, d.newId(), &why);
    ok &= Solver::connectShapes(f, body, {270, 400}, tail, {250, 400}, JointType::Free, d.newId(), &why);
    const bool loopRejected = !Solver::connectShapes(f, hand, {480, 270}, body, {300, 300}, JointType::Hinge, d.newId(), &why);
    std::printf("connect ok=%d loopRejected=%d\n", ok, loopRejected);

    auto maxErr = [](const Frame& fr) {
        double e = 0;
        for (const Joint& j : fr.joints) {
            const QPointF a = Solver::anchorWorld(*fr.find(j.parent), j.anchorP);
            const QPointF b = Solver::anchorWorld(*fr.find(j.child), j.anchorC);
            e = qMax(e, std::hypot(a.x() - b.x(), a.y() - b.y()));
        }
        return e;
    };

    Frame f2 = f;
    const double armRot0 = f2.find(arm)->rot;
    Solver::dragShape(f2, arm, f2.find(arm)->toLocal(f2.find(hand)->pos), {420, 160});  // hinge: rotates only
    const double armRot1 = f2.find(arm)->rot;
    const QPointF bodyBefore = f2.find(body)->pos;
    Solver::dragShape(f2, body, QPointF(0, 0), {420, 380});                            // carries everything
    std::printf("hinge drag rotated arm %.1f -> %.1f deg, body moved (%.0f,%.0f)->(%.0f,%.0f), err=%.2e\n",
                armRot0, armRot1, bodyBefore.x(), bodyBefore.y(), f2.find(body)->pos.x(), f2.find(body)->pos.y(),
                maxErr(f2));
    std::printf("free joint: tail rot after drag = %.1f deg (swings)\n", f2.find(tail)->rot);

    // limits
    Frame f3 = f;
    f3.joints[0].limited = true;
    f3.joints[0].minAngle = -30;
    f3.joints[0].maxAngle = 30;
    Solver::rotateShape(f3, arm, -90);
    std::printf("limited hinge: requested -90, got %.1f\n", f3.find(arm)->rot);

    d.frames << f2;
    double worst = 0;
    QVector<Frame> mids;
    for (int k = 1; k <= 8; ++k) {
        Frame t = Solver::tween(f, f2, Solver::ease(k / 9.0, 1));
        worst = qMax(worst, maxErr(t));
        mids << t;
    }
    for (int k = 0; k < mids.size(); ++k) d.frames.insert(1 + k, mids[k]);
    std::printf("tween: %d frames, worst joint gap %.2e px\n", int(mids.size()), worst);

    const QString path = QDir(dir).filePath("selftest.tood");
    QString err;
    Document back;
    const bool saved = d.save(path, &err);
    const bool loaded = Document::load(path, back, &err);
    std::printf("save=%d load=%d frames=%d joints=%d %s\n", saved, loaded, int(back.frames.size()),
                int(back.frames.at(0).joints.size()), qPrintable(err));

    GifWriter gw;
    const QImage first = renderFrameImage(d, d.frames.at(0), 0.5, true, Qt::white);
    gw.open(QDir(dir).filePath("selftest.gif"), first.width(), first.height());
    for (const Frame& fr : d.frames) gw.addFrame(renderFrameImage(d, fr, 0.5, true, Qt::white), 8);
    gw.close();
    renderFrameImage(d, d.frames.at(4), 1, true).save(QDir(dir).filePath("selftest_mid.png"));
    return ok && loopRejected && saved && loaded && worst < 1e-6 ? 0 : 1;
}

// Second smoke test for connectors, elastic joints, deformation and new shapes.
static int selfTest2(const QString& dir)
{
    Document d;
    Frame& f = d.frames[0];
    auto add = [&](ShapeType t, QPointF p, double w, double h, QColor c) {
        Shape s;
        s.id = d.newId();
        s.type = t;
        s.pos = p;
        s.w = w;
        s.h = h;
        s.fill = c;
        f.shapes << s;
        return s.id;
    };
    const int a = add(ShapeType::Rect, {150, 150}, 80, 80, QColor(80, 140, 220));
    const int b = add(ShapeType::Ellipse, {450, 150}, 80, 80, QColor(220, 50, 47));
    // spring connector glued centre-to-centre
    Shape sp;
    sp.id = d.newId();
    sp.type = ShapeType::Line;
    sp.lineStyle = 1;
    sp.sides = 8;
    sp.strokeWidth = 3;
    sp.pos = {300, 150};
    sp.w = 300;
    sp.h = 0;
    sp.ends[0] = {a, {0, 0}};
    sp.ends[1] = {b, {0, 0}};
    f.shapes << sp;
    Solver::translateShapes(f, {b}, QPointF(100, 200));
    const Shape* L = f.find(sp.id);
    const double gapA = QLineF(L->toWorld({-L->w / 2, 0}), f.find(a)->pos).length();
    const double gapB = QLineF(L->toWorld({L->w / 2, 0}), f.find(b)->pos).length();
    std::printf("connector: length %.1f, end gaps %.2e / %.2e\n", L->w, gapA, gapB);

    // elastic joint: lag while dragging, spring back afterwards
    const int c = add(ShapeType::Rect, {150, 450}, 60, 60, QColor(80, 180, 70));
    const int e = add(ShapeType::Rect, {230, 450}, 60, 60, QColor(140, 80, 200));
    QString why;
    Solver::connectShapes(f, c, {180, 450}, e, {200, 450}, JointType::Elastic, d.newId(), &why);
    for (int i = 0; i < 10; ++i) Solver::dragShape(f, c, QPointF(0, 0), QPointF(150 + 20 * (i + 1), 450), true);
    const bool gapWhileDragging = Solver::hasElasticGap(f, 1.0);
    QHash<int, QPointF> vel;
    int steps = 0;
    while (Solver::springStep(f, {c}, vel, 1.0 / 60) && steps < 600) ++steps;
    std::printf("elastic: gap while dragging=%d, settled after %d steps, gap now=%d\n", gapWhileDragging, steps,
                Solver::hasElasticGap(f, 0.5));

    // new shapes + warp + squash
    add(ShapeType::Hourglass, {600, 450}, 70, 100, QColor(250, 210, 40));
    f.shapes.last().innerRatio = 0.12;
    add(ShapeType::Diamond, {720, 450}, 110, 110, QColor(0, 0, 0, 0));
    f.shapes.last().sides = 9;
    f.shapes.last().strokeWidth = 1;
    add(ShapeType::Eye, {600, 300}, 140, 70, Qt::white);
    f.shapes.last().innerRatio = 0.6;
    add(ShapeType::Rect, {720, 300}, 90, 60, QColor(30, 160, 160));
    f.shapes.last().warp[1] = {0.4, -0.3};
    f.shapes.last().warp[2] = {0.2, 0.2};
    add(ShapeType::Ellipse, {420, 470}, 60, 60, QColor(240, 120, 170));
    f.shapes.last().squash = 1.8;
    f.shapes.last().squashAngle = 30;
    const QPointF warped = f.shapes.at(f.shapes.size() - 2).toWorld({45, -30});
    const QPointF back = f.shapes.at(f.shapes.size() - 2).toLocal(warped);
    std::printf("warp round-trip error %.2e\n", QLineF(back, QPointF(45, -30)).length());

    Document back2;
    QString err;
    d.save(QDir(dir).filePath("selftest2.tood"), &err);
    Document::load(QDir(dir).filePath("selftest2.tood"), back2, &err);
    const Frame& g = back2.frames.at(0);
    std::printf("roundtrip: shapes %d, warp kept=%d, links kept=%d, type kept=%d\n", int(g.shapes.size()),
                g.shapes.at(g.shapes.size() - 2).hasWarp(), g.find(sp.id)->hasLinks(),
                g.shapes.at(5).type == ShapeType::Hourglass);
    renderFrameImage(d, f, 1, true).save(QDir(dir).filePath("selftest2.png"));

    // chapters: two chapters survive a save/load round trip, the active one is restored
    Document cd;
    cd.frames[0].shapes << f.shapes.first();
    cd.chapters[0].name = "Intro";
    cd.chapters.append(Chapter{"Part 2", {Frame(), Frame(), Frame()}});
    cd.switchChapter(1);
    const QString cpath = QDir(dir).filePath("chapters.tood");
    cd.save(cpath, &err);
    Document cback;
    Document::load(cpath, cback, &err);
    const bool chOk = cback.chapters.size() == 2 && cback.activeChapter == 1 && cback.frames.size() == 3 &&
                      cback.chapterFrames(0).size() == 1 && cback.chapters.at(0).name == "Intro" && cback.totalFrames() == 4;
    std::printf("chapters roundtrip ok=%d (total %d frames)\n", chOk, cback.totalFrames());
    return (gapA < 1e-6 && gapB < 1e-6 && gapWhileDragging && !Solver::hasElasticGap(f, 0.5) && chOk) ? 0 : 1;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("TooD");
    QApplication::setOrganizationName("TooD");

    const QStringList args = app.arguments();
    const int st = args.indexOf("--selftest");
    if (st >= 0) {
        const QString dir = args.value(st + 1, QDir::tempPath());
        const int r1 = selfTest(dir);
        const int r2 = selfTest2(dir);
        std::printf("selftest %s\n", r1 == 0 && r2 == 0 ? "PASSED" : "FAILED");
        return r1 | r2;
    }
    const int shot = args.indexOf("--screenshot");  // --screenshot out.png [file.tood]

    MainWindow w;
    w.resize(1400, 900);
    bool opened = false;
    for (int i = 1; i < args.size(); ++i)
        if (args[i].endsWith(".tood")) opened |= w.openPath(args[i]);
    const int startShot = args.indexOf("--screenshot-start");  // debug: grab the start dialog
    if (startShot >= 0) {
        const QString out = args.value(startShot + 1);
        QTimer::singleShot(500, [out] {
            if (QWidget* d = QApplication::activeModalWidget()) d->grab().save(out);
            else for (QWidget* t : QApplication::topLevelWidgets()) if (t->isVisible()) t->grab().save(out);
            qApp->exit(0);
            std::exit(0);
        });
    }
    if (!opened && shot < 0 && !w.showStartDialog(true)) return 0;  // start dialog: new or open
    w.show();
    if (shot >= 0) {
        const QString out = args.value(shot + 1);
        QTimer::singleShot(900, &w, [&w, out] {
            w.grab().save(out);
            qApp->quit();
        });
    }
    return app.exec();
}
