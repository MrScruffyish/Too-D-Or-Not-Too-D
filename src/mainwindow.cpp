#include "mainwindow.h"

#include "canvas.h"
#include "gifwriter.h"
#include "solver.h"

#include <QtWidgets>
#include <QCryptographicHash>
#include <QSvgGenerator>
#include <QUndoStack>
#include <utility>

// ================================================================ helpers

static QPixmap swatchPixmap(const QColor& c, QSize sz)
{
    QPixmap pm(sz);
    pm.fill(Qt::white);
    QPainter p(&pm);
    for (int y = 0; y < sz.height(); y += 4)
        for (int x = 0; x < sz.width(); x += 4)
            if (((x / 4) + (y / 4)) & 1) p.fillRect(x, y, 4, 4, QColor(200, 200, 200));
    p.fillRect(pm.rect(), c);
    if (c.alpha() == 0) {
        p.setPen(QPen(Qt::red, 2));
        p.drawLine(pm.rect().bottomLeft(), pm.rect().topRight());
    }
    p.setPen(QColor(60, 60, 60));
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    return pm;
}

class ColorButton : public QToolButton {
public:
    explicit ColorButton(const QString& title, QWidget* parent = nullptr) : QToolButton(parent), m_title(title)
    {
        setIconSize(QSize(28, 16));
        setToolTip(title);
        setColor(Qt::black);
        connect(this, &QToolButton::clicked, this, [this] {
            const QColor c = QColorDialog::getColor(m_color, this, m_title, QColorDialog::ShowAlphaChannel);
            if (!c.isValid()) return;
            setColor(c);
            if (onChanged) onChanged(c);
        });
    }
    void setColor(const QColor& c)
    {
        m_color = c;
        setIcon(QIcon(swatchPixmap(c, QSize(28, 16))));
    }
    QColor color() const { return m_color; }
    std::function<void(const QColor&)> onChanged;

private:
    QString m_title;
    QColor m_color;
};

// Grid of colours: 5 tints of 12 hues, a grey ramp, then "none" + recently used.
class PaletteWidget : public QWidget {
public:
    std::function<void(const QColor&, bool stroke)> onPick;

    PaletteWidget()
    {
        const double sv[5][2] = {{0.22, 1.0}, {0.45, 1.0}, {0.85, 1.0}, {1.0, 0.72}, {1.0, 0.42}};
        for (const auto& t : sv) {
            QVector<QColor> row;
            for (int h = 0; h < kCols; ++h) row << QColor::fromHsvF(h / double(kCols), t[0], t[1]);
            m_rows << row;
        }
        QVector<QColor> greys;
        for (int i = 0; i < kCols; ++i) greys << QColor::fromHsvF(0, 0, i / double(kCols - 1));
        m_rows << greys;
        setFixedSize(kCols * kCell + 1, (m_rows.size() + 1) * kCell + 6);
        setToolTip(tr("Left-click: fill · Right-click: stroke"));
    }
    void addRecent(const QColor& c)
    {
        m_recent.removeAll(c);
        m_recent.prepend(c);
        while (m_recent.size() > kCols - 1) m_recent.removeLast();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        for (int r = 0; r <= m_rows.size(); ++r)
            for (int c = 0; c < kCols; ++c) {
                const QColor col = colorAt(r, c);
                if (!col.isValid()) continue;
                const QRect rc = cellRect(r, c);
                p.drawPixmap(rc, swatchPixmap(col, rc.size()));
            }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const QPoint pt = e->position().toPoint();
        int r = pt.y() / kCell;
        if (r >= m_rows.size()) r = (pt.y() - 6) / kCell;
        const QColor c = colorAt(r, pt.x() / kCell);
        if (c.isValid() && onPick) onPick(c, e->button() == Qt::RightButton);
    }

private:
    static constexpr int kCols = 12, kCell = 18;
    QRect cellRect(int r, int c) const
    {
        const int y = r * kCell + (r == m_rows.size() ? 6 : 0);
        return QRect(c * kCell, y, kCell - 1, kCell - 1);
    }
    QColor colorAt(int r, int c) const
    {
        if (c < 0 || c >= kCols || r < 0) return QColor();
        if (r < m_rows.size()) return m_rows[r][c];
        if (r == m_rows.size()) {
            if (c == 0) return QColor(0, 0, 0, 0);
            return c - 1 < m_recent.size() ? m_recent[c - 1] : QColor();
        }
        return QColor();
    }
    QVector<QVector<QColor>> m_rows;
    QVector<QColor> m_recent;
};

// Library list: dragging an entry onto the canvas inserts it there.
class LibraryList : public QListWidget {
protected:
    QStringList mimeTypes() const override { return {QStringLiteral("application/x-tood-library")}; }
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        auto* m = new QMimeData;
        if (!items.isEmpty()) m->setData("application/x-tood-library", items.first()->data(Qt::UserRole).toString().toUtf8());
        return m;
    }
};

static QString libraryDir()
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/library");
    QDir().mkpath(d);
    return d;
}

namespace {

// Undo command holding two whole-document snapshots. Documents are implicitly
// shared, so unchanged frames/shape lists cost nothing.
class SnapshotCmd : public QUndoCommand {
public:
    SnapshotCmd(MainWindow* w, Document before, int fb, Document after, int fa, const QString& text, int mergeId)
        : QUndoCommand(text), m_w(w), m_before(std::move(before)), m_after(std::move(after)),
          m_fb(fb), m_fa(fa), m_id(mergeId) {}
    void undo() override { m_w->applySnapshot(m_before, m_fb); }
    void redo() override
    {
        if (m_first) { m_first = false; return; }  // state is already current when pushed
        m_w->applySnapshot(m_after, m_fa);
    }
    int id() const override { return m_id; }
    bool mergeWith(const QUndoCommand* o) override
    {
        auto* c = static_cast<const SnapshotCmd*>(o);
        m_after = c->m_after;
        m_fa = c->m_fa;
        return true;
    }

private:
    MainWindow* m_w;
    Document m_before, m_after;
    int m_fb, m_fa, m_id;
    bool m_first = true;
};

QIcon toolIcon(int kind)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor ink(40, 40, 40), blue(90, 150, 230), pink(230, 0, 140);
    p.setPen(QPen(ink, 2));
    p.setBrush(blue);
    auto shape = [&](ShapeType t, double w, double h, int sides = 5, double inner = 0.45, QColor fill = QColor(90, 150, 230)) {
        Shape s;
        s.type = t;
        s.w = w;
        s.h = h;
        s.sides = sides;
        s.innerRatio = inner;
        s.fill = fill;
        s.stroke = ink;
        s.strokeWidth = t == ShapeType::Diamond ? 1.2 : 2;
        p.save();
        p.translate(16, 16);
        paintShapeLocal(&p, s, nullptr);
        p.restore();
    };
    auto dot = [&](QPointF c) {
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(pink);
        p.drawEllipse(c, 3.2, 3.2);
        p.restore();
    };
    switch (kind) {
    case Canvas::ToolSelect: {
        QPolygonF a;
        a << QPointF(9, 5) << QPointF(9, 26) << QPointF(14, 21) << QPointF(18, 29) << QPointF(21, 27)
          << QPointF(17, 20) << QPointF(24, 20);
        p.setBrush(Qt::white);
        p.drawPolygon(a);
        break;
    }
    case Canvas::ToolWarp: {
        QPolygonF q;
        q << QPointF(5, 8) << QPointF(26, 4) << QPointF(28, 27) << QPointF(9, 23);
        p.drawPolygon(q);
        p.setBrush(QColor(255, 150, 0));
        p.setPen(QPen(ink, 1));
        p.drawEllipse(QPointF(26, 4), 3, 3);
        break;
    }
    case Canvas::ToolRect: shape(ShapeType::Rect, 24, 18); break;
    case Canvas::ToolEllipse: shape(ShapeType::Ellipse, 24, 18); break;
    case Canvas::ToolTriangle: shape(ShapeType::Triangle, 24, 22); break;
    case Canvas::ToolPolygon: shape(ShapeType::Polygon, 24, 24); break;
    case Canvas::ToolStar: shape(ShapeType::Star, 26, 26); break;
    case Canvas::ToolDiamond: shape(ShapeType::Diamond, 26, 26, 7); break;
    case Canvas::ToolHourglass: shape(ShapeType::Hourglass, 20, 26, 5, 0.12); break;
    case Canvas::ToolEye: shape(ShapeType::Eye, 28, 16, 5, 0.55, Qt::white); break;
    case Canvas::ToolLine: p.drawLine(6, 26, 26, 6); break;
    case Canvas::ToolArrow: {
        p.drawLine(6, 26, 22, 10);
        p.setBrush(ink);
        QPolygonF h;
        h << QPointF(27, 5) << QPointF(16, 10) << QPointF(22, 16);
        p.drawPolygon(h);
        break;
    }
    case Canvas::ToolText: {
        QFont f;
        f.setPixelSize(26);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(0, 0, 32, 32), Qt::AlignCenter, "T");
        break;
    }
    case Canvas::ToolPoint:
        dot(QPointF(16, 16));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(pink, 1));
        p.drawEllipse(QPointF(16, 16), 8, 8);
        break;
    case Canvas::ToolConnect: {
        p.drawRect(3, 12, 16, 9);
        p.setBrush(QColor(230, 120, 90));
        p.save();
        p.translate(19, 16);
        p.rotate(-40);
        p.drawRect(0, -4, 13, 8);
        p.restore();
        p.setBrush(QColor(255, 140, 0));
        p.drawEllipse(QPointF(19, 16), 3.5, 3.5);
        break;
    }
    case Canvas::ToolMidpoint:
        p.drawLine(4, 24, 28, 8);
        dot(QPointF(16, 16));
        break;
    case Canvas::ToolPerpendicular:
        p.drawLine(4, 26, 28, 26);
        p.drawLine(16, 26, 16, 4);
        p.setPen(QPen(ink, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(16, 20, 6, 6);
        break;
    case Canvas::ToolParallel:
        p.drawLine(4, 22, 22, 4);
        p.drawLine(10, 28, 28, 10);
        break;
    case Canvas::ToolMeasure: {
        p.setBrush(QColor(250, 210, 40));
        p.save();
        p.translate(16, 16);
        p.rotate(-30);
        p.drawRect(-14, -5, 28, 10);
        p.setPen(QPen(ink, 1));
        for (int x = -10; x <= 10; x += 4) p.drawLine(x, -5, x, -1);
        p.restore();
        break;
    }
    default: {  // image
        p.setBrush(Qt::white);
        p.drawRect(4, 6, 24, 20);
        p.setBrush(QColor(80, 170, 80));
        QPolygonF m;
        m << QPointF(5, 25) << QPointF(13, 14) << QPointF(19, 21) << QPointF(23, 17) << QPointF(27, 25);
        p.setPen(Qt::NoPen);
        p.drawPolygon(m);
        p.setBrush(QColor(250, 200, 40));
        p.drawEllipse(QPointF(22, 11), 2.5, 2.5);
    }
    }
    return QIcon(pm);
}

QIcon themeIcon(const char* name, QStyle::StandardPixmap fb)
{
    return QIcon::fromTheme(QString::fromLatin1(name), qApp->style()->standardIcon(fb));
}

QString shapeLabel(const Shape* s)
{
    if (!s) return QStringLiteral("?");
    return s->name.isEmpty() ? QStringLiteral("%1 #%2").arg(shapeTypeName(s->type)).arg(s->id) : s->name;
}

QDoubleSpinBox* dspin(double lo, double hi, double step, int dec, const QString& suffix = QString())
{
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(dec);
    s->setKeyboardTracking(false);
    s->setSuffix(suffix);
    return s;
}

}  // namespace

// ================================================================ construction

MainWindow::MainWindow()
{
    m_undo = new QUndoStack(this);
    m_undo->setUndoLimit(500);
    m_canvas = new Canvas(this);
    m_canvas->style.fill = QColor(80, 140, 220);
    m_canvas->style.stroke = QColor(30, 30, 40);
    m_canvas->style.strokeWidth = 2;
    m_canvas->setDocument(&m_doc);
    setCentralWidget(m_canvas);
    m_committed = m_doc;

    // a useful set of custom colours in the colour dialog (skin tones, muted, vivid)
    const QList<QColor> custom = {QColor(255, 224, 189), QColor(255, 205, 178), QColor(229, 152, 102), QColor(141, 85, 36),
                                  QColor(46, 52, 64),    QColor(94, 129, 172),  QColor(163, 190, 140), QColor(235, 203, 139),
                                  QColor(191, 97, 106),  QColor(180, 142, 173), QColor(136, 192, 208), QColor(216, 222, 233),
                                  QColor(255, 99, 71),   QColor(255, 215, 0),   QColor(64, 224, 208),  QColor(138, 43, 226)};
    for (int i = 0; i < custom.size(); ++i) QColorDialog::setCustomColor(i, custom[i]);

    buildActions();
    buildDocks();
    buildMenus();
    buildToolbars();

    connect(m_canvas, &Canvas::edited, this, [this](const QString& t) {
        commit(t, t == QLatin1String("Nudge") ? mid(60) : -1);
        refreshProps();
    });
    connect(m_canvas, &Canvas::liveChanged, this, &MainWindow::refreshProps);
    connect(m_canvas, &Canvas::selectionChanged, this, [this] {
        ++m_selEpoch;
        refreshProps();
    });
    connect(m_canvas, &Canvas::toolChanged, this, [this](int t) {
        if (t >= 0 && t < m_toolActs.size()) m_toolActs[t]->setChecked(true);
    });
    connect(m_canvas, &Canvas::textEditRequested, this, &MainWindow::editText);
    connect(m_canvas, &Canvas::imageDropped, this, [this](const QString& p, QPointF at) { insertImage(p, at); });
    connect(m_canvas, &Canvas::libraryDropped, this, [this](const QString& p, QPointF at) { libraryInsert(p, at); });
    connect(m_canvas, &Canvas::message, this, &MainWindow::log);
    connect(m_canvas, &Canvas::prompt, this, &MainWindow::setPrompt);
    connect(m_canvas, &Canvas::zoomChanged, this, [this](double s) {
        m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(s * 100)));
    });
    connect(m_canvas, &Canvas::cursorMoved, this, [this](QPointF p) {
        m_posLabel->setText(QStringLiteral("x %1  y %2").arg(p.x(), 0, 'f', 0).arg(p.y(), 0, 'f', 0));
    });

    m_thumbTimer.setSingleShot(true);
    connect(&m_thumbTimer, &QTimer::timeout, this, &MainWindow::renderThumbs);
    m_playTimer.setSingleShot(true);
    connect(&m_playTimer, &QTimer::timeout, this, &MainWindow::playStep);
    connect(m_undo, &QUndoStack::cleanChanged, this, &MainWindow::updateTitle);

    setPrompt(tr(Canvas::toolInfo(Canvas::ToolSelect).help));
    log(tr("Welcome. Hover a tool to see what it does; step-by-step instructions appear above. "
           "Middle-drag or Space+drag pans, the wheel zooms."));
    refreshChapters();
    refreshTimeline();
    setCurrentFrame(0);
    updateTitle();
}

void MainWindow::showEvent(QShowEvent* e)
{
    QMainWindow::showEvent(e);
    if (m_shownOnce) return;
    m_shownOnce = true;
    QTimer::singleShot(0, this, [this] {  // layout-dependent setup needs the final window size
        auto* tdock = findChild<QDockWidget*>("frames");
        auto* idock = findChild<QDockWidget*>("info");
        if (tdock && idock) resizeDocks({tdock, idock}, {width() * 6 / 10, width() * 4 / 10}, Qt::Horizontal);
        m_canvas->fitToCanvas();
    });
}

