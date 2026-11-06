// Connection constraints and in-betweening.
//
// Joints form a forest (each shape has at most one parent). Moving a shape
// propagates outward breadth-first through the joint graph; every neighbour is
// re-placed so the shared anchor points coincide again:
//   Weld            neighbour gets the same rotation delta (rigid assembly)
//   Hinge, down     child keeps its angle relative to the parent (carried along)
//   Hinge, up       parent only translates; when you drag a hinge child directly
//                   it rotates around the pin instead of moving (see dragShape)
//   Free            neighbour swings so its centre trails behind the pin
//   Elastic         neighbour only translates; in "lag" mode (interactive drags)
//                   it follows by a fraction (stiffness), springStep() pulls it back
//
// Stretch connectors (Line/Arrow with EndLinks) are not part of the joint graph.
// After every propagation their ends are re-glued to their targets, so they
// stretch and turn; they never push the shapes they are glued to.
#pragma once

#include "model.h"

#include <QSet>

namespace Solver {

struct DragInfo {
    int joint = -1;     // index of the joint that anchors the dragged assembly, -1 = none
    int root = -1;      // shape whose parent joint that is
    QPointF pivot;      // world position of that joint
    bool hinge = false; // true -> assembly can only rotate around pivot
};

QPointF anchorWorld(const Shape& s, QPointF norm);
QPointF normalizeLocal(const Shape& s, QPointF local);
double normDeg(double a);            // -> (-180, 180]
double angleDeg(QPointF v);

void propagate(Frame& f, const QHash<int, double>& seedRotDelta, bool elasticLag = false);
void enforce(Frame& f, int shapeId);  // re-attach after a resize / property edit
DragInfo dragInfo(const Frame& f, int shapeId);
void rotateShape(Frame& f, int shapeId, double deltaDeg, bool elasticLag = false);
void dragShape(Frame& f, int shapeId, QPointF grabLocal, QPointF mouseWorld, bool elasticLag = false);
void translateShapes(Frame& f, const QVector<int>& ids, QPointF delta);

// Elastic springback: one integration step, pinned shapes stay put.
// Returns true while anything is still moving.
bool hasElasticGap(const Frame& f, double tol = 0.5);
bool springStep(Frame& f, const QVector<int>& pinned, QHash<int, QPointF>& velocity, double dt);

// Re-glue connector ends. Returns the ids of connectors that changed.
QVector<int> updateStretch(Frame& f);

QSet<int> component(const Frame& f, int shapeId);
// Everything reachable through joints and glued connector ends.
QSet<int> assembly(const Frame& f, const QSet<int>& ids);
void reroot(Frame& f, int shapeId);  // reverse joints so shapeId has no parent
bool connectShapes(Frame& f, int parent, QPointF parentPt, int child, QPointF childPt,
                   JointType type, int jointId, QString* why);
void removeJointsOf(Frame& f, const QSet<int>& shapeIds);  // also frees connector ends

double ease(double t, int mode);  // 0 linear, 1 in-out, 2 in, 3 out

struct TweenOptions {
    double squash = 0;      // 0..1: stretch unconnected shapes along their motion
    double motionStep = 0;  // fraction of the a->b distance travelled in this in-between
};
// Interpolates a -> b. Shapes are matched by id; connected shapes are
// interpolated in joint space (relative angles) so connections stay intact.
Frame tween(const Frame& a, const Frame& b, double t, const TweenOptions& opt = {});

}  // namespace Solver
