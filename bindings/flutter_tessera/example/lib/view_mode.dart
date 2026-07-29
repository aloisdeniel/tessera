// view_mode.dart — the shared "View mode" free-look control every example
// game gets from its `GameScreen` host.
//
// An app-bar toggle arms it; while armed the player grabs the scene and
// orbits the camera around the centre of the board by dragging, and zooms
// with a pinch (or trackpad pinch). Every gesture update goes through the
// engine's IMPERATIVE camera (`TesseraController.setCamera`), which snaps the
// pose with no tween — zero lag between the finger and the view. The game
// itself is frozen for the duration — no auto-play, no taps. Entering eases
// onto the orbit pose, and leaving the mode (the toggle again, a board tap,
// or any other interaction) glides the camera back to wherever the game had
// put it — those two transitions ride the scene path, which tweens.

import 'package:flutter/widgets.dart' show ChangeNotifier, Offset;
import 'package:flutter_tessera/flutter_tessera.dart';

class ViewModeController extends ChangeNotifier {
  TesseraController? _tessera;
  TesseraScene? _scene; // the game's live scene, with its own camera
  TesseraCameraPose? _pose; // the orbit pose while active
  double _baseDistance = 0; // pinch reference (gesture-start distance)

  /// Whether view mode is currently armed.
  bool get active => _pose != null;

  static const double _minPitch = 0.15, _maxPitch = 1.45;
  static const double _minDistance = 4.0, _maxDistance = 40.0;

  /// Arm view mode over [scene] (the game's last-pushed scene): the camera
  /// glides onto an orbit pose around the centre of the board (a scene push,
  /// so it tweens at the game's own camera timing).
  void enter({
    required TesseraController tessera,
    required TesseraScene scene,
  }) {
    if (active) return;
    _tessera = tessera;
    _scene = scene;
    final pose = _orbitPoseOf(scene);
    _pose = pose;
    tessera.setScene(_withCamera(scene, pose));
    notifyListeners();
  }

  /// Disarm: glide the camera back to the scene's own (initial) camera —
  /// again via the scene path, so the return home tweens instead of jumping.
  /// Safe to call when inactive.
  void exit() {
    if (!active) return;
    final tessera = _tessera;
    final scene = _scene;
    _pose = null;
    if (tessera != null && scene != null) {
      tessera.setScene(scene); // the original camera: tweens back home
    }
    notifyListeners();
  }

  /// A drag/pinch gesture began: remember the distance the pinch scales from.
  void beginGesture() {
    _baseDistance = _pose?.distance ?? 0;
  }

  /// One drag/pinch update: [delta] (the focal point's movement) orbits —
  /// horizontal spins the yaw, vertical tilts the pitch — and [scale] (the
  /// pinch factor since the gesture began, 1 = none) zooms the distance.
  void gesture(Offset delta, double scale) {
    final p = _pose;
    if (p == null) return;
    _pose = TesseraCameraPose(
      focusX: p.focusX,
      focusY: p.focusY,
      distance: (_baseDistance / (scale <= 0 ? 1 : scale))
          .clamp(_minDistance, _maxDistance)
          .toDouble(),
      yaw: p.yaw - delta.dx * 0.010,
      pitch: (p.pitch + delta.dy * 0.008).clamp(_minPitch, _maxPitch).toDouble(),
      fov: p.fov,
    );
    // The imperative camera API: retargets the camera alone — no scene
    // re-push, no state diff — and SNAPS instead of tweening, so the view
    // tracks the finger frame-for-frame.
    _tessera?.setCamera(_pose!);
  }

  /// The orbit pose view mode starts from: the scene's own camera when it
  /// already is an orbit pose, else a default overview centred on the board
  /// (the centroid of the scene's tiles).
  static TesseraCameraPose _orbitPoseOf(TesseraScene scene) {
    final cam = scene.camera;
    if (cam is TesseraCameraPose) return cam;
    var fx = 0.0, fy = 0.0;
    if (scene.tiles.isNotEmpty) {
      for (final t in scene.tiles) {
        fx += t.x;
        fy += t.y;
      }
      fx /= scene.tiles.length;
      fy /= scene.tiles.length;
    }
    return TesseraCameraPose(
        focusX: fx, focusY: fy, distance: 14, yaw: 0, pitch: 0.9, fov: 0.72);
  }

  static TesseraScene _withCamera(TesseraScene s, TesseraCamera camera) =>
      TesseraScene(
        tiles: s.tiles,
        entities: s.entities,
        effects: s.effects,
        cards: s.cards,
        cardDraws: s.cardDraws,
        hands: s.hands,
        dice: s.dice,
        overlays: s.overlays,
        labels: s.labels,
        highlights: s.highlights,
        pointLights: s.pointLights,
        world: s.world,
        camera: camera,
        epoch: s.epoch,
      );
}