bool MainWindow::showStartDialog(bool startup)
{
    QDialog dlg(startup ? nullptr : this);
    dlg.setWindowTitle(startup ? tr("TooD — start") : tr("New project"));
    dlg.setMinimumWidth(520);
    auto* lay = new QVBoxLayout(&dlg);
    QSettings st;

    // ---- create new
    auto* newBox = new QGroupBox(tr("Create new project"));
    auto* nf = new QFormLayout(newBox);
    auto* preset = new QComboBox;
    struct Preset {
        const char* name;
        int w, h;
    };
    static const Preset presets[] = {
        {"800 × 600", 800, 600},          {"HD 1280 × 720", 1280, 720},         {"Full HD 1920 × 1080", 1920, 1080},
        {"Square 1080 × 1080", 1080, 1080}, {"Portrait 1080 × 1920", 1080, 1920}, {"Icon 512 × 512", 512, 512},
        {"A4 portrait, 300 dpi (2480 × 3508)", 2480, 3508}, {"A4 landscape, 300 dpi (3508 × 2480)", 3508, 2480},
    };
    for (const Preset& p : presets) preset->addItem(tr(p.name), QSize(p.w, p.h));
    preset->addItem(tr("Custom"));
    auto* w = new QSpinBox;
    w->setRange(1, 20000);
    w->setSuffix(tr(" px"));
    w->setValue(st.value("new/width", 800).toInt());
    auto* h = new QSpinBox;
    h->setRange(1, 20000);
    h->setSuffix(tr(" px"));
    h->setValue(st.value("new/height", 600).toInt());
    auto* bg = new ColorButton(tr("Background"));
    bg->setColor(st.value("new/bg", QColor(Qt::white)).value<QColor>());
    auto* fps = new QSpinBox;
    fps->setRange(1, 120);
    fps->setValue(st.value("new/fps", 12).toInt());
    auto syncPreset = [=] {
        int idx = preset->count() - 1;
        for (int i = 0; i < preset->count() - 1; ++i)
            if (preset->itemData(i).toSize() == QSize(w->value(), h->value())) idx = i;
        const QSignalBlocker b(preset);
        preset->setCurrentIndex(idx);
    };
    connect(preset, &QComboBox::currentIndexChanged, &dlg, [=](int i) {
        const QSize s = preset->itemData(i).toSize();
        if (!s.isValid()) return;
        const QSignalBlocker bw(w), bh(h);
        w->setValue(s.width());
        h->setValue(s.height());
    });
    connect(w, &QSpinBox::valueChanged, &dlg, syncPreset);
    connect(h, &QSpinBox::valueChanged, &dlg, syncPreset);
    syncPreset();
    auto* swap = new QPushButton(tr("⇄"));
    swap->setToolTip(tr("Swap width and height"));
    swap->setFixedWidth(36);
    connect(swap, &QPushButton::clicked, &dlg, [=] {
        const int t = w->value();
        w->setValue(h->value());
        h->setValue(t);
    });
    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(w);
    sizeRow->addWidget(new QLabel(QStringLiteral("×")));
    sizeRow->addWidget(h);
    sizeRow->addWidget(swap);
    auto* createBtn = new QPushButton(tr("Create"));
    createBtn->setDefault(true);
    nf->addRow(tr("Preset"), preset);
    nf->addRow(tr("Size"), sizeRow);
    nf->addRow(tr("Background"), bg);
    nf->addRow(tr("Animation FPS"), fps);
    nf->addRow(QString(), createBtn);
    lay->addWidget(newBox);

    // ---- open existing
    auto* openBox = new QGroupBox(tr("Open existing project"));
    auto* ov = new QVBoxLayout(openBox);
    auto* recent = new QListWidget;
    for (const QString& p : st.value("recent").toStringList()) {
        if (!QFileInfo::exists(p)) continue;
        auto* it = new QListWidgetItem(QStringLiteral("%1    —  %2").arg(QFileInfo(p).fileName(), QFileInfo(p).absolutePath()));
        it->setData(Qt::UserRole, p);
        it->setToolTip(p);
        recent->addItem(it);
    }
    if (recent->count()) recent->setCurrentRow(0);
    recent->setMaximumHeight(140);
    auto* openRow = new QHBoxLayout;
    auto* openBtn = new QPushButton(tr("Open selected"));
    openBtn->setEnabled(recent->count() > 0);
    auto* browse = new QPushButton(tr("Browse…"));
    openRow->addStretch();
    openRow->addWidget(openBtn);
    openRow->addWidget(browse);
    if (recent->count()) {
        ov->addWidget(new QLabel(tr("Recent projects:")));
        ov->addWidget(recent);
    } else {
        ov->addWidget(new QLabel(tr("No recent projects.")));
        recent->hide();
    }
    ov->addLayout(openRow);
    lay->addWidget(openBox);

    auto* bottom = new QHBoxLayout;
    auto* quit = new QPushButton(startup ? tr("Quit") : tr("Cancel"));
    bottom->addStretch();
    bottom->addWidget(quit);
    lay->addLayout(bottom);

    QString chosen;
    bool create = false;
    connect(createBtn, &QPushButton::clicked, &dlg, [&] {
        create = true;
        dlg.accept();
    });
    auto openItem = [&](QListWidgetItem* it) {
        if (!it) return;
        chosen = it->data(Qt::UserRole).toString();
        dlg.accept();
    };
    connect(openBtn, &QPushButton::clicked, &dlg, [&] { openItem(recent->currentItem()); });
    connect(recent, &QListWidget::itemDoubleClicked, &dlg, openItem);
    connect(browse, &QPushButton::clicked, &dlg, [&] {
        const QString p = QFileDialog::getOpenFileName(&dlg, tr("Open project"), QFileInfo(baseName()).absolutePath(),
                                                       tr("TooD projects (*.tood);;All files (*)"));
        if (p.isEmpty()) return;
        chosen = p;
        dlg.accept();
    });
    connect(quit, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return false;
    if (!create) {
        if (openPath(chosen)) return true;
        return showStartDialog(startup);  // failed to open: ask again
    }
    st.setValue("new/width", w->value());
    st.setValue("new/height", h->value());
    st.setValue("new/bg", bg->color());
    st.setValue("new/fps", fps->value());
    Document d;
    d.width = w->value();
    d.height = h->value();
    d.background = bg->color();
    d.fps = fps->value();
    m_path.clear();
    m_thumbs.clear();
    applySnapshot(d, 0);
    m_canvas->resetScene();
    m_undo->clear();
    updateTitle();
    m_canvas->fitToCanvas();
    log(tr("New project, %1 × %2 px").arg(d.width).arg(d.height));
    return true;
}

QAction* MainWindow::act(const QString& text, const QKeySequence& ks, std::function<void()> fn, const QIcon& icon)
{
    auto* a = new QAction(icon, text, this);
    if (!ks.isEmpty()) a->setShortcut(ks);
    connect(a, &QAction::triggered, this, [fn = std::move(fn)] { fn(); });
    return a;
}

void MainWindow::buildActions()
{
    m_actNew = act(tr("&New"), QKeySequence::New, [this] { newDoc(); }, themeIcon("document-new", QStyle::SP_FileIcon));
    m_actOpen = act(tr("&Open…"), QKeySequence::Open, [this] { openDoc(); }, themeIcon("document-open", QStyle::SP_DialogOpenButton));
    m_actSave = act(tr("&Save"), QKeySequence::Save, [this] { saveDoc(); }, themeIcon("document-save", QStyle::SP_DialogSaveButton));
    m_actSaveAs = act(tr("Save &As…"), QKeySequence::SaveAs, [this] { saveDocAs(); });
    m_actImage = act(tr("Insert &Image…"), QKeySequence(tr("Ctrl+I")), [this] { insertImage(); }, toolIcon(1000));
    m_actExportFrame = act(tr("Export Current Frame (PNG/JPG/SVG)…"), QKeySequence(tr("Ctrl+E")), [this] { exportFrame(); });
    m_actExportAnim = act(tr("Export &Animation (MP4/GIF/WebM/PNG)…"), QKeySequence(tr("Ctrl+Shift+E")), [this] { exportAnimation(); });
    m_actDocSettings = act(tr("Canvas &Settings…"), QKeySequence(), [this] { docSettings(); });
    m_actQuit = act(tr("&Quit"), QKeySequence::Quit, [this] { close(); });

    m_actUndo = m_undo->createUndoAction(this, tr("&Undo"));
    m_actUndo->setShortcut(QKeySequence::Undo);
    m_actUndo->setIcon(themeIcon("edit-undo", QStyle::SP_ArrowBack));
    m_actRedo = m_undo->createRedoAction(this, tr("&Redo"));
    m_actRedo->setShortcuts({QKeySequence::Redo, QKeySequence(tr("Ctrl+Y"))});
    m_actRedo->setIcon(themeIcon("edit-redo", QStyle::SP_ArrowForward));
    m_actCut = act(tr("Cu&t"), QKeySequence::Cut, [this] { cutSel(); });
    m_actCopy = act(tr("&Copy"), QKeySequence::Copy, [this] { copySel(); });
    m_actPaste = act(tr("&Paste"), QKeySequence::Paste, [this] { paste(); });
    m_actDup = act(tr("&Duplicate"), QKeySequence(tr("Ctrl+D")), [this] { duplicateSel(); });
    m_actDel = act(tr("De&lete"), QKeySequence::Delete, [this] { deleteSel(); });
    m_actSelAll = act(tr("Select &All"), QKeySequence::SelectAll, [this] {
        QVector<int> ids;
        for (const Shape& s : m_doc.frames.at(m_cur).shapes) ids << s.id;
        m_canvas->setSelectedIds(ids);
    });
    m_actSelConnected = act(tr("Select &Connected"), QKeySequence(tr("Ctrl+L")), [this] { selectConnected(); });
    m_actGroup = act(tr("&Group"), QKeySequence(tr("Ctrl+G")), [this] { groupSel(); });
    m_actUngroup = act(tr("U&ngroup"), QKeySequence(tr("Ctrl+Shift+G")), [this] { ungroupSel(); });
    m_actDisconnect = act(tr("Remove Connections of Selection"), QKeySequence(tr("Ctrl+Shift+C")), [this] { disconnectSel(); });
    m_actFwd = act(tr("Bring &Forward"), QKeySequence(tr("Ctrl+]")), [this] { reorder(0); });
    m_actBack = act(tr("Send &Backward"), QKeySequence(tr("Ctrl+[")), [this] { reorder(1); });
    m_actFront = act(tr("Bring to Front"), QKeySequence(tr("Ctrl+Shift+]")), [this] { reorder(2); });
    m_actBottom = act(tr("Send to Back"), QKeySequence(tr("Ctrl+Shift+[")), [this] { reorder(3); });

    m_actFrameNew = act(tr("New Frame (copy)"), QKeySequence(), [this] { frameNewCopy(); });
    m_actFrameBlank = act(tr("Blank Frame"), QKeySequence(), [this] { frameNewBlank(); });
    m_actFrameDup = act(tr("Duplicate ×N…"), QKeySequence(), [this] { frameDuplicateN(); });
    m_actFrameDel = act(tr("Delete Frame"), QKeySequence(), [this] { frameDelete(); });
    m_actFrameLeft = act(tr("◀ Move"), QKeySequence(), [this] { frameMove(-1); });
    m_actFrameRight = act(tr("Move ▶"), QKeySequence(), [this] { frameMove(1); });
    m_actTween = act(tr("Tween…"), QKeySequence(), [this] { frameTween(); });
    m_actPrev = act(tr("Previous Frame"), QKeySequence(), [this] { setCurrentFrame(m_cur - 1); });
    m_actNext = act(tr("Next Frame"), QKeySequence(), [this] { setCurrentFrame(m_cur + 1); });
    m_actPlay = act(tr("Play"), QKeySequence(), [this] { togglePlay(); }, themeIcon("media-playback-start", QStyle::SP_MediaPlay));
    // Frame shortcuts (single keys avoid the tool letters; they don't fire while typing in a text field)
    const QList<QPair<QAction*, QList<QKeySequence>>> frameKeys = {
        {m_actFrameNew, {QKeySequence(tr("N")), QKeySequence(tr("Ctrl+Shift+N"))}},
        {m_actFrameBlank, {QKeySequence(tr("Shift+N"))}},
        {m_actFrameDup, {QKeySequence(tr("Ctrl+Shift+D"))}},
        {m_actFrameDel, {QKeySequence(tr("Shift+Del")), QKeySequence(tr("Ctrl+Shift+Del"))}},
        {m_actFrameLeft, {QKeySequence(tr("Alt+PgUp")), QKeySequence(tr("Alt+,"))}},
        {m_actFrameRight, {QKeySequence(tr("Alt+PgDown")), QKeySequence(tr("Alt+."))}},
        {m_actTween, {QKeySequence(tr("Ctrl+T"))}},
        {m_actPrev, {QKeySequence(Qt::Key_PageUp), QKeySequence(tr(","))}},
        {m_actNext, {QKeySequence(Qt::Key_PageDown), QKeySequence(tr("."))}},
        {m_actPlay, {QKeySequence(tr("Ctrl+Return")), QKeySequence(tr("Ctrl+Space"))}},
    };
    m_actChNew = act(tr("New Chapter…"), QKeySequence(tr("Ctrl+Alt+N")), [this] { chapterNew(); });
    m_actChDup = act(tr("Duplicate Chapter"), QKeySequence(), [this] { chapterDuplicate(); });
    m_actChRename = act(tr("Rename Chapter…"), QKeySequence(), [this] { chapterRename(); });
    m_actChDel = act(tr("Delete Chapter"), QKeySequence(), [this] { chapterDelete(); });
    m_actChLeft = act(tr("Move Chapter Earlier"), QKeySequence(), [this] { chapterMove(-1); });
    m_actChRight = act(tr("Move Chapter Later"), QKeySequence(), [this] { chapterMove(1); });
    m_actChImport = act(tr("Import Chapters from Project…"), QKeySequence(), [this] { chapterImport(); });
    m_actChExport = act(tr("Export Chapter as Project…"), QKeySequence(), [this] { chapterExport(); });
    m_actChPrev = act(tr("Previous Chapter"), QKeySequence(tr("Ctrl+PgUp")), [this] { switchChapter(m_doc.activeChapter - 1); });
    m_actChNext = act(tr("Next Chapter"), QKeySequence(tr("Ctrl+PgDown")), [this] { switchChapter(m_doc.activeChapter + 1); });
    m_actLibAdd = act(tr("Add Selection to Library…"), QKeySequence(tr("Ctrl+B")), [this] { libraryAdd(); });
    m_actLibInsert = act(tr("Insert from Library"), QKeySequence(), [this] {
        if (QListWidgetItem* it = m_library->currentItem()) libraryInsert(it->data(Qt::UserRole).toString());
    });
    for (const auto& h : QList<QPair<QAction*, QString>>{
             {m_actChNew, tr("New chapter (Ctrl+Alt+N): a project can be split into chapters; you work on one at a time. "
                             "The movie export can merge all chapters.")},
             {m_actLibAdd, tr("Add to library (Ctrl+B): saves the selection (optionally with everything connected to it) "
                              "under a name. Double-click or drag it from the Library panel into any frame, in any project.")}}) {
        const QString text = h.second;
        connect(h.first, &QAction::hovered, this, [this, text] { setPrompt(text); });
    }
    for (const auto& fk : frameKeys) {
        QAction* a = fk.first;
        a->setShortcuts(fk.second);
        a->setShortcutContext(Qt::WindowShortcut);
        QStringList keys;
        for (const QKeySequence& k : fk.second) keys << k.toString(QKeySequence::NativeText);
        a->setToolTip(QStringLiteral("%1  (%2)").arg(a->text().remove('&'), keys.join(QStringLiteral(" / "))));
    }

    m_actZoomIn = act(tr("Zoom In"), QKeySequence::ZoomIn, [this] { m_canvas->zoomBy(1.25); });
    m_actZoomOut = act(tr("Zoom Out"), QKeySequence::ZoomOut, [this] { m_canvas->zoomBy(0.8); });
    m_actFit = act(tr("Fit"), QKeySequence(tr("Ctrl+0")), [this] { m_canvas->fitToCanvas(); });
    m_actGrid = act(tr("Grid"), QKeySequence(tr("Ctrl+'")), [this] { m_canvas->setShowGrid(m_actGrid->isChecked()); });
    m_actSnap = act(tr("Snap grid"), QKeySequence(tr("Ctrl+Shift+'")), [this] { m_canvas->snap = m_actSnap->isChecked(); });
    m_actSnapPts = act(tr("Snap points"), QKeySequence(tr("F9")), [this] { m_canvas->snapPoints = m_actSnapPts->isChecked(); });
    m_actOrtho = act(tr("Ortho"), QKeySequence(tr("F8")), [this] { m_canvas->ortho = m_actOrtho->isChecked(); });
    m_actSticky = act(tr("Sticky ends"), QKeySequence(), [this] { m_canvas->stickyEnds = m_actSticky->isChecked(); });
    m_actOnion = act(tr("Onion skin"), QKeySequence(tr("Ctrl+K")), [this] { m_canvas->setOnion(m_actOnion->isChecked()); });
    m_actJoints = act(tr("Connections"), QKeySequence(tr("Ctrl+J")), [this] {
        m_canvas->showJoints = m_actJoints->isChecked();
        m_canvas->viewport()->update();
    });
    for (QAction* a : {m_actGrid, m_actSnap, m_actSnapPts, m_actOrtho, m_actSticky, m_actOnion, m_actJoints}) a->setCheckable(true);
    m_actSnapPts->setChecked(true);
    m_actSticky->setChecked(true);
    m_actJoints->setChecked(true);

    // short tooltips; the long explanation goes to the info panel on hover
    const QList<QPair<QAction*, QString>> help = {
        {m_actFrameNew, tr("New Frame (copy): inserts a copy of the current frame after it and switches to it. Modify the copy to make the next step.")},
        {m_actFrameDup, tr("Duplicate ×N: inserts identical copies, i.e. holds the image longer. Alternatively change the frame Duration.")},
        {m_actTween, tr("Tween: generates in-between frames from this frame to the next one, with easing and optional squash & stretch.")},
        {m_actSnapPts, tr("Snap points (F9): point-picking tools snap to corners, edge midpoints and centres of shapes.")},
        {m_actOrtho, tr("Ortho (F8): lines are drawn only horizontally or vertically.")},
        {m_actSticky, tr("Sticky ends: line/arrow ends drawn or dropped on a shape are glued to it, so the line stretches when the shape moves.")},
        {m_actOnion, tr("Onion skin (Ctrl+K): shows the previous frame faintly behind the current one.")},
        {m_actSelConnected, tr("Select connected (Ctrl+L): extends the selection to everything joined to it (joints and glued lines). Then move, copy or group.")},
        {m_actGroup, tr("Group (Ctrl+G): grouped shapes are always selected together. Alt+click selects a single member. Ungroup with Ctrl+Shift+G.")},
    };
    for (const auto& h : help) {
        const QString text = h.second;
        connect(h.first, &QAction::hovered, this, [this, text] { setPrompt(text); });
    }

    // tools
    auto* group = new QActionGroup(this);
    for (int t = 0; t < Canvas::ToolCount; ++t) {
        const auto& ti = Canvas::toolInfo(t);
        QAction* a = act(QStringLiteral("%1 (%2)").arg(tr(ti.name), QString::fromLatin1(ti.key)),
                         QKeySequence(QString::fromLatin1(ti.key)),
                         [this, t] { m_canvas->setTool(Canvas::Tool(t)); }, toolIcon(t));
        a->setCheckable(true);
        group->addAction(a);
        const QString h = tr(ti.help);
        connect(a, &QAction::hovered, this, [this, h] { setPrompt(h); });
        m_toolActs << a;
    }
    m_toolActs[0]->setChecked(true);
}

void MainWindow::buildMenus()
{
    QMenu* f = menuBar()->addMenu(tr("&File"));
    f->addActions({m_actNew, m_actOpen});
    m_recentMenu = f->addMenu(tr("Open &Recent"));
    rebuildRecentMenu();
    f->addActions({m_actSave, m_actSaveAs});
    f->addSeparator();
    f->addActions({m_actImage, m_actDocSettings});
    f->addSeparator();
    f->addActions({m_actExportFrame, m_actExportAnim});
    f->addSeparator();
    f->addAction(m_actQuit);

    QMenu* e = menuBar()->addMenu(tr("&Edit"));
    e->addActions({m_actUndo, m_actRedo});
    e->addSeparator();
    e->addActions({m_actCut, m_actCopy, m_actPaste, m_actDup, m_actDel});
    e->addSeparator();
    e->addActions({m_actSelAll, m_actSelConnected, m_actGroup, m_actUngroup});
    e->addSeparator();
    e->addActions({m_actFwd, m_actBack, m_actFront, m_actBottom});
    e->addSeparator();
    e->addAction(m_actDisconnect);
    e->addSeparator();
    e->addActions({m_actLibAdd, m_actLibInsert});

    QMenu* t = menuBar()->addMenu(tr("&Tools"));
    for (int i = 0; i < m_toolActs.size(); ++i) {
        if (i == Canvas::ToolRect || i == Canvas::ToolLine || i == Canvas::ToolConnect) t->addSeparator();
        t->addAction(m_toolActs[i]);
    }

    QMenu* fr = menuBar()->addMenu(tr("F&rames"));
    fr->addActions({m_actFrameNew, m_actFrameBlank, m_actFrameDup, m_actFrameDel});
    fr->addSeparator();
    fr->addActions({m_actFrameLeft, m_actFrameRight, m_actTween});
    fr->addSeparator();
    fr->addActions({m_actPrev, m_actNext, m_actPlay});

    QMenu* ch = menuBar()->addMenu(tr("&Chapters"));
    ch->addActions({m_actChNew, m_actChDup, m_actChRename, m_actChDel});
    ch->addSeparator();
    ch->addActions({m_actChPrev, m_actChNext, m_actChLeft, m_actChRight});
    ch->addSeparator();
    ch->addActions({m_actChImport, m_actChExport});

    QMenu* v = menuBar()->addMenu(tr("&View"));
    v->addActions({m_actZoomIn, m_actZoomOut, m_actFit});
    v->addSeparator();
    v->addActions({m_actGrid, m_actSnap, m_actSnapPts, m_actOrtho, m_actSticky, m_actOnion, m_actJoints});
    v->addSeparator();
    for (QDockWidget* d : findChildren<QDockWidget*>()) v->addAction(d->toggleViewAction());
}

void MainWindow::buildToolbars()
{
    auto* tools = new QToolBar(tr("Tools"), this);
    tools->setObjectName("tools");
    addToolBar(Qt::LeftToolBarArea, tools);
    tools->setIconSize(QSize(26, 26));
    for (int i = 0; i < m_toolActs.size(); ++i) {
        if (i == Canvas::ToolRect || i == Canvas::ToolLine || i == Canvas::ToolConnect) tools->addSeparator();
        tools->addAction(m_toolActs[i]);
    }
    tools->addSeparator();
    tools->addAction(m_actImage);

    QToolBar* tb = addToolBar(tr("Main"));
    tb->setObjectName("main");
    tb->addActions({m_actNew, m_actOpen, m_actSave});
    tb->addSeparator();
    tb->addActions({m_actUndo, m_actRedo});
    tb->addSeparator();

    tb->addWidget(new QLabel(tr(" Fill ")));
    m_styleFill = new ColorButton(tr("Fill colour"));
    m_styleFill->setColor(m_canvas->style.fill);
    m_styleFill->onChanged = [this](const QColor& c) { setStyleFill(c); };
    tb->addWidget(m_styleFill);
    tb->addWidget(new QLabel(tr(" Stroke ")));
    m_styleStroke = new ColorButton(tr("Stroke colour"));
    m_styleStroke->setColor(m_canvas->style.stroke);
    m_styleStroke->onChanged = [this](const QColor& c) { setStyleStroke(c); };
    tb->addWidget(m_styleStroke);
    m_styleWidth = dspin(0, 200, 0.5, 1, tr(" px"));
    m_styleWidth->setValue(m_canvas->style.strokeWidth);
    m_styleWidth->setToolTip(tr("Stroke width"));
    connect(m_styleWidth, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        m_canvas->style.strokeWidth = v;
        editAll([v](Shape& s) { s.strokeWidth = v; }, tr("Stroke width"), 11);
    });
    tb->addWidget(m_styleWidth);
    tb->addSeparator();

    tb->addWidget(new QLabel(tr(" Connect as ")));
    m_jointCombo = new QComboBox;
    for (int i = 0; i < 4; ++i) m_jointCombo->addItem(jointTypeName(JointType(i)));
    m_jointCombo->setToolTip(tr("Type for new connections"));
    connect(m_jointCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_canvas->newJointType = JointType(i);
        m_canvas->setTool(Canvas::ToolConnect);
        static const char* h[] = {
            "Hinge: the child only rotates around the pin (optionally within angle limits); moving the parent carries it along.",
            "Free: the pin stays connected; the other shape swings along behind when you drag.",
            "Weld: both shapes move and rotate as one rigid piece.",
            "Elastic: like a rubber band. The other shape lags behind while you drag and springs back when you let go. Stiffness in Properties."};
        setPrompt(tr(h[qBound(0, i, 3)]));
    });
    tb->addWidget(m_jointCombo);
    tb->addSeparator();
    tb->addActions({m_actSnapPts, m_actOrtho, m_actSticky, m_actGrid, m_actSnap, m_actOnion, m_actJoints});
    tb->addSeparator();
    tb->addAction(m_actFit);

    m_posLabel = new QLabel;
    m_zoomLabel = new QLabel(QStringLiteral("100%"));
    statusBar()->addPermanentWidget(m_posLabel);
    statusBar()->addPermanentWidget(m_zoomLabel);
}

