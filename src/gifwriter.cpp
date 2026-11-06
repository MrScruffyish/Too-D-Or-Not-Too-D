#include "gifwriter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

void put16(QByteArray& b, int v)
{
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
}

struct Pt {
    int key;
    int r, g, b;
    quint32 n;
};

// Histogram in RGB555 space, then median cut to <= 256 boxes. The box an
// entry ends up in is directly its palette index (no nearest-colour search).
void quantize(const QImage& im, const QRect& rc, QVector<QRgb>& pal, std::vector<uint8_t>& out)
{
    std::vector<quint32> cnt(32768, 0);
    std::vector<quint64> sr(32768, 0), sg(32768, 0), sb(32768, 0);
    for (int y = rc.top(); y <= rc.bottom(); ++y) {
        const QRgb* L = reinterpret_cast<const QRgb*>(im.constScanLine(y));
        for (int x = rc.left(); x <= rc.right(); ++x) {
            const QRgb c = L[x];
            const int r = qRed(c), g = qGreen(c), b = qBlue(c);
            const int k = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
            ++cnt[k];
            sr[k] += r;
            sg[k] += g;
            sb[k] += b;
        }
    }
    std::vector<Pt> pts;
    for (int k = 0; k < 32768; ++k)
        if (cnt[k])
            pts.push_back({k, int(sr[k] / cnt[k]), int(sg[k] / cnt[k]), int(sb[k] / cnt[k]), cnt[k]});

    std::vector<uint8_t> map(32768, 0);
    pal.clear();
    if (pts.size() <= 256) {
        for (size_t i = 0; i < pts.size(); ++i) {
            pal << qRgb(pts[i].r, pts[i].g, pts[i].b);
            map[pts[i].key] = uint8_t(i);
        }
    } else {
        struct Box {
            int b, e;
        };
        std::vector<Box> boxes{{0, int(pts.size())}};
        while (boxes.size() < 256) {
            int best = -1, bestCh = 0;
            double bestScore = 0;
            for (size_t i = 0; i < boxes.size(); ++i) {
                const Box bx = boxes[i];
                if (bx.e - bx.b < 2) continue;
                int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
                quint64 w = 0;
                for (int k = bx.b; k < bx.e; ++k) {
                    const Pt& p = pts[k];
                    const int v[3] = {p.r, p.g, p.b};
                    for (int c = 0; c < 3; ++c) {
                        mn[c] = std::min(mn[c], v[c]);
                        mx[c] = std::max(mx[c], v[c]);
                    }
                    w += p.n;
                }
                int ch = 0;
                for (int c = 1; c < 3; ++c)
                    if (mx[c] - mn[c] > mx[ch] - mn[ch]) ch = c;
                const double score = double(mx[ch] - mn[ch]) * std::sqrt(double(w));
                if (score > bestScore) {
                    bestScore = score;
                    best = int(i);
                    bestCh = ch;
                }
            }
            if (best < 0) break;
            const Box bx = boxes[best];
            std::sort(pts.begin() + bx.b, pts.begin() + bx.e, [bestCh](const Pt& a, const Pt& b) {
                return bestCh == 0 ? a.r < b.r : bestCh == 1 ? a.g < b.g : a.b < b.b;
            });
            quint64 tot = 0, acc = 0;
            for (int k = bx.b; k < bx.e; ++k) tot += pts[k].n;
            int m = bx.b;
            for (; m < bx.e - 1; ++m) {
                acc += pts[m].n;
                if (acc * 2 >= tot) break;
            }
            m = std::clamp(m + 1, bx.b + 1, bx.e - 1);
            boxes[best] = {bx.b, m};
            boxes.push_back({m, bx.e});
        }
        for (size_t i = 0; i < boxes.size(); ++i) {
            quint64 r = 0, g = 0, b = 0, n = 0;
            for (int k = boxes[i].b; k < boxes[i].e; ++k) {
                const Pt& p = pts[k];
                r += quint64(p.r) * p.n;
                g += quint64(p.g) * p.n;
                b += quint64(p.b) * p.n;
                n += p.n;
                map[p.key] = uint8_t(i);
            }
            pal << qRgb(int(r / n), int(g / n), int(b / n));
        }
    }

    out.resize(size_t(rc.width()) * rc.height());
    size_t o = 0;
    for (int y = rc.top(); y <= rc.bottom(); ++y) {
        const QRgb* L = reinterpret_cast<const QRgb*>(im.constScanLine(y));
        for (int x = rc.left(); x <= rc.right(); ++x) {
            const QRgb c = L[x];
            out[o++] = map[((qRed(c) >> 3) << 10) | ((qGreen(c) >> 3) << 5) | (qBlue(c) >> 3)];
        }
    }
}

