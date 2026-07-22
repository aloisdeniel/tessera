// quat.dart — minimal quaternion helpers for card / hand orientations (xyzw).
import 'dart:math' as math;

/// Identity quaternion (the engine also treats all-zero as identity).
const List<double> quatIdentity = [0, 0, 0, 1];

/// Quaternion (xyzw) for a rotation of [angle] radians about axis (x, y, z).
List<double> quatAxis(double x, double y, double z, double angle) {
  final s = math.sin(angle / 2);
  return [x * s, y * s, z * s, math.cos(angle / 2)];
}

/// Hamilton product a·b (apply b first, then a), both xyzw.
List<double> quatMul(List<double> a, List<double> b) {
  final ax = a[0], ay = a[1], az = a[2], aw = a[3];
  final bx = b[0], by = b[1], bz = b[2], bw = b[3];
  return [
    aw * bx + ax * bw + ay * bz - az * by,
    aw * by - ax * bz + ay * bw + az * bx,
    aw * bz + ax * by - ay * bx + az * bw,
    aw * bw - ax * bx - ay * by - az * bz,
  ];
}