void MainWindow::buildDocks()
{
    // ---------------- colours
    auto* cw = new QWidget;
    auto* cv = new QVBoxLayout(cw);
    cv->setContentsMargins(6, 4, 6, 4);
    m_palette = new PaletteWidget;
    m_palette->onPick = [this](const QColor& c, bool stroke) {
        if (stroke) setStyleStroke(c);
        else setStyleFill(c);
    };
    cv->addWidget(m_palette);
    auto* hintL = new QLabel(tr("Left-click: fill · right-click: stroke. Bottom row: none + recent. "
                                "Any colour: the Fill/Stroke buttons in the toolbar."));
    hintL->setWordWrap(true);
    hintL->setStyleSheet("color: gray; font-size: 9pt;");
    cv->addWidget(hintL);
    cw->setMaximumHeight(cw->sizeHint().height() + 10);
    auto* cdock = new QDockWidget(tr("Colours"), this);
    cdock->setObjectName("colours");
    cdock->setWidget(cw);
    addDockWidget(Qt::RightDockWidgetArea, cdock);

    // ---------------- properties
    auto* pw = new QWidget;
    buildPropertyPanel(pw);
    auto* sa = new QScrollArea;
    sa->setWidget(pw);
    sa->setWidgetResizable(true);
    auto* dock = new QDockWidget(tr("Properties"), this);
    dock->setObjectName("props");
    dock->setWidget(sa);
    dock->setMinimumWidth(280);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    // ---------------- library
    auto* lw = new QWidget;
    auto* lv = new QVBoxLayout(lw);
    lv->setContentsMargins(4, 4, 4, 4);
    auto* lbar = new QHBoxLayout;
    auto* libAdd = new QPushButton(tr("Add selection"));
    libAdd->setToolTip(tr("Save the selected shapes as a library item (Ctrl+B)"));
    connect(libAdd, &QPushButton::clicked, this, [this] { libraryAdd(); });
    auto* libIns = new QPushButton(tr("Insert"));
    connect(libIns, &QPushButton::clicked, m_actLibInsert, &QAction::trigger);
    auto* libFolder = new QToolButton;
    libFolder->setText(QStringLiteral("…"));
    libFolder->setToolTip(tr("Open the library folder (items are .tood files you can copy or share)"));
    connect(libFolder, &QToolButton::clicked, this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(libraryDir())); });
    lbar->addWidget(libAdd);
    lbar->addWidget(libIns);
    lbar->addStretch();
    lbar->addWidget(libFolder);
    lv->addLayout(lbar);
    m_library = new LibraryList;
    m_library->setViewMode(QListView::IconMode);
    m_library->setIconSize(QSize(96, 72));
    m_library->setGridSize(QSize(112, 100));
    m_library->setResizeMode(QListView::Adjust);
    m_library->setMovement(QListView::Static);
    m_library->setWordWrap(true);
    m_library->setDragEnabled(true);
    m_library->setDragDropMode(QAbstractItemView::DragOnly);
    m_library->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_library, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        libraryInsert(it->data(Qt::UserRole).toString());
    });
    connect(m_library, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pt) {
        QMenu menu;
        if (m_library->itemAt(pt)) {
            menu.addAction(m_actLibInsert);
            menu.addAction(tr("Rename…"), this, [this] { libraryRename(); });
            menu.addAction(tr("Delete"), this, [this] { libraryDelete(); });
            menu.addSeparator();
        }
        menu.addAction(m_actLibAdd);
        menu.addAction(tr("Refresh"), this, [this] { refreshLibrary(); });
        menu.exec(m_library->viewport()->mapToGlobal(pt));
    });
    lv->addWidget(m_library, 1);
    auto* ldock = new QDockWidget(tr("Library"), this);
    ldock->setObjectName("library");
    ldock->setWidget(lw);
    addDockWidget(Qt::RightDockWidgetArea, ldock);
    tabifyDockWidget(dock, ldock);
    dock->raise();
    refreshLibrary();

    // ---------------- timeline
    auto* tw = new QWidget;
    auto* tv = new QVBoxLayout(tw);
    tv->setContentsMargins(4, 2, 4, 2);
    // chapter + navigation row
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Chapter:")));
    m_chapterCombo = new QComboBox;
    m_chapterCombo->setMinimumWidth(200);
    m_chapterCombo->setToolTip(tr("Chapter being edited (Ctrl+PgUp / Ctrl+PgDn)"));
    connect(m_chapterCombo, &QComboBox::currentIndexChanged, this, [this](int c) {
        if (!m_updatingTimeline && c >= 0) switchChapter(c);
    });
    top->addWidget(m_chapterCombo);
    for (QAction* a : {m_actChNew, m_actChRename, m_actChDel}) {
        auto* b = new QToolButton;
        b->setDefaultAction(a);
        b->setText(a == m_actChNew ? QStringLiteral("+") : a == m_actChDel ? QStringLiteral("✕") : tr("Rename"));
        top->addWidget(b);
    }
    top->addSpacing(16);
    top->addWidget(new QLabel(tr("Go to frame:")));
    m_goto = new QSpinBox;
    m_goto->setRange(1, 1);
    m_goto->setKeyboardTracking(false);
    m_goto->setToolTip(tr("Type a frame number and press Enter"));
    connect(m_goto, &QSpinBox::valueChanged, this, [this](int v) {
        if (!m_updatingTimeline) setCurrentFrame(v - 1);
    });
    top->addWidget(m_goto);
    top->addSpacing(16);
    top->addWidget(new QLabel(tr("Thumbnails:")));
    auto* thumbSize = new QSlider(Qt::Horizontal);
    thumbSize->setRange(40, 200);
    thumbSize->setValue(QSettings().value("thumbSize", 128).toInt());
    thumbSize->setMaximumWidth(140);
    thumbSize->setToolTip(tr("Thumbnail size (small = more frames visible)"));
    connect(thumbSize, &QSlider::valueChanged, this, [this](int w) {
        QSettings().setValue("thumbSize", w);
        m_timeline->setIconSize(QSize(w, w * 3 / 4));
        m_timeline->setGridSize(QSize(w + 12, w * 3 / 4 + 26));
        m_thumbs.clear();
        for (int i = 0; i < m_timeline->count(); ++i) m_timeline->item(i)->setData(Qt::UserRole, QVariant());
        refreshTimeline();
    });
    top->addWidget(thumbSize);
    top->addStretch();
    tv->addLayout(top);

    auto* bar = new QHBoxLayout;
    auto addBtn = [bar](QAction* a) {
        auto* b = new QToolButton;
        b->setDefaultAction(a);
        b->setToolButtonStyle(a->icon().isNull() ? Qt::ToolButtonTextOnly : Qt::ToolButtonTextBesideIcon);
        bar->addWidget(b);
    };
    for (QAction* a : {m_actFrameNew, m_actFrameBlank, m_actFrameDup, m_actFrameDel, m_actFrameLeft, m_actFrameRight, m_actTween})
        addBtn(a);
    bar->addSpacing(12);
    bar->addWidget(new QLabel(tr("Duration:")));
    m_dur = new QSpinBox;
    m_dur->setRange(10, 60000);
    m_dur->setSingleStep(10);
    m_dur->setSuffix(tr(" ms"));
    m_dur->setKeyboardTracking(false);
    m_dur->setToolTip(tr("Display time of the selected frame(s)"));
    connect(m_dur, &QSpinBox::valueChanged, this, [this](int ms) {
        if (!m_updatingProps) setFrameDurations(ms, false);
    });
    bar->addWidget(m_dur);
    auto* allBtn = new QPushButton(tr("All"));
    allBtn->setToolTip(tr("Apply this duration to all frames"));
    connect(allBtn, &QPushButton::clicked, this, [this] { setFrameDurations(m_dur->value(), true); });
    bar->addWidget(allBtn);
    bar->addSpacing(12);
    addBtn(m_actPlay);
    m_loop = new QCheckBox(tr("Loop"));
    m_loop->setChecked(true);
    bar->addWidget(m_loop);
    bar->addStretch();
    m_timeInfo = new QLabel;
    bar->addWidget(m_timeInfo);
    tv->addLayout(bar);

    m_timeline = new QListWidget;
    m_timeline->setViewMode(QListView::IconMode);
    m_timeline->setFlow(QListView::LeftToRight);
    m_timeline->setWrapping(false);
    m_timeline->setMovement(QListView::Static);
    {
        const int w = thumbSize->value();
        m_timeline->setIconSize(QSize(w, w * 3 / 4));
        m_timeline->setGridSize(QSize(w + 12, w * 3 / 4 + 26));
    }
    m_timeline->setLayoutMode(QListView::Batched);  // stays responsive with thousands of frames
    m_timeline->setBatchSize(200);
    m_timeline->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_timeline->setSpacing(4);
    m_timeline->setMinimumHeight(150);
    m_timeline->setContextMenuPolicy(Qt::ActionsContextMenu);
    m_timeline->addActions({m_actFrameNew, m_actFrameBlank, m_actFrameDup, m_actFrameDel, m_actTween});
    connect(m_timeline, &QListWidget::currentRowChanged, this, [this](int r) {
        if (!m_updatingTimeline && r >= 0) setCurrentFrame(r, true);
    });
    connect(m_timeline->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!m_thumbTimer.isActive()) m_thumbTimer.start(60);  // render the newly visible thumbnails first
    });
    tv->addWidget(m_timeline);

    auto* tdock = new QDockWidget(tr("Frames"), this);
    tdock->setObjectName("frames");
    tdock->setWidget(tw);
    addDockWidget(Qt::BottomDockWidgetArea, tdock);

    // ---------------- info / messages
    auto* iw = new QWidget;
    auto* iv = new QVBoxLayout(iw);
    iv->setContentsMargins(6, 4, 6, 4);
    m_prompt = new QLabel;
    m_prompt->setWordWrap(true);
    m_prompt->setStyleSheet("QLabel { background: #fff6d5; color: #222; border: 1px solid #e0c870; border-radius: 4px; padding: 5px; }");
    m_prompt->setMinimumHeight(48);
    m_prompt->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    iv->addWidget(m_prompt);
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    m_log->setPlaceholderText(tr("Messages"));
    iv->addWidget(m_log, 1);
    auto* idock = new QDockWidget(tr("Info"), this);
    idock->setObjectName("info");
    idock->setWidget(iw);
    addDockWidget(Qt::BottomDockWidgetArea, idock);
    splitDockWidget(tdock, idock, Qt::Horizontal);
}