// Variable-width LZW as used by GIF (code size logic follows the well-known
// public-domain gif.h encoder).
void writeLzw(QByteArray& out, const std::vector<uint8_t>& px, std::vector<uint16_t>& dict)
{
    const int minCode = 8, clear = 256;
    out.append(char(minCode));
    QByteArray data;
    data.reserve(int(px.size()));
    quint32 acc = 0;
    int nb = 0;
    auto put = [&](quint32 code, int size) {
        acc |= code << nb;
        nb += size;
        while (nb >= 8) {
            data.append(char(acc & 0xFF));
            acc >>= 8;
            nb -= 8;
        }
    };
    dict.assign(4096 * 256, 0);
    int codeSize = minCode + 1, maxCode = clear + 1;
    put(clear, codeSize);
    int cur = -1;
    for (uint8_t v : px) {
        if (cur < 0) {
            cur = v;
            continue;
        }
        uint16_t& nx = dict[size_t(cur) * 256 + v];
        if (nx) {
            cur = nx;
            continue;
        }
        put(quint32(cur), codeSize);
        nx = uint16_t(++maxCode);
        if (maxCode >= (1 << codeSize)) ++codeSize;
        if (maxCode == 4095) {
            put(clear, codeSize);
            std::fill(dict.begin(), dict.end(), 0);
            codeSize = minCode + 1;
            maxCode = clear + 1;
        }
        cur = v;
    }
    if (cur >= 0) put(quint32(cur), codeSize);
    put(clear, codeSize);
    put(clear + 1, minCode + 1);
    if (nb > 0) data.append(char(acc & 0xFF));

    for (int i = 0; i < data.size(); i += 255) {
        const int n = std::min(255, int(data.size()) - i);
        out.append(char(n));
        out.append(data.constData() + i, n);
    }
    out.append(char(0));
}

}  // namespace

bool GifWriter::open(const QString& path, int w, int h, int loops)
{
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly)) return false;
    m_w = w;
    m_h = h;
    m_prev = QImage();
    QByteArray hd("GIF89a");
    put16(hd, w);
    put16(hd, h);
    hd.append(char(0x70));  // no global colour table, 8-bit colour resolution
    hd.append(char(0));
    hd.append(char(0));
    if (loops >= 0) {
        hd.append("\x21\xFF\x0B", 3);
        hd.append("NETSCAPE2.0", 11);
        hd.append(char(3));
        hd.append(char(1));
        put16(hd, loops);
        hd.append(char(0));
    }
    return m_file.write(hd) == hd.size();
}

bool GifWriter::addFrame(const QImage& image, int delayCs)
{
    QImage cur = image.convertToFormat(QImage::Format_RGB32);
    if (cur.width() != m_w || cur.height() != m_h)
        cur = cur.scaled(m_w, m_h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QRect rc(0, 0, m_w, m_h);
    if (!m_prev.isNull()) {  // only encode the bounding box of changed pixels
        int x0 = m_w, y0 = m_h, x1 = -1, y1 = -1;
        for (int y = 0; y < m_h; ++y) {
            const QRgb* a = reinterpret_cast<const QRgb*>(cur.constScanLine(y));
            const QRgb* b = reinterpret_cast<const QRgb*>(m_prev.constScanLine(y));
            if (std::memcmp(a, b, size_t(m_w) * 4) == 0) continue;
            y0 = std::min(y0, y);
            y1 = y;
            for (int x = 0; x < m_w; ++x)
                if (a[x] != b[x]) {
                    x0 = std::min(x0, x);
                    break;
                }
            for (int x = m_w - 1; x >= 0; --x)
                if (a[x] != b[x]) {
                    x1 = std::max(x1, x);
                    break;
                }
        }
        rc = x1 < 0 ? QRect(0, 0, 1, 1) : QRect(QPoint(x0, y0), QPoint(x1, y1));
    }

    QVector<QRgb> pal;
    std::vector<uint8_t> idx;
    quantize(cur, rc, pal, idx);

    QByteArray fr;
    fr.append("\x21\xF9\x04", 3);  // graphic control extension
    fr.append(char(0x04));          // disposal: do not dispose
    put16(fr, std::clamp(delayCs, 0, 65535));
    fr.append(char(0));
    fr.append(char(0));
    fr.append(char(0x2C));          // image descriptor
    put16(fr, rc.x());
    put16(fr, rc.y());
    put16(fr, rc.width());
    put16(fr, rc.height());
    fr.append(char(0x87));          // local colour table, 256 entries
    for (int i = 0; i < 256; ++i) {
        const QRgb c = i < pal.size() ? pal[i] : 0;
        fr.append(char(qRed(c)));
        fr.append(char(qGreen(c)));
        fr.append(char(qBlue(c)));
    }
    writeLzw(fr, idx, m_dict);
    m_prev = cur;
    return m_file.write(fr) == fr.size();
}

bool GifWriter::close()
{
    const bool ok = m_file.putChar(0x3B);
    m_file.close();
    m_prev = QImage();
    return ok;
}
