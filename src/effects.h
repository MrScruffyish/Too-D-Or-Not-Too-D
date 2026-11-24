// Procedural particle effects. Rendering is a pure function of the shape's
// effect settings and effectTime, so the same frame looks identical in the
// editor, during playback and in every export format.
#pragma once

#include <QStringList>

class QPainter;
struct Shape;

QStringList effectNames();
int effectCount();
int mouthEffectPreset();
bool effectLoops(int preset);
// Paints in the shape's local coordinate system (centre at origin).
void paintEffect(QPainter* p, const Shape& s);