void MainWindow::buildPropertyPanel(QWidget* pw)
{
    auto* v = new QVBoxLayout(pw);
    m_hint = new QLabel(tr("Select a shape, or click a connection marker\n(orange hinge, green free, red weld, purple elastic)."));
    m_hint->setWordWrap(true);
    v->addWidget(m_hint);

    m_shapeBox = new QGroupBox(tr("Shape"));
    m_form = new QFormLayout(m_shapeBox);
    m_typeLabel = new QLabel;
    m_groupLabel = new QLabel;
    m_name = new QLineEdit;
    m_x = dspin(-1e6, 1e6, 1, 1);
    m_y = dspin(-1e6, 1e6, 1, 1);
    m_w = dspin(1, 1e6, 1, 1);
    m_h = dspin(1, 1e6, 1, 1);
    m_rx = dspin(0.5, 1e6, 1, 1);
    m_ry = dspin(0.5, 1e6, 1, 1);
    m_lock = new QCheckBox(tr("Lock aspect"));
    m_rot = dspin(-36000, 36000, 1, 1, QStringLiteral("°"));
    m_sq = dspin(10, 1000, 5, 0, QStringLiteral(" %"));
    m_sq->setToolTip(tr("Stretch along the angle below (area is kept: the other direction squashes)"));
    m_sqa = dspin(-360, 360, 5, 1, QStringLiteral("°"));
    m_resetWarp = new QPushButton(tr("Reset warp"));
    m_fill = new ColorButton(tr("Fill colour"));
    m_stroke = new ColorButton(tr("Stroke colour"));
    m_accent = new ColorButton(tr("Pupil colour"));
    m_sw = dspin(0, 500, 0.5, 1, tr(" px"));
    m_op = dspin(0, 100, 5, 0, QStringLiteral(" %"));
    m_cr = dspin(0, 1e5, 1, 1);
    m_sides = new QSpinBox;
    m_sides->setRange(1, 64);
    m_sides->setKeyboardTracking(false);
    m_inner = dspin(0, 1, 0.05, 2);
    m_lineStyle = new QComboBox;
    m_lineStyle->addItems({tr("Straight"), tr("Spring (zigzag)")});
    m_amp = dspin(1, 500, 1, 1, tr(" px"));
    m_endsLabel = new QLabel;
    m_endsLabel->setWordWrap(true);
    auto* detach = new QPushButton(tr("Unglue"));
    m_endsRow = new QWidget;
    auto* er = new QHBoxLayout(m_endsRow);
    er->setContentsMargins(0, 0, 0, 0);
    er->addWidget(m_endsLabel, 1);
    er->addWidget(detach);
    m_text = new QLineEdit;
    m_font = new QFontComboBox;
    m_fs = dspin(1, 2000, 1, 0, tr(" px"));
    m_bold = new QCheckBox(tr("Bold"));
    m_italic = new QCheckBox(tr("Italic"));
    auto* bi = new QWidget;
    auto* bil = new QHBoxLayout(bi);
    bil->setContentsMargins(0, 0, 0, 0);
    bil->addWidget(m_bold);
    bil->addWidget(m_italic);
    bil->addStretch();
    m_align = new QComboBox;
    m_align->addItems({tr("Centre"), tr("Left"), tr("Right")});

    m_form->addRow(tr("Type"), m_typeLabel);
    m_form->addRow(tr("Name"), m_name);
    m_form->addRow(tr("Group"), m_groupLabel);
    m_form->addRow(tr("X"), m_x);
    m_form->addRow(tr("Y"), m_y);
    m_form->addRow(tr("Width"), m_w);
    m_form->addRow(tr("Height"), m_h);
    m_form->addRow(tr("Radius X"), m_rx);
    m_form->addRow(tr("Radius Y"), m_ry);
    m_form->addRow(QString(), m_lock);
    m_form->addRow(tr("Rotation"), m_rot);
    m_form->addRow(tr("Stretch"), m_sq);
    m_form->addRow(tr("Stretch angle"), m_sqa);
    m_form->addRow(tr("Warp"), m_resetWarp);
    m_form->addRow(tr("Fill"), m_fill);
    m_form->addRow(tr("Stroke"), m_stroke);
    m_form->addRow(tr("Pupil"), m_accent);
    m_form->addRow(tr("Stroke width"), m_sw);
    m_form->addRow(tr("Opacity"), m_op);
    m_form->addRow(tr("Corner radius"), m_cr);
    m_form->addRow(tr("Sides"), m_sides);
    m_form->addRow(tr("Inner ratio"), m_inner);
    m_form->addRow(tr("Line style"), m_lineStyle);
    m_form->addRow(tr("Spring width"), m_amp);
    m_form->addRow(tr("Ends"), m_endsRow);
    m_form->addRow(tr("Text"), m_text);
    m_form->addRow(tr("Font"), m_font);
    m_form->addRow(tr("Font size"), m_fs);
    m_form->addRow(QString(), bi);
    m_form->addRow(tr("Align"), m_align);
    v->addWidget(m_shapeBox);

    m_jointBox = new QGroupBox(tr("Connection"));
    m_jForm = new QFormLayout(m_jointBox);
    m_jType = new QComboBox;
    for (int i = 0; i < 4; ++i) m_jType->addItem(jointTypeName(JointType(i)));
    m_jLim = new QCheckBox(tr("Limit rotation"));
    m_jMin = dspin(-180, 180, 5, 1, QStringLiteral("°"));
    m_jMax = dspin(-180, 180, 5, 1, QStringLiteral("°"));
    m_jStiff = dspin(0.05, 1, 0.05, 2);
    m_jStiff->setToolTip(tr("Low = very stretchy and slow, 1 = stiff"));
    m_jInfo = new QLabel;
    m_jInfo->setWordWrap(true);
    auto* jRev = new QPushButton(tr("Reverse direction"));
    jRev->setToolTip(tr("Swap which side is the parent"));
    auto* jDel = new QPushButton(tr("Delete"));
    auto* jb = new QHBoxLayout;
    jb->addWidget(jRev);
    jb->addWidget(jDel);
    m_jForm->addRow(tr("Type"), m_jType);
    m_jForm->addRow(QString(), m_jLim);
    m_jForm->addRow(tr("Min angle"), m_jMin);
    m_jForm->addRow(tr("Max angle"), m_jMax);
    m_jForm->addRow(tr("Stiffness"), m_jStiff);
    m_jForm->addRow(m_jInfo);
    m_jForm->addRow(jb);
    v->addWidget(m_jointBox);
    v->addStretch();

    // ---- edits
    connect(m_name, &QLineEdit::editingFinished, this, [this] {
        const QString t = m_name->text();
        editSingle([t](Frame&, int, Shape& s) { s.name = t; }, tr("Rename"), 0);
    });
    connect(m_x, &QDoubleSpinBox::valueChanged, this, [this](double val) {
        editSingle([val](Frame& f, int id, Shape& s) { Solver::translateShapes(f, {id}, QPointF(val - s.pos.x(), 0)); }, tr("Move"), 1);
    });
    connect(m_y, &QDoubleSpinBox::valueChanged, this, [this](double val) {
        editSingle([val](Frame& f, int id, Shape& s) { Solver::translateShapes(f, {id}, QPointF(0, val - s.pos.y())); }, tr("Move"), 2);
    });
    auto setW = [](Shape& s, double w) {
        if (s.lockAspect && s.w > 0) s.h *= w / s.w;
        s.w = w;
    };
    auto setH = [](Shape& s, double h) {
        if (s.lockAspect && s.h > 0) s.w *= h / s.h;
        s.h = h;
    };
    connect(m_w, &QDoubleSpinBox::valueChanged, this, [this, setW](double val) {
        editSingle([=](Frame& f, int id, Shape& s) { setW(s, val); Solver::enforce(f, id); }, tr("Resize"), 3);
    });
    connect(m_h, &QDoubleSpinBox::valueChanged, this, [this, setH](double val) {
        editSingle([=](Frame& f, int id, Shape& s) { setH(s, val); Solver::enforce(f, id); }, tr("Resize"), 4);
    });
    connect(m_rx, &QDoubleSpinBox::valueChanged, this, [this, setW](double val) {
        editSingle([=](Frame& f, int id, Shape& s) { setW(s, 2 * val); Solver::enforce(f, id); }, tr("Radius"), 3);
    });
    connect(m_ry, &QDoubleSpinBox::valueChanged, this, [this, setH](double val) {
        editSingle([=](Frame& f, int id, Shape& s) { setH(s, 2 * val); Solver::enforce(f, id); }, tr("Radius"), 4);
    });
    connect(m_lock, &QCheckBox::toggled, this, [this](bool b) {
        editAll([b](Shape& s) {
            s.lockAspect = b;
            if (b && s.type == ShapeType::Ellipse) s.h = s.w;  // circle
        }, tr("Lock aspect"), 20);
    });
    connect(m_rot, &QDoubleSpinBox::valueChanged, this, [this](double val) {
        editSingle([val](Frame& f, int id, Shape& s) { Solver::rotateShape(f, id, val - s.rot); }, tr("Rotate"), 5);
    });
    connect(m_sq, &QDoubleSpinBox::valueChanged, this, [this](double val) { editAll([val](Shape& s) { s.squash = val / 100.0; }, tr("Stretch"), 21); });
    connect(m_sqa, &QDoubleSpinBox::valueChanged, this, [this](double val) { editAll([val](Shape& s) { s.squashAngle = val; }, tr("Stretch angle"), 22); });
    connect(m_resetWarp, &QPushButton::clicked, this, [this] {
        editAll([](Shape& s) { for (QPointF& w : s.warp) w = QPointF(); }, tr("Reset warp"), -1);
    });
    m_fill->onChanged = [this](const QColor& c) {
        m_palette->addRecent(c);
        editAll([c](Shape& s) { s.fill = c; }, tr("Fill colour"), 6);
    };
    m_stroke->onChanged = [this](const QColor& c) {
        m_palette->addRecent(c);
        editAll([c](Shape& s) { s.stroke = c; }, tr("Stroke colour"), 7);
    };
    m_accent->onChanged = [this](const QColor& c) {
        m_palette->addRecent(c);
        editAll([c](Shape& s) { s.accent = c; }, tr("Pupil colour"), 27);
    };
    connect(m_sw, &QDoubleSpinBox::valueChanged, this, [this](double val) { editAll([val](Shape& s) { s.strokeWidth = val; }, tr("Stroke width"), 8); });
    connect(m_op, &QDoubleSpinBox::valueChanged, this, [this](double val) { editAll([val](Shape& s) { s.opacity = val / 100.0; }, tr("Opacity"), 9); });
    connect(m_cr, &QDoubleSpinBox::valueChanged, this, [this](double val) { editAll([val](Shape& s) { s.cornerRadius = val; }, tr("Corner radius"), 12); });
    connect(m_sides, &QSpinBox::valueChanged, this, [this](int val) { editAll([val](Shape& s) { s.sides = val; }, tr("Sides"), 13); });
    connect(m_inner, &QDoubleSpinBox::valueChanged, this, [this](double val) { editAll([val](Shape& s) { s.innerRatio = val; }, tr("Inner ratio"), 14); });
    connect(m_lineStyle, &QComboBox::currentIndexChanged, this, [this](int i) {
        editAll([i](Shape& s) {
            s.lineStyle = i;
            if (i == 1 && s.sides < 2) s.sides = 8;
        }, tr("Line style"), 23);
    });
    connect(m_amp, &QDoubleSpinBox::valueChanged, this, [this](double val) { editAll([val](Shape& s) { s.amplitude = val; }, tr("Spring width"), 24); });
    connect(detach, &QPushButton::clicked, this, [this] {
        editAll([](Shape& s) { s.ends[0] = s.ends[1] = EndLink(); }, tr("Unglue line"), -1);
    });
    connect(m_text, &QLineEdit::editingFinished, this, [this] {
        const QString t = m_text->text();
        const auto ids = m_canvas->selectedIds();
        if (ids.size() != 1) return;
        const Shape* s = m_doc.frames.at(m_cur).find(ids[0]);
        if (!s || s->text == t) return;
        editSingle([t](Frame& f, int id, Shape& s) { s.text = t; fitTextShape(s); Solver::enforce(f, id); }, tr("Edit text"), 15);
    });
    connect(m_font, &QFontComboBox::currentFontChanged, this, [this](const QFont& fnt) {
        const QString fam = fnt.family();
        editAll([fam](Shape& s) { s.fontFamily = fam; }, tr("Font"), 16);
    });
    connect(m_fs, &QDoubleSpinBox::valueChanged, this, [this](double val) { editAll([val](Shape& s) { s.fontSize = val; }, tr("Font size"), 17); });
    connect(m_bold, &QCheckBox::toggled, this, [this](bool b) { editAll([b](Shape& s) { s.bold = b; }, tr("Bold"), 18); });
    connect(m_italic, &QCheckBox::toggled, this, [this](bool b) { editAll([b](Shape& s) { s.italic = b; }, tr("Italic"), 25); });
    connect(m_align, &QComboBox::currentIndexChanged, this, [this](int i) { editAll([i](Shape& s) { s.align = i; }, tr("Align"), 26); });

    connect(m_jType, &QComboBox::currentIndexChanged, this, [this](int i) {
        editJoint([i](Frame&, Joint& j) { j.type = JointType(i); }, tr("Connection type"), 30);
    });
    connect(m_jLim, &QCheckBox::toggled, this, [this](bool b) { editJoint([b](Frame&, Joint& j) { j.limited = b; }, tr("Rotation limit"), 31); });
    connect(m_jMin, &QDoubleSpinBox::valueChanged, this, [this](double val) {
        editJoint([val](Frame&, Joint& j) { j.minAngle = qMin(val, j.maxAngle); }, tr("Rotation limit"), 32);
    });
    connect(m_jMax, &QDoubleSpinBox::valueChanged, this, [this](double val) {
        editJoint([val](Frame&, Joint& j) { j.maxAngle = qMax(val, j.minAngle); }, tr("Rotation limit"), 33);
    });
    connect(m_jStiff, &QDoubleSpinBox::valueChanged, this, [this](double val) {
        editJoint([val](Frame&, Joint& j) { j.stiffness = val; }, tr("Stiffness"), 34);
    });
    connect(jRev, &QPushButton::clicked, this, [this] {
        editJoint([](Frame& f, Joint& j) { Solver::reroot(f, j.child); }, tr("Reverse connection"), -1);
    });
    connect(jDel, &QPushButton::clicked, this, [this] { deleteSel(); });
}

