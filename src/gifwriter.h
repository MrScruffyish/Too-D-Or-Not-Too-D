// Minimal animated GIF89a encoder: per-frame median-cut palette (256 colours),
// LZW compression, and only the changed rectangle of each frame is stored.
#pragma once

#include <QFile>
#include <QImage>
#include <cstdint>
#include <vector>

class GifWriter {
public:
    // loops: 0 = forever, -1 = play once
    bool open(const QString& path, int width, int height, int loops = 0);
    bool addFrame(const QImage& image, int delayCentiseconds);
    bool close();
    QString errorString() const { return m_file.errorString(); }

private:
    QFile m_file;
    int m_w = 0, m_h = 0;
    QImage m_prev;
    std::vector<uint16_t> m_dict;
};