// ================================================================ state

void MainWindow::log(const QString& text)
{
    m_log->appendPlainText(QTime::currentTime().toString("HH:mm:ss  ") + text);
    statusBar()->showMessage(text, 5000);
}

void MainWindow::setPrompt(const QString& text) { m_prompt->setText(text); }

void MainWindow::commit(const QString& text, int mergeId)
{
    m_canvas->finishSpring(false);
    m_undo->push(new SnapshotCmd(this, m_committed, m_committedFrame, m_doc, m_cur, text, mergeId));
    m_committed = m_doc;
    m_committedFrame = m_cur;
    refreshTimeline();
    updateTitle();
}

void MainWindow::applySnapshot(const Document& d, int frame)
{
    m_canvas->finishSpring(false);
    if (m_playing) togglePlay();
    const bool geom = d.width != m_doc.width || d.height != m_doc.height || d.background != m_doc.background;
    m_doc = d;
    m_committed = d;
    m_cur = qBound(0, frame, m_doc.frames.size() - 1);
    m_committedFrame = m_cur;
    if (geom) {
        m_thumbs.clear();
        m_canvas->resetScene();
    }
    m_canvas->setFrameIndex(m_cur);
    refreshChapters();
    refreshTimeline();
    setCurrentFrame(m_cur);
}

void MainWindow::setCurrentFrame(int i, bool fromTimeline)
{
    m_canvas->finishSpring(true);
    i = qBound(0, i, m_doc.frames.size() - 1);
    if (i != m_cur || m_canvas->frameIndex() != i) {
        m_cur = i;
        m_committedFrame = i;
        m_canvas->setFrameIndex(i);
    }
    m_updatingTimeline = true;
    if (!fromTimeline) m_timeline->setCurrentRow(i, QItemSelectionModel::ClearAndSelect);  // only the current frame is marked
    m_timeline->scrollToItem(m_timeline->item(i));
    m_goto->setValue(i + 1);
    m_updatingTimeline = false;
    m_updatingProps = true;
    m_dur->setValue(m_doc.frames.at(i).durationMs);
    m_updatingProps = false;
    refreshProps();
}

void MainWindow::refreshTimeline()
{
    m_updatingTimeline = true;
    const int n = m_doc.frames.size();
    while (m_timeline->count() > n) delete m_timeline->takeItem(m_timeline->count() - 1);
    while (m_timeline->count() < n) m_timeline->addItem(new QListWidgetItem);
    bool missing = false;
    qint64 total = 0;
    for (int i = 0; i < n; ++i) {
        const Frame& f = m_doc.frames.at(i);
        total += f.durationMs;
        QListWidgetItem* it = m_timeline->item(i);
        const QString txt = QStringLiteral("%1 · %2 ms").arg(i + 1).arg(f.durationMs);
        if (it->text() != txt) it->setText(txt);
        const void* key = f.shapes.constData();
        const quintptr k = quintptr(key);
        if (it->data(Qt::UserRole).value<quintptr>() == k && !it->icon().isNull()) continue;
        auto th = m_thumbs.constFind(key);
        if (th != m_thumbs.constEnd()) {
            it->setIcon(th->icon);
            it->setData(Qt::UserRole, QVariant::fromValue(k));
        } else {
            missing = true;
        }
    }
    if (m_timeline->currentRow() != m_cur) m_timeline->setCurrentRow(m_cur, QItemSelectionModel::ClearAndSelect);
    m_goto->setRange(1, n);
    m_goto->setSuffix(QStringLiteral(" / %1").arg(n));
    m_updatingTimeline = false;

    qint64 all = 0;
    for (int c = 0; c < m_doc.chapters.size(); ++c)
        for (const Frame& f : m_doc.chapterFrames(c)) all += f.durationMs;
    QString info = tr("%1 frames · %2 s").arg(n).arg(total / 1000.0, 0, 'f', 2);
    if (m_doc.chapters.size() > 1)
        info += tr("   (project: %1 chapters, %2 frames, %3 s)").arg(m_doc.chapters.size()).arg(m_doc.totalFrames()).arg(all / 1000.0, 0, 'f', 1);
    m_timeInfo->setText(info);
    if (missing && !m_thumbTimer.isActive()) m_thumbTimer.start(100);
}

// Renders missing thumbnails in small time slices, visible frames first, so a
// chapter with thousands of frames never blocks the UI.
void MainWindow::renderThumbs()
{
    const QSize ts = m_timeline->iconSize();
    const double sc = qMin(ts.width() / double(m_doc.width), ts.height() / double(m_doc.height));
    if (m_thumbs.size() > m_doc.frames.size() * 2 + 256) {  // prune entries no frame uses
        QHash<const void*, Thumb> keep;
        for (const Frame& f : m_doc.frames) {
            auto it = m_thumbs.constFind(f.shapes.constData());
            if (it != m_thumbs.constEnd()) keep.insert(it.key(), it.value());
        }
        m_thumbs.swap(keep);
    }
    const int n = m_doc.frames.size();
    if (n == 0) return;
    int first = m_timeline->indexAt(QPoint(8, m_timeline->viewport()->height() / 2)).row();
    if (first < 0) first = m_cur;
    QElapsedTimer timer;
    timer.start();
    bool more = false;
    for (int k = 0; k < n; ++k) {
        const Frame& f = m_doc.frames.at((first + k) % n);
        const void* key = f.shapes.constData();
        if (m_thumbs.contains(key)) continue;
        if (timer.elapsed() > 30) {
            more = true;
            break;
        }
        const QImage img = renderFrameImage(m_doc, f, sc, true, Qt::white);
        m_thumbs.insert(key, Thumb{f.shapes, QIcon(QPixmap::fromImage(img))});
    }
    refreshTimeline();
    if (more) m_thumbTimer.start(5);
}

void MainWindow::refreshProps()
{
    m_updatingProps = true;
    const Frame& f = m_doc.frames.at(m_cur);
    const QVector<int> ids = m_canvas->selectedIds();
    const Shape* s = ids.isEmpty() ? nullptr : f.find(ids.first());
    const int ji = f.jointIndex(m_canvas->selectedJoint());
    m_shapeBox->setVisible(s);
    m_jointBox->setVisible(!s && ji >= 0);
    m_hint->setVisible(!s && ji < 0);
    if (s) {
        const bool single = ids.size() == 1;
        m_shapeBox->setTitle(single ? tr("Shape") : tr("%1 shapes").arg(ids.size()));
        m_typeLabel->setText(single ? QStringLiteral("%1 #%2").arg(shapeTypeName(s->type)).arg(s->id) : tr("(multiple)"));
        m_groupLabel->setText(s->group ? tr("#%1 (Alt+click picks one member)").arg(s->group) : tr("none"));
        m_name->setText(s->name);
        m_x->setValue(s->pos.x());
        m_y->setValue(s->pos.y());
        m_w->setValue(s->w);
        m_h->setValue(s->h);
        m_rx->setValue(s->w / 2);
        m_ry->setValue(s->h / 2);
        m_lock->setChecked(s->lockAspect);
        m_rot->setValue(s->rot);
        m_sq->setValue(s->squash * 100);
        m_sqa->setValue(s->squashAngle);
        for (QWidget* w : std::initializer_list<QWidget*>{m_name, m_x, m_y, m_w, m_h, m_rx, m_ry, m_rot, m_text}) w->setEnabled(single);
        m_fill->setColor(s->fill);
        m_stroke->setColor(s->stroke);
        m_accent->setColor(s->accent);
        m_sw->setValue(s->strokeWidth);
        m_op->setValue(s->opacity * 100);
        m_cr->setValue(s->cornerRadius);
        m_sides->setValue(s->sides);
        m_inner->setValue(s->innerRatio);
        m_lineStyle->setCurrentIndex(s->lineStyle);
        m_amp->setValue(s->amplitude);
        m_text->setText(s->text);
        m_font->setCurrentFont(s->fontFamily.isEmpty() ? QFont() : QFont(s->fontFamily));
        m_fs->setValue(s->fontSize);
        m_bold->setChecked(s->bold);
        m_italic->setChecked(s->italic);
        m_align->setCurrentIndex(s->align);

        const ShapeType t = s->type;
        const bool text = t == ShapeType::Text, img = t == ShapeType::Image, lin = s->isLinear();
        const bool pt = t == ShapeType::Point, ell = t == ShapeType::Ellipse;
        auto row = [this](QWidget* w, bool on) { m_form->setRowVisible(w, on); };
        auto label = [this](QWidget* w, const QString& l) {
            if (QWidget* lw = m_form->labelForField(w)) lw->setProperty("text", l);
        };
        row(m_w, !ell && !pt);
        row(m_h, !ell && !lin && !pt);
        row(m_rx, ell);
        row(m_ry, ell);
        row(m_lock, !lin && !pt);
        m_lock->setText(ell ? tr("Circle (radius X = Y)") : tr("Lock aspect"));
        row(m_sq, !lin && !pt);
        row(m_sqa, !lin && !pt);
        row(m_resetWarp, s->hasWarp());
        row(m_fill, !lin && !img && !pt);
        label(m_fill, text ? tr("Colour") : tr("Fill"));
        row(m_stroke, !text && !pt);
        row(m_accent, t == ShapeType::Eye);
        row(m_sw, !text && !pt);
        row(m_op, !pt);
        row(m_cr, t == ShapeType::Rect);
        const bool hasSides = t == ShapeType::Polygon || t == ShapeType::Star || t == ShapeType::Diamond || (lin && s->lineStyle == 1);
        row(m_sides, hasSides);
        label(m_sides, lin ? tr("Coils") : t == ShapeType::Star ? tr("Points") : t == ShapeType::Diamond ? tr("Corners") : tr("Sides"));
        row(m_inner, t == ShapeType::Star || t == ShapeType::Hourglass || t == ShapeType::Eye);
        label(m_inner, t == ShapeType::Hourglass ? tr("Waist") : t == ShapeType::Eye ? tr("Iris size") : tr("Inner ratio"));
        row(m_lineStyle, lin);
        row(m_amp, lin && s->lineStyle == 1);
        row(m_endsRow, lin);
        if (lin) {
            auto endName = [&](const EndLink& l) { return l.target ? shapeLabel(f.find(l.target)) : tr("free"); };
            m_endsLabel->setText(tr("start: %1\nend: %2").arg(endName(s->ends[0]), endName(s->ends[1])));
        }
        for (QWidget* w : std::initializer_list<QWidget*>{m_text, m_font, m_fs, m_align}) row(w, text);
        row(m_bold->parentWidget(), text);
    }
    if (!s && ji >= 0) {
        const Joint& j = f.joints.at(ji);
        m_jType->setCurrentIndex(int(j.type));
        const bool hinge = j.type == JointType::Hinge;
        m_jLim->setChecked(j.limited);
        m_jMin->setValue(j.minAngle);
        m_jMax->setValue(j.maxAngle);
        m_jStiff->setValue(j.stiffness);
        m_jForm->setRowVisible(m_jLim, hinge);
        m_jForm->setRowVisible(m_jMin, hinge && j.limited);
        m_jForm->setRowVisible(m_jMax, hinge && j.limited);
        m_jForm->setRowVisible(m_jStiff, j.type == JointType::Elastic);
        const Shape* p = f.find(j.parent);
        const Shape* c = f.find(j.child);
        const double rel = (p && c) ? Solver::normDeg(c->rot - p->rot - j.restAngle) : 0.0;
        m_jInfo->setText(tr("Parent: %1\nChild: %2\nCurrent angle: %3°\n\nDrag the marker to move the pivot.")
                             .arg(shapeLabel(p), shapeLabel(c))
                             .arg(rel, 0, 'f', 1));
    }
    m_updatingProps = false;
}

void MainWindow::updateTitle()
{
    const QString name = m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).fileName();
    setWindowTitle(QStringLiteral("%1[*] — TooD").arg(name));
    setWindowModified(!m_undo->isClean());
}

bool MainWindow::maybeSave()
{
    if (m_undo->isClean()) return true;
    const auto r = QMessageBox::question(this, tr("Unsaved changes"), tr("Save changes to the project?"),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Save) return saveDoc();
    return r == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (maybeSave()) e->accept();
    else e->ignore();
}

QVector<int> MainWindow::selectedFrames() const
{
    QVector<int> rows;
    for (const QModelIndex& ix : m_timeline->selectionModel()->selectedIndexes()) rows << ix.row();
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    if (rows.isEmpty()) rows << m_cur;
    return rows;
}

QString MainWindow::baseName() const
{
    if (m_path.isEmpty()) return QDir::home().filePath(QStringLiteral("untitled"));
    const QFileInfo fi(m_path);
    return fi.dir().filePath(fi.completeBaseName());
}

void MainWindow::addRecentFile(const QString& path)
{
    QSettings st;
    QStringList r = st.value("recent").toStringList();
    r.removeAll(path);
    r.prepend(path);
    while (r.size() > 10) r.removeLast();
    st.setValue("recent", r);
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu()
{
    m_recentMenu->clear();
    const QStringList r = QSettings().value("recent").toStringList();
    for (const QString& p : r) {
        QAction* a = m_recentMenu->addAction(QFileInfo(p).fileName());
        a->setToolTip(p);
        connect(a, &QAction::triggered, this, [this, p] {
            if (maybeSave()) openPath(p);
        });
    }
    m_recentMenu->setEnabled(!r.isEmpty());
}

// ================================================================ file

void MainWindow::newDoc()
{
    if (!maybeSave()) return;
    showStartDialog(false);
}

bool MainWindow::openPath(const QString& path)
{
    Document d;
    QString err;
    if (!Document::load(path, d, &err)) {
        QMessageBox::warning(this, tr("Open failed"), tr("Could not open %1:\n%2").arg(path, err));
        return false;
    }
    m_path = path;
    m_thumbs.clear();
    applySnapshot(d, 0);
    m_canvas->resetScene();
    m_undo->clear();
    updateTitle();
    m_canvas->fitToCanvas();
    addRecentFile(path);
    log(tr("Opened %1 (%2 frames)").arg(path).arg(d.frames.size()));
    return true;
}

void MainWindow::openDoc()
{
    if (!maybeSave()) return;
    const QString p = QFileDialog::getOpenFileName(this, tr("Open project"), QFileInfo(baseName()).absolutePath(),
                                                   tr("TooD projects (*.tood);;All files (*)"));
    if (!p.isEmpty()) openPath(p);
}

bool MainWindow::saveDoc()
{
    if (m_path.isEmpty()) return saveDocAs();
    m_canvas->finishSpring(true);
    QString err;
    if (!m_doc.save(m_path, &err)) {
        QMessageBox::warning(this, tr("Save failed"), err);
        return false;
    }
    m_undo->setClean();
    updateTitle();
    addRecentFile(m_path);
    log(tr("Saved %1").arg(m_path));
    return true;
}

bool MainWindow::saveDocAs()
{
    QString p = QFileDialog::getSaveFileName(this, tr("Save project as"), baseName() + ".tood", tr("TooD projects (*.tood)"));
    if (p.isEmpty()) return false;
    if (QFileInfo(p).suffix().isEmpty()) p += ".tood";
    m_path = p;
    return saveDoc();
}

void MainWindow::insertImage(const QString& pathIn, QPointF at)
{
    QString path = pathIn;
    if (path.isEmpty())
        path = QFileDialog::getOpenFileName(this, tr("Insert image"), QFileInfo(baseName()).absolutePath(),
                                            tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.svg);;All files (*)"));
    if (path.isEmpty()) return;
    if (path.endsWith(".tood")) {
        if (maybeSave()) openPath(path);
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = file.readAll();
    QImage img;
    if (!img.loadFromData(bytes)) {
        QImageReader r(path);  // fall back to format detection by extension (e.g. svg)
        img = r.read();
    }
    if (img.isNull()) {
        log(tr("Not an image: %1").arg(path));
        return;
    }
    const QString key = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex().left(24));
    if (!m_doc.images.contains(key)) m_doc.images.insert(key, img.convertToFormat(QImage::Format_ARGB32_Premultiplied));

    Shape s;
    s.id = m_doc.newId();
    s.type = ShapeType::Image;
    s.imageKey = key;
    s.name = QFileInfo(path).fileName();
    s.fill = Qt::transparent;
    s.strokeWidth = 0;
    s.lockAspect = true;
    const double w = img.width(), h = img.height();
    const double k = qMin(1.0, qMin(m_doc.width * 0.8 / w, m_doc.height * 0.8 / h));
    s.w = w * k;
    s.h = h * k;
    s.pos = qIsNaN(at.x()) ? QPointF(m_doc.width / 2.0, m_doc.height / 2.0) : at;
    curFrame().shapes.append(s);
    m_canvas->sync();
    m_canvas->setSelectedIds({s.id});
    m_canvas->setTool(Canvas::ToolSelect);
    commit(tr("Insert image"));
    log(tr("Inserted image %1 (%2×%3)").arg(s.name).arg(img.width()).arg(img.height()));
}

void MainWindow::exportFrame()
{
    QString selFilter;
    QString path = QFileDialog::getSaveFileName(this, tr("Export frame"),
                                                baseName() + QStringLiteral("_frame%1.png").arg(m_cur + 1),
                                                tr("PNG image (*.png);;JPEG image (*.jpg);;SVG vector (*.svg)"), &selFilter);
    if (path.isEmpty()) return;
    QString suf = QFileInfo(path).suffix().toLower();
    if (suf.isEmpty()) {
        suf = selFilter.contains("svg") ? "svg" : selFilter.contains("jpg") ? "jpg" : "png";
        path += "." + suf;
    }
    const Frame& f = m_doc.frames.at(m_cur);
    bool ok = true;
    if (suf == QLatin1String("svg")) {
        QSvgGenerator gen;
        gen.setFileName(path);
        gen.setSize(QSize(m_doc.width, m_doc.height));
        gen.setViewBox(QRect(0, 0, m_doc.width, m_doc.height));
        gen.setTitle(QFileInfo(path).completeBaseName());
        QPainter p(&gen);
        renderFrame(&p, m_doc, f, true);
        ok = p.end();
    } else {
        bool accepted = false;
        const double sc = QInputDialog::getDouble(this, tr("Export frame"), tr("Scale factor:"), 1.0, 0.05, 16, 2, &accepted);
        if (!accepted) return;
        const bool jpg = suf == QLatin1String("jpg") || suf == QLatin1String("jpeg");
        ok = renderFrameImage(m_doc, f, sc, true, jpg ? QColor(Qt::white) : QColor(Qt::transparent)).save(path);
    }
    if (ok) log(tr("Exported %1").arg(path));
    else QMessageBox::warning(this, tr("Export failed"), tr("Could not write %1").arg(path));
}

void MainWindow::exportAnimation()
{
    if (m_playing) togglePlay();
    m_canvas->finishSpring(true);
    const int n = m_doc.frames.size();
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Export animation"));
    auto* fl = new QFormLayout(&dlg);
    auto* fmt = new QComboBox;
    fmt->addItems({tr("MP4 video (H.264)"), tr("Animated GIF"), tr("WebM video (VP9)"), tr("PNG sequence")});
    const bool haveFfmpeg = !QStandardPaths::findExecutable("ffmpeg").isEmpty();
    if (!haveFfmpeg) fmt->setCurrentIndex(1);
    auto* scale = dspin(0.05, 8, 0.25, 2);
    scale->setKeyboardTracking(true);
    scale->setValue(1.0);
    auto* sizeLbl = new QLabel;
    auto updSize = [&] {
        sizeLbl->setText(QStringLiteral("%1 × %2 px").arg(qRound(m_doc.width * scale->value())).arg(qRound(m_doc.height * scale->value())));
    };
    updSize();
    connect(scale, &QDoubleSpinBox::valueChanged, &dlg, updSize);
    auto* range = new QComboBox;
    range->addItem(tr("Whole movie: all %1 chapters merged (%2 frames)").arg(m_doc.chapters.size()).arg(m_doc.totalFrames()));
    range->addItem(tr("Current chapter \"%1\" (%2 frames)").arg(m_doc.chapters.at(m_doc.activeChapter).name).arg(n));
    range->addItem(tr("Frame range of the current chapter"));
    range->setCurrentIndex(m_doc.chapters.size() > 1 ? 0 : 1);
    auto* from = new QSpinBox;
    from->setRange(1, n);
    from->setValue(1);
    auto* to = new QSpinBox;
    to->setRange(1, n);
    to->setValue(n);
    auto* fps = new QSpinBox;
    fps->setRange(1, 120);
    fps->setValue(m_doc.fps);
    auto* loop = new QCheckBox(tr("Loop forever (GIF)"));
    loop->setChecked(m_loop->isChecked());
    auto* note = new QLabel(haveFfmpeg ? tr("MP4/WebM use a constant frame rate; each frame is repeated to match its duration.\n"
                                            "GIF keeps every frame's own duration (10 ms resolution).")
                                       : tr("ffmpeg was not found in PATH: MP4/WebM need it (sudo apt install ffmpeg)."));
    note->setWordWrap(true);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    fl->addRow(tr("Format"), fmt);
    fl->addRow(tr("Scale"), scale);
    fl->addRow(QString(), sizeLbl);
    fl->addRow(tr("Range"), range);
    fl->addRow(tr("First frame"), from);
    fl->addRow(tr("Last frame"), to);
    fl->addRow(tr("Video FPS"), fps);
    fl->addRow(QString(), loop);
    fl->addRow(note);
    fl->addRow(bb);
    auto updRange = [=] {
        from->setEnabled(range->currentIndex() == 2);
        to->setEnabled(range->currentIndex() == 2);
    };
    updRange();
    connect(range, &QComboBox::currentIndexChanged, &dlg, updRange);
    if (dlg.exec() != QDialog::Accepted) return;

    QVector<Frame> seq;  // the frames to export, in order (cheap shared copies)
    if (range->currentIndex() == 0) {
        for (int c = 0; c < m_doc.chapters.size(); ++c) seq += m_doc.chapterFrames(c);
    } else if (range->currentIndex() == 1) {
        seq = m_doc.frames;
    } else {
        const int a0 = qMin(from->value(), to->value()) - 1, a1 = qMax(from->value(), to->value()) - 1;
        seq = m_doc.frames.mid(a0, a1 - a0 + 1);
    }

    enum { MP4, GIF, WEBM, PNGS };
    const int kind = fmt->currentIndex();
    const int f0 = 0, f1 = seq.size() - 1;
    const double sc = scale->value();
    m_doc.fps = fps->value();

    QString path;
    if (kind == PNGS) {
        path = QFileDialog::getExistingDirectory(this, tr("Folder for PNG frames"), QFileInfo(baseName()).absolutePath());
    } else {
        static const char* ext[] = {"mp4", "gif", "webm"};
        path = QFileDialog::getSaveFileName(this, tr("Export animation"), baseName() + "." + ext[kind],
                                            QStringLiteral("*.%1").arg(ext[kind]));
        if (!path.isEmpty() && QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".") + ext[kind];
    }
    if (path.isEmpty()) return;

    QString ffmpeg;
    if (kind == MP4 || kind == WEBM) {
        ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (ffmpeg.isEmpty()) {
            QMessageBox::warning(this, tr("ffmpeg not found"), tr("Video export needs ffmpeg in PATH. GIF and PNG export work without it."));
            return;
        }
    }

    QProgressDialog prog(tr("Exporting…"), tr("Cancel"), 0, f1 - f0 + 1, this);
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(200);
    QString err;
    auto frameImg = [&](int i, const QColor& base) { return renderFrameImage(m_doc, seq.at(i), sc, true, base); };

    if (kind == GIF) {
        const QImage first = frameImg(f0, Qt::white);
        GifWriter gw;
        if (!gw.open(path, first.width(), first.height(), loop->isChecked() ? 0 : -1)) {
            err = gw.errorString();
        } else {
            for (int i = f0; i <= f1 && !prog.wasCanceled(); ++i) {
                const QImage img = i == f0 ? first : frameImg(i, Qt::white);
                if (!gw.addFrame(img, qMax(2, qRound(seq.at(i).durationMs / 10.0)))) {
                    err = gw.errorString();
                    break;
                }
                prog.setValue(i - f0 + 1);
            }
            gw.close();
        }
    } else if (kind == PNGS) {
        const QDir dir(path);
        for (int i = f0; i <= f1 && !prog.wasCanceled(); ++i) {
            const QString fn = dir.filePath(QStringLiteral("frame_%1.png").arg(i - f0 + 1, 4, 10, QChar('0')));
            if (!frameImg(i, Qt::transparent).save(fn)) {
                err = tr("Could not write %1").arg(fn);
                break;
            }
            prog.setValue(i - f0 + 1);
        }
    } else {
        const QImage first = frameImg(f0, Qt::white);
        const int W = first.width(), H = first.height();
        QStringList args{"-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgra",
                         "-s", QStringLiteral("%1x%2").arg(W).arg(H), "-r", QString::number(m_doc.fps), "-i", "-",
                         "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2"};
        if (kind == MP4) args << "-c:v" << "libx264" << "-preset" << "medium" << "-crf" << "18" << "-pix_fmt" << "yuv420p" << "-movflags" << "+faststart";
        else args << "-c:v" << "libvpx-vp9" << "-b:v" << "0" << "-crf" << "32" << "-pix_fmt" << "yuv420p";
        args << path;
        QProcess proc;
        proc.start(ffmpeg, args);
        if (!proc.waitForStarted()) {
            err = proc.errorString();
        } else {
            for (int i = f0; i <= f1 && !prog.wasCanceled(); ++i) {
                const QImage img = (i == f0 ? first : frameImg(i, Qt::white)).convertToFormat(QImage::Format_ARGB32);
                const int reps = qMax(1, qRound(seq.at(i).durationMs * m_doc.fps / 1000.0));
                for (int r = 0; r < reps; ++r) {
                    proc.write(reinterpret_cast<const char*>(img.constBits()), img.sizeInBytes());
                    while (proc.bytesToWrite() > 64 * 1024 * 1024) proc.waitForBytesWritten(1000);
                }
                prog.setValue(i - f0 + 1);
                if (proc.state() != QProcess::Running) break;
            }
            proc.closeWriteChannel();
            proc.waitForFinished(-1);
            if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
                err = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
            if (err.isEmpty() && proc.exitCode() != 0) err = tr("ffmpeg failed (exit code %1)").arg(proc.exitCode());
        }
    }
    prog.setValue(prog.maximum());
    if (!err.isEmpty()) QMessageBox::warning(this, tr("Export failed"), err);
    else if (prog.wasCanceled()) log(tr("Export cancelled"));
    else log(tr("Exported %1").arg(path));
}

void MainWindow::docSettings()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Canvas settings"));
    auto* fl = new QFormLayout(&dlg);
    auto* w = new QSpinBox;
    w->setRange(1, 20000);
    w->setValue(m_doc.width);
    auto* h = new QSpinBox;
    h->setRange(1, 20000);
    h->setValue(m_doc.height);
    auto* bg = new ColorButton(tr("Background"));
    bg->setColor(m_doc.background);
    auto* fps = new QSpinBox;
    fps->setRange(1, 120);
    fps->setValue(m_doc.fps);
    auto* grid = dspin(2, 1000, 1, 0, tr(" px"));
    grid->setValue(m_canvas->gridSize);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    fl->addRow(tr("Width"), w);
    fl->addRow(tr("Height"), h);
    fl->addRow(tr("Background"), bg);
    fl->addRow(tr("Video FPS"), fps);
    fl->addRow(tr("Grid size"), grid);
    fl->addRow(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    m_canvas->gridSize = grid->value();
    m_doc.width = w->value();
    m_doc.height = h->value();
    m_doc.background = bg->color();
    m_doc.fps = fps->value();
    m_thumbs.clear();
    m_canvas->resetScene();
    commit(tr("Canvas settings"));
}

// ================================================================ edit

void MainWindow::copySel()
{
    const QVector<int> ids = m_canvas->selectedIds();
    if (ids.isEmpty()) return;
    const QSet<int> sel(ids.cbegin(), ids.cend());
    const Frame& f = m_doc.frames.at(m_cur);
    m_clipShapes.clear();
    m_clipJoints.clear();
    for (const Shape& s : f.shapes)
        if (sel.contains(s.id)) m_clipShapes << s;
    for (const Joint& j : f.joints)
        if (sel.contains(j.parent) && sel.contains(j.child)) m_clipJoints << j;
    log(tr("Copied %1 shape(s), %2 connection(s)").arg(m_clipShapes.size()).arg(m_clipJoints.size()));
}

void MainWindow::cutSel()
{
    copySel();
    deleteSel();
}

void MainWindow::paste()
{
    if (m_clipShapes.isEmpty()) return;
    pasteShapes(m_clipShapes, m_clipJoints, false);
}

void MainWindow::duplicateSel()
{
    const QVector<int> ids = m_canvas->selectedIds();
    if (ids.isEmpty()) return;
    const QSet<int> sel(ids.cbegin(), ids.cend());
    const Frame& f = m_doc.frames.at(m_cur);
    QVector<Shape> shapes;
    QVector<Joint> joints;
    for (const Shape& s : f.shapes)
        if (sel.contains(s.id)) shapes << s;
    for (const Joint& j : f.joints)
        if (sel.contains(j.parent) && sel.contains(j.child)) joints << j;
    pasteShapes(shapes, joints, true);
}

// Pasting into a frame that does not contain a shape keeps its id, so the
// copy is "the same object" for tweening purposes. Otherwise new ids + offset.
// Joints, glued line ends and groups inside the pasted set are kept.
void MainWindow::pasteShapes(const QVector<Shape>& shapes, const QVector<Joint>& joints, bool forceNew, bool nudge)
{
    Frame& f = curFrame();
    QHash<int, int> remap, groupMap;
    bool anyNew = false;
    for (const Shape& s : shapes) {
        const bool clash = forceNew || f.indexOf(s.id) >= 0;
        remap.insert(s.id, clash ? m_doc.newId() : s.id);
        anyNew |= clash;
    }
    QVector<int> newIds;
    for (const Shape& s : shapes) {
        Shape c = s;
        c.id = remap.value(s.id);
        if (anyNew && nudge) c.pos += QPointF(20, 20);
        for (EndLink& l : c.ends)
            if (l.target) l.target = remap.value(l.target, 0);  // unglue ends whose target was not copied
        if (c.group && anyNew) {
            if (!groupMap.contains(c.group)) groupMap.insert(c.group, m_doc.newId());
            c.group = groupMap.value(c.group);
        }
        f.shapes.append(c);
        newIds << c.id;
    }
    for (const Joint& j : joints) {
        Joint c = j;
        if (forceNew || f.jointIndex(j.id) >= 0) c.id = m_doc.newId();
        c.parent = remap.value(j.parent);
        c.child = remap.value(j.child);
        f.joints.append(c);
    }
    m_canvas->sync();
    m_canvas->setSelectedIds(newIds);
    commit(forceNew ? tr("Duplicate") : tr("Paste"));
    log(tr("%1 %2 shape(s), %3 connection(s)").arg(forceNew ? tr("Duplicated") : tr("Pasted")).arg(shapes.size()).arg(joints.size()));
}

void MainWindow::deleteSel()
{
    Frame& f = curFrame();
    const QVector<int> ids = m_canvas->selectedIds();
    if (ids.isEmpty()) {
        const int ji = f.jointIndex(m_canvas->selectedJoint());
        if (ji < 0) return;
        f.joints.removeAt(ji);
        m_canvas->setSelectedJoint(-1);
        m_canvas->sync();
        commit(tr("Delete connection"));
        return;
    }
    const QSet<int> sel(ids.cbegin(), ids.cend());
    Solver::removeJointsOf(f, sel);
    f.shapes.removeIf([&](const Shape& s) { return sel.contains(s.id); });
    m_canvas->sync();
    commit(tr("Delete"));
}

void MainWindow::reorder(int mode)
{
    const QVector<int> ids = m_canvas->selectedIds();
    if (ids.isEmpty()) return;
    const QSet<int> sel(ids.cbegin(), ids.cend());
    QVector<Shape>& v = curFrame().shapes;
    if (mode == 2 || mode == 3) {
        QVector<Shape> rest, chosen;
        for (const Shape& s : std::as_const(v)) (sel.contains(s.id) ? chosen : rest).append(s);
        v = mode == 2 ? rest + chosen : chosen + rest;
    } else if (mode == 0) {
        for (int i = v.size() - 2; i >= 0; --i)
            if (sel.contains(v.at(i).id) && !sel.contains(v.at(i + 1).id)) v.swapItemsAt(i, i + 1);
    } else {
        for (int i = 1; i < v.size(); ++i)
            if (sel.contains(v.at(i).id) && !sel.contains(v.at(i - 1).id)) v.swapItemsAt(i, i - 1);
    }
    m_canvas->sync();
    commit(tr("Change order"));
}

void MainWindow::disconnectSel()
{
    const QVector<int> ids = m_canvas->selectedIds();
    if (ids.isEmpty()) return;
    Frame& f = curFrame();
    const int before = f.joints.size();
    const QSet<int> sel(ids.cbegin(), ids.cend());
    Solver::removeJointsOf(f, sel);
    for (int id : ids)
        if (Shape* s = f.find(id)) s->ends[0] = s->ends[1] = EndLink();
    m_canvas->sync();
    commit(tr("Remove connections"));
    log(tr("Removed %1 connection(s)").arg(before - f.joints.size()));
}

void MainWindow::groupSel()
{
    const QVector<int> ids = m_canvas->selectedIds();
    if (ids.size() < 2) {
        log(tr("Select at least two shapes to group."));
        return;
    }
    const int g = m_doc.newId();
    Frame& f = curFrame();
    for (int id : ids)
        if (Shape* s = f.find(id)) s->group = g;
    m_canvas->sync();
    commit(tr("Group"));
    log(tr("Grouped %1 shapes (group #%2). Alt+click selects a single member.").arg(ids.size()).arg(g));
    refreshProps();
}

void MainWindow::ungroupSel()
{
    const QVector<int> ids = m_canvas->selectedIds();
    Frame& f = curFrame();
    int n = 0;
    for (int id : ids) {
        Shape* s = f.find(id);
        if (s && s->group) {
            s->group = 0;
            ++n;
        }
    }
    if (!n) return;
    m_canvas->sync();
    commit(tr("Ungroup"));
    log(tr("Ungrouped %1 shape(s)").arg(n));
    refreshProps();
}

void MainWindow::selectConnected()
{
    const QVector<int> ids = m_canvas->selectedIds();
    const Frame& f = m_doc.frames.at(m_cur);
    QSet<int> start(ids.cbegin(), ids.cend());
    if (start.isEmpty()) {
        const int ji = f.jointIndex(m_canvas->selectedJoint());
        if (ji < 0) return;
        start << f.joints.at(ji).parent;
    }
    QSet<int> all = Solver::assembly(f, start);
    QSet<int> groups;
    for (const Shape& s : f.shapes)
        if (s.group && all.contains(s.id)) groups << s.group;
    for (const Shape& s : f.shapes)  // include whole groups too
        if (groups.contains(s.group)) all.insert(s.id);
    m_canvas->setSelectedIds(QVector<int>(all.cbegin(), all.cend()));
    log(tr("Selected %1 connected shape(s). Move them, copy (Ctrl+C) or group (Ctrl+G).").arg(all.size()));
}

void MainWindow::editText(int id)
{
    const Shape* s = m_doc.frames.at(m_cur).find(id);
    if (!s || s->type != ShapeType::Text) return;
    bool ok = false;
    const QString t = QInputDialog::getMultiLineText(this, tr("Edit text"), tr("Text:"), s->text, &ok);
    if (!ok || t == s->text) return;
    Frame& f = curFrame();
    Shape* m = f.find(id);
    m->text = t;
    fitTextShape(*m);
    Solver::enforce(f, id);
    m_canvas->sync();
    commit(tr("Edit text"));
    refreshProps();
}

void MainWindow::editSingle(const std::function<void(Frame&, int, Shape&)>& fn, const QString& text, int field)
{
    if (m_updatingProps) return;
    const QVector<int> ids = m_canvas->selectedIds();
    if (ids.size() != 1) return;
    Frame& f = curFrame();
    Shape* s = f.find(ids.first());
    if (!s) return;
    fn(f, ids.first(), *s);
    m_canvas->sync();
    commit(text, field < 0 ? -1 : mid(field));
    refreshProps();
}

void MainWindow::editAll(const std::function<void(Shape&)>& fn, const QString& text, int field)
{
    if (m_updatingProps) return;
    const QVector<int> ids = m_canvas->selectedIds();
    if (ids.isEmpty()) return;
    Frame& f = curFrame();
    for (int id : ids)
        if (Shape* s = f.find(id)) fn(*s);
    for (int id : ids) Solver::enforce(f, id);
    m_canvas->sync();
    commit(text, field < 0 ? -1 : mid(field));
    refreshProps();
}

void MainWindow::editJoint(const std::function<void(Frame&, Joint&)>& fn, const QString& text, int field)
{
    if (m_updatingProps) return;
    Frame& f = curFrame();
    const int ji = f.jointIndex(m_canvas->selectedJoint());
    if (ji < 0) return;
    fn(f, f.joints[ji]);
    // re-apply limits right away (rotating by 0 clamps into range)
    for (const Joint& j : std::as_const(f.joints))
        if (j.type == JointType::Hinge && j.limited) Solver::rotateShape(f, j.child, 0.0);
    m_canvas->sync();
    commit(text, field < 0 ? -1 : mid(field));
    refreshProps();
}

void MainWindow::setStyleFill(const QColor& c)
{
    m_canvas->style.fill = c;
    m_styleFill->setColor(c);
    m_palette->addRecent(c);
    editAll([c](Shape& s) { s.fill = c; }, tr("Fill colour"), 10);
}

void MainWindow::setStyleStroke(const QColor& c)
{
    m_canvas->style.stroke = c;
    m_styleStroke->setColor(c);
    m_palette->addRecent(c);
    editAll([c](Shape& s) { s.stroke = c; }, tr("Stroke colour"), 19);
}

// ================================================================ frames

void MainWindow::frameNewCopy()
{
    m_canvas->finishSpring(true);
    const Frame copy = m_doc.frames.at(m_cur);  // shares data until modified
    m_doc.frames.insert(m_cur + 1, copy);
    m_cur += 1;
    m_canvas->setFrameIndex(m_cur);
    commit(tr("New frame"));
    setCurrentFrame(m_cur);
    log(tr("Frame %1 created as a copy of frame %2. Modify it, then use Tween to fill in-betweens.").arg(m_cur + 1).arg(m_cur));
}

void MainWindow::frameNewBlank()
{
    Frame f;
    f.durationMs = m_doc.frames.at(m_cur).durationMs;
    m_doc.frames.insert(m_cur + 1, f);
    m_cur += 1;
    m_canvas->setFrameIndex(m_cur);
    commit(tr("Blank frame"));
    setCurrentFrame(m_cur);
}

void MainWindow::frameDuplicateN()
{
    bool ok = false;
    const int n = QInputDialog::getInt(this, tr("Duplicate frame"), tr("Number of copies to insert after frame %1:").arg(m_cur + 1),
                                       1, 1, 1000, 1, &ok);
    if (!ok) return;
    const Frame copy = m_doc.frames.at(m_cur);
    m_doc.frames.insert(m_cur + 1, n, copy);
    refreshTimeline();
    commit(tr("Duplicate frame"));
}

void MainWindow::frameDelete()
{
    QVector<int> rows = selectedFrames();
    if (rows.size() >= m_doc.frames.size()) {  // keep at least one frame
        m_doc.frames.clear();
        m_doc.frames.append(Frame());
    } else {
        for (int i = rows.size() - 1; i >= 0; --i) m_doc.frames.removeAt(rows[i]);
    }
    m_cur = qBound(0, rows.first(), m_doc.frames.size() - 1);
    m_canvas->setFrameIndex(m_cur);
    m_timeline->clearSelection();
    commit(rows.size() > 1 ? tr("Delete frames") : tr("Delete frame"));
    setCurrentFrame(m_cur);
}

void MainWindow::frameMove(int dir)
{
    const int j = m_cur + dir;
    if (j < 0 || j >= m_doc.frames.size()) return;
    m_doc.frames.swapItemsAt(m_cur, j);
    m_cur = j;
    m_canvas->setFrameIndex(m_cur);
    commit(tr("Move frame"));
    setCurrentFrame(m_cur);
}

void MainWindow::frameTween()
{
    if (m_cur + 1 >= m_doc.frames.size()) {
        log(tr("Tween needs a next frame: make one with \"New Frame (copy)\", change it, then select the first frame and run Tween."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Tween frames %1 → %2").arg(m_cur + 1).arg(m_cur + 2));
    auto* fl = new QFormLayout(&dlg);
    auto* count = new QSpinBox;
    count->setRange(1, 500);
    count->setValue(5);
    auto* easing = new QComboBox;
    easing->addItems({tr("Linear"), tr("Ease in-out"), tr("Ease in"), tr("Ease out")});
    easing->setCurrentIndex(1);
    auto* dur = new QSpinBox;
    dur->setRange(10, 60000);
    dur->setSingleStep(10);
    dur->setSuffix(tr(" ms"));
    dur->setValue(m_doc.frames.at(m_cur).durationMs);
    auto* squash = new QSpinBox;
    squash->setRange(0, 100);
    squash->setSuffix(QStringLiteral(" %"));
    squash->setToolTip(tr("Stretch unconnected shapes along their motion (cartoon squash & stretch)"));
    auto* note = new QLabel(tr("Shapes are matched by identity (frames made with \"New Frame (copy)\" share them). "
                               "Connected shapes are interpolated by joint angles so they stay attached; "
                               "shapes in only one frame fade in/out."));
    note->setWordWrap(true);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    fl->addRow(tr("In-between frames"), count);
    fl->addRow(tr("Easing"), easing);
    fl->addRow(tr("Duration each"), dur);
    fl->addRow(tr("Squash && stretch"), squash);
    fl->addRow(note);
    fl->addRow(bb);
    if (dlg.exec() != QDialog::Accepted) return;

    const Frame a = m_doc.frames.at(m_cur), b = m_doc.frames.at(m_cur + 1);
    const int n = count->value(), em = easing->currentIndex();
    QVector<Frame> mids;
    mids.reserve(n);
    for (int k = 1; k <= n; ++k) {
        Solver::TweenOptions opt;
        opt.squash = squash->value() / 100.0;
        opt.motionStep = Solver::ease(qMin(1.0, (k + 0.5) / (n + 1)), em) - Solver::ease(qMax(0.0, (k - 0.5) / (n + 1)), em);
        Frame t = Solver::tween(a, b, Solver::ease(double(k) / (n + 1), em), opt);
        t.durationMs = dur->value();
        mids << t;
    }
    for (int k = 0; k < n; ++k) m_doc.frames.insert(m_cur + 1 + k, mids.at(k));
    commit(tr("Tween"));
    log(tr("Inserted %1 in-between frames after frame %2").arg(n).arg(m_cur + 1));
}

void MainWindow::setFrameDurations(int ms, bool all)
{
    QVector<int> rows;
    if (all) for (int i = 0; i < m_doc.frames.size(); ++i) rows << i;
    else rows = selectedFrames();
    bool changed = false;
    for (int r : rows) {
        if (m_doc.frames.at(r).durationMs == ms) continue;
        m_doc.frames[r].durationMs = ms;
        changed = true;
    }
    if (changed) commit(tr("Frame duration"), all ? -1 : mid(40));
}

void MainWindow::togglePlay()
{
    if (m_playing) {
        m_playing = false;
        m_playTimer.stop();
        m_canvas->setReadOnly(false);
        m_actPlay->setText(tr("Play"));
        m_actPlay->setIcon(themeIcon("media-playback-start", QStyle::SP_MediaPlay));
        return;
    }
    if (m_doc.frames.size() < 2) return;
    m_playing = true;
    m_canvas->setSelectedIds({});
    m_canvas->setReadOnly(true);
    m_actPlay->setText(tr("Stop"));
    m_actPlay->setIcon(themeIcon("media-playback-stop", QStyle::SP_MediaStop));
    m_playTimer.start(qMax(10, m_doc.frames.at(m_cur).durationMs));
}

void MainWindow::playStep()
{
    if (!m_playing) return;
    int next = m_cur + 1;
    if (next >= m_doc.frames.size()) {
        if (!m_loop->isChecked()) {
            togglePlay();
            return;
        }
        next = 0;
    }
    setCurrentFrame(next);
    m_playTimer.start(qMax(10, m_doc.frames.at(m_cur).durationMs));
}

// ================================================================ chapters

void MainWindow::refreshChapters()
{
    m_updatingTimeline = true;
    m_chapterCombo->clear();
    for (int c = 0; c < m_doc.chapters.size(); ++c)
        m_chapterCombo->addItem(QStringLiteral("%1. %2  (%3)").arg(c + 1).arg(m_doc.chapters.at(c).name).arg(m_doc.chapterFrames(c).size()));
    m_chapterCombo->setCurrentIndex(m_doc.activeChapter);
    m_updatingTimeline = false;
    const bool many = m_doc.chapters.size() > 1;
    m_actChDel->setEnabled(many);
    m_actChPrev->setEnabled(m_doc.activeChapter > 0);
    m_actChNext->setEnabled(m_doc.activeChapter + 1 < m_doc.chapters.size());
    m_actChLeft->setEnabled(m_doc.activeChapter > 0);
    m_actChRight->setEnabled(m_doc.activeChapter + 1 < m_doc.chapters.size());
}

// Switching chapters is navigation, not an edit: no undo entry.
void MainWindow::switchChapter(int c)
{
    if (c < 0 || c >= m_doc.chapters.size() || c == m_doc.activeChapter) {
        refreshChapters();
        return;
    }
    if (m_playing) togglePlay();
    m_canvas->finishSpring(true);
    m_doc.switchChapter(c);
    m_committed = m_doc;
    m_cur = 0;
    m_committedFrame = 0;
    m_canvas->setSelectedIds({});
    m_canvas->setFrameIndex(0);
    m_timeline->scrollToTop();
    refreshChapters();
    refreshTimeline();
    setCurrentFrame(0);
    log(tr("Chapter %1: %2 (%3 frames)").arg(c + 1).arg(m_doc.chapters.at(c).name).arg(m_doc.frames.size()));
}

void MainWindow::chapterNew()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("New chapter"));
    auto* fl = new QFormLayout(&dlg);
    auto* name = new QLineEdit(tr("Chapter %1").arg(m_doc.chapters.size() + 1));
    auto* copy = new QCheckBox(tr("Start with a copy of the current frame (continuity)"));
    copy->setChecked(true);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    fl->addRow(tr("Name"), name);
    fl->addRow(QString(), copy);
    fl->addRow(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    Frame first = copy->isChecked() ? m_doc.frames.at(m_cur) : Frame();
    if (!copy->isChecked()) first.durationMs = m_doc.frames.at(m_cur).durationMs;
    const int at = m_doc.activeChapter + 1;
    m_doc.chapters.insert(at, Chapter{name->text().trimmed().isEmpty() ? tr("Chapter %1").arg(at + 1) : name->text().trimmed(), {first}});
    commit(tr("New chapter"));
    switchChapter(at);
}

void MainWindow::chapterDuplicate()
{
    const int at = m_doc.activeChapter + 1;
    m_doc.chapters.insert(at, Chapter{tr("%1 (copy)").arg(m_doc.chapters.at(m_doc.activeChapter).name), m_doc.frames});
    commit(tr("Duplicate chapter"));
    switchChapter(at);
}

void MainWindow::chapterRename()
{
    bool ok = false;
    const QString n = QInputDialog::getText(this, tr("Rename chapter"), tr("Name:"), QLineEdit::Normal,
                                            m_doc.chapters.at(m_doc.activeChapter).name, &ok);
    if (!ok || n.trimmed().isEmpty()) return;
    m_doc.chapters[m_doc.activeChapter].name = n.trimmed();
    commit(tr("Rename chapter"));
    refreshChapters();
}

void MainWindow::chapterDelete()
{
    if (m_doc.chapters.size() < 2) return;
    const int a = m_doc.activeChapter;
    if (QMessageBox::question(this, tr("Delete chapter"),
                              tr("Delete chapter \"%1\" with %2 frames? (Undo can bring it back.)")
                                  .arg(m_doc.chapters.at(a).name).arg(m_doc.frames.size())) != QMessageBox::Yes)
        return;
    m_doc.chapters.removeAt(a);
    const int na = qMin(a, int(m_doc.chapters.size()) - 1);
    m_doc.activeChapter = na;
    m_doc.frames = m_doc.chapters[na].frames;
    m_doc.chapters[na].frames.clear();
    m_cur = 0;
    m_canvas->setSelectedIds({});
    m_canvas->setFrameIndex(0);
    commit(tr("Delete chapter"));
    refreshChapters();
    setCurrentFrame(0);
}

void MainWindow::chapterMove(int dir)
{
    const int a = m_doc.activeChapter, b = a + dir;
    if (b < 0 || b >= m_doc.chapters.size()) return;
    m_doc.chapters.swapItemsAt(a, b);  // the active chapter's (stale) slot moves with it
    m_doc.activeChapter = b;
    commit(tr("Move chapter"));
    refreshChapters();
}

// Merge another project's chapters (e.g. parts made in separate files) into this one.
void MainWindow::chapterImport()
{
    const QString p = QFileDialog::getOpenFileName(this, tr("Import chapters from project"), QFileInfo(baseName()).absolutePath(),
                                                   tr("TooD projects (*.tood)"));
    if (p.isEmpty()) return;
    Document d;
    QString err;
    if (!Document::load(p, d, &err)) {
        QMessageBox::warning(this, tr("Import failed"), err);
        return;
    }
    if (d.width != m_doc.width || d.height != m_doc.height)
        log(tr("Note: imported project is %1×%2, this one is %3×%4; shapes keep their coordinates.")
                .arg(d.width).arg(d.height).arg(m_doc.width).arg(m_doc.height));
    // shift all ids so nothing collides with this project's ids
    const int off = m_doc.nextId;
    for (int c = 0; c < d.chapters.size(); ++c) {
        QVector<Frame> frames = d.chapterFrames(c);
        for (Frame& f : frames) {
            for (Shape& s : f.shapes) {
                s.id += off;
                if (s.group) s.group += off;
                for (EndLink& l : s.ends)
                    if (l.target) l.target += off;
            }
            for (Joint& j : f.joints) {
                j.id += off;
                j.parent += off;
                j.child += off;
            }
        }
        m_doc.chapters.append(Chapter{d.chapters.at(c).name, frames});
    }
    m_doc.nextId = off + d.nextId;
    for (auto it = d.images.cbegin(); it != d.images.cend(); ++it)
        if (!m_doc.images.contains(it.key())) m_doc.images.insert(it.key(), it.value());
    commit(tr("Import chapters"));
    refreshChapters();
    refreshTimeline();
    log(tr("Imported %1 chapter(s) from %2").arg(d.chapters.size()).arg(QFileInfo(p).fileName()));
}

// Save the current chapter as its own project (for splitting big projects or sharing a part).
void MainWindow::chapterExport()
{
    const QString name = m_doc.chapters.at(m_doc.activeChapter).name;
    QString p = QFileDialog::getSaveFileName(this, tr("Export chapter as project"),
                                             QFileInfo(baseName()).absolutePath() + "/" + name + ".tood", tr("TooD projects (*.tood)"));
    if (p.isEmpty()) return;
    if (QFileInfo(p).suffix().isEmpty()) p += ".tood";
    Document d;
    d.width = m_doc.width;
    d.height = m_doc.height;
    d.background = m_doc.background;
    d.fps = m_doc.fps;
    d.frames = m_doc.frames;
    d.chapters = {Chapter{name, {}}};
    d.images = m_doc.images;
    d.nextId = m_doc.nextId;
    QString err;
    if (!d.save(p, &err)) QMessageBox::warning(this, tr("Export failed"), err);
    else log(tr("Chapter \"%1\" saved as %2").arg(name, p));
}

// ================================================================ library

void MainWindow::refreshLibrary()
{
    m_library->clear();
    const QDir dir(libraryDir());
    const QSize ts = m_library->iconSize();
    for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.tood")}, QDir::Files, QDir::Name | QDir::IgnoreCase)) {
        Document d;
        if (!Document::load(fi.filePath(), d, nullptr)) continue;
        const Frame& f = d.frames.at(0);
        const double sc = qMin(1.0, qMin(ts.width() / double(d.width), ts.height() / double(d.height)));
        QPixmap pm(ts);
        pm.fill(QColor(245, 245, 245));
        {
            QPainter p(&pm);
            const QImage img = renderFrameImage(d, f, sc, true);
            p.drawImage((ts.width() - img.width()) / 2, (ts.height() - img.height()) / 2, img);
        }
        auto* it = new QListWidgetItem(QIcon(pm), fi.completeBaseName());
        it->setData(Qt::UserRole, fi.filePath());
        it->setToolTip(tr("%1\n%2 shape(s), %3 connection(s)\nDouble-click to insert, or drag onto the canvas.")
                           .arg(fi.completeBaseName()).arg(f.shapes.size()).arg(f.joints.size()));
        m_library->addItem(it);
    }
}

void MainWindow::libraryAdd()
{
    const QVector<int> ids = m_canvas->selectedIds();
    if (ids.isEmpty()) {
        log(tr("Select the shapes to save first (Ctrl+L selects everything connected)."));
        return;
    }
    const Frame& f = m_doc.frames.at(m_cur);
    const QSet<int> sel(ids.cbegin(), ids.cend());
    QSet<int> withConnected = Solver::assembly(f, sel);
    QSet<int> groups;
    for (const Shape& s : f.shapes)
        if (s.group && withConnected.contains(s.id)) groups << s.group;
    for (const Shape& s : f.shapes)
        if (groups.contains(s.group)) withConnected.insert(s.id);

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Add to library"));
    auto* fl = new QFormLayout(&dlg);
    const Shape* first = f.find(ids.first());
    auto* name = new QLineEdit(first && !first->name.isEmpty() ? first->name : tr("Item %1").arg(m_library->count() + 1));
    auto* conn = new QCheckBox(tr("Include everything connected or grouped (%1 shapes instead of %2)")
                                   .arg(withConnected.size()).arg(sel.size()));
    conn->setChecked(withConnected.size() > sel.size());
    conn->setEnabled(withConnected.size() > sel.size());
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    fl->addRow(tr("Name"), name);
    fl->addRow(QString(), conn);
    fl->addRow(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    QString n = name->text().trimmed();
    n.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("_"));
    if (n.isEmpty()) return;
    const QString path = QDir(libraryDir()).filePath(n + ".tood");
    if (QFileInfo::exists(path) &&
        QMessageBox::question(this, tr("Replace?"), tr("A library item \"%1\" exists. Replace it?").arg(n)) != QMessageBox::Yes)
        return;

    const QSet<int> set = conn->isChecked() ? withConnected : sel;
    Frame item;
    QRectF box;
    for (const Shape& s : f.shapes) {
        if (!set.contains(s.id)) continue;
        Shape c = s;
        for (EndLink& l : c.ends)
            if (!set.contains(l.target)) l = EndLink();
        item.shapes << c;
        const QRectF r = s.type == ShapeType::Point ? QRectF(s.pos - QPointF(5, 5), QSizeF(10, 10))
                                                    : s.transform().mapRect(s.localRect().adjusted(-s.strokeWidth, -s.strokeWidth, s.strokeWidth, s.strokeWidth));
        box = box.isNull() ? r : box.united(r);
    }
    for (const Joint& j : f.joints)
        if (set.contains(j.parent) && set.contains(j.child)) item.joints << j;
    const double pad = 4;
    box.adjust(-pad, -pad, pad, pad);
    for (Shape& s : item.shapes) s.pos -= box.topLeft();  // item coordinates start at (0,0)

    Document d;
    d.width = qMax(1, qCeil(box.width()));
    d.height = qMax(1, qCeil(box.height()));
    d.background = Qt::transparent;
    d.frames = {item};
    for (const Shape& s : item.shapes)
        if (s.type == ShapeType::Image && m_doc.images.contains(s.imageKey)) d.images.insert(s.imageKey, m_doc.images.value(s.imageKey));
    d.nextId = m_doc.nextId;
    QString err;
    if (!d.save(path, &err)) {
        QMessageBox::warning(this, tr("Library"), err);
        return;
    }
    refreshLibrary();
    log(tr("Saved \"%1\" to the library (%2 shapes, %3 connections).").arg(n).arg(item.shapes.size()).arg(item.joints.size()));
}

void MainWindow::libraryInsert(const QString& path, QPointF at)
{
    if (path.isEmpty()) return;
    Document d;
    QString err;
    if (!Document::load(path, d, &err)) {
        log(tr("Could not load library item: %1").arg(err));
        return;
    }
    for (auto it = d.images.cbegin(); it != d.images.cend(); ++it)
        if (!m_doc.images.contains(it.key())) m_doc.images.insert(it.key(), it.value());
    const QPointF centre = qIsNaN(at.x()) ? m_canvas->mapToScene(m_canvas->viewport()->rect().center()) : at;
    const QPointF off = centre - QPointF(d.width / 2.0, d.height / 2.0);
    QVector<Shape> shapes = d.frames.at(0).shapes;
    for (Shape& s : shapes) s.pos += off;
    m_canvas->setTool(Canvas::ToolSelect);
    pasteShapes(shapes, d.frames.at(0).joints, true, false);
    log(tr("Inserted \"%1\" from the library.").arg(QFileInfo(path).completeBaseName()));
}

void MainWindow::libraryRename()
{
    QListWidgetItem* it = m_library->currentItem();
    if (!it) return;
    const QString path = it->data(Qt::UserRole).toString();
    bool ok = false;
    QString n = QInputDialog::getText(this, tr("Rename library item"), tr("Name:"), QLineEdit::Normal, it->text(), &ok).trimmed();
    n.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("_"));
    if (!ok || n.isEmpty() || n == it->text()) return;
    if (!QFile::rename(path, QDir(libraryDir()).filePath(n + ".tood"))) log(tr("Could not rename (name taken?)"));
    refreshLibrary();
}

void MainWindow::libraryDelete()
{
    QListWidgetItem* it = m_library->currentItem();
    if (!it) return;
    if (QMessageBox::question(this, tr("Delete library item"), tr("Delete \"%1\" from the library?").arg(it->text())) != QMessageBox::Yes)
        return;
    QFile::remove(it->data(Qt::UserRole).toString());
    refreshLibrary();
}
