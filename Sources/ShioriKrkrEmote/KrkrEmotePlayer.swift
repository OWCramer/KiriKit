// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.

import CatSystemKit
import Foundation

/// One E-mote character as a KiriKiri game plays it (NEKOPARA Vol. 0–2's D3DEmotePlayer):
/// Shiori's own player over CatSystemKit's model and renderer.
///
/// What the game asks of it, as the scripts use it:
/// - variables (`head_LR`, `face_eye_open` …) set at once or moved over a number of frames
///   with an easing;
/// - timelines from the model's `timelineControl` played by label: flag 2 marks a
///   difference timeline, whose values are added to the pose scaled by its blend ratio
///   (the idle `差分用_waiting_loop` breathing); the others set their variables, mixed in by
///   their blend ratio. A timeline with loop points wraps from its end back to its loop
///   start; one without ends at its last key and stops playing;
/// - the character's place (`setCoord`, canvas pixels from the centre), turn, scale and
///   colour (`0xRRGGBBAA`, multiplied in), each optionally moved over frames;
/// - the model's own controllers: blinking (`eyeControl`) and hair, bust and parts swing
///   (`hairControl`, `bustControl`, `partsControl`), see `EmoteControllers`.
///
/// Frames are the game's 60ths of a second.
public final class KrkrEmotePlayer {
    public let models: [EmoteModel]
    let renderers: [EmoteRenderer]

    /// The variables as set (before timelines and controllers).
    private(set) var variables: [String: Double] = [:]
    private var moving: [String: Move] = [:]
    private(set) var clock = 0.0
    var shown = false

    struct Timeline {
        let label: String
        var time = 0.0
        var flags: Int
        var blend = 1.0
        var blendMove: Move?
        /// Fading out: the blend heads to 0 and the timeline stops there.
        var stopAtZero = false
        let length: Double
        let loopStart: Double?
        var playing = true
        var difference: Bool { flags & 2 != 0 }
    }
    private(set) var timelines: [Timeline] = []

    var coordX = Animated(0), coordY = Animated(0)
    var rotation = Animated(0), scale = Animated(1)
    var red = Animated(1), green = Animated(1), blue = Animated(1), alpha = Animated(1)
    var controllers: EmoteControllers
    var controlScales = (mesh: 1.0, bust: 1.0, hair: 1.0, parts: 1.0)
    var wind: (start: Double, goal: Double, speed: Double, min: Double, max: Double)?

    /// A value moving from `from` to `to` over `frames` from `start` (the player's clock).
    struct Move {
        let from: Double, to: Double, start: Double, frames: Double, easing: Double
        func value(at t: Double) -> Double {
            guard frames > 0 else { return to }
            let p = min(1, max(0, (t - start) / frames))
            return from + (to - from) * Self.ease(p, easing)
        }
        func done(at t: Double) -> Bool { t - start >= frames }
        /// KiriKiri's acceleration: positive starts slowly, negative ends slowly, 0 is even.
        static func ease(_ p: Double, _ e: Double) -> Double {
            if e > 0 { return pow(p, 1 + e) }
            if e < 0 { return 1 - pow(1 - p, 1 - e) }
            return p
        }
    }

    struct Animated {
        var value: Double
        var move: Move?
        init(_ v: Double) { value = v }
        mutating func set(_ v: Double, frames: Double, easing: Double, clock: Double) {
            if frames > 0 { move = Move(from: current(clock), to: v, start: clock, frames: frames, easing: easing) }
            else { move = nil }
            value = v
        }
        func current(_ clock: Double) -> Double { move?.value(at: clock) ?? value }
        mutating func settle(_ clock: Double) { if let m = move, m.done(at: clock) { move = nil } }
        mutating func finish() { move = nil }
    }

    let metadata: [PSBFile.Value]

    /// `metadata`: each model's PSB `metadata` (its controllers).
    public init(models: [EmoteModel], metadata: [PSBFile.Value]) {
        self.models = models
        self.metadata = metadata
        renderers = models.map(EmoteRenderer.init)
        controllers = EmoteControllers(metadata: metadata)
        for model in models {
            var rest = model.restingVariables()
            model.applySelectors(&rest)
            for (k, v) in rest where variables[k] == nil { variables[k] = v }
        }
    }

    /// A copy in the same state (the game's layer copies for a transition).
    public func copy() -> KrkrEmotePlayer {
        let other = KrkrEmotePlayer(models: models, metadata: metadata)
        other.variables = variables
        other.moving = moving
        other.clock = clock
        other.shown = shown
        other.timelines = timelines
        other.coordX = coordX; other.coordY = coordY
        other.rotation = rotation; other.scale = scale
        other.red = red; other.green = green; other.blue = blue; other.alpha = alpha
        other.controllers = controllers
        other.controlScales = controlScales
        other.wind = wind
        other.motionPlay = motionPlay
        for (a, b) in zip(other.renderers, renderers) { a.object = b.object; a.motion = b.motion }
        return other
    }

    // MARK: Labels

    public var mainTimelines: [String] { timelineLabels(difference: false) }
    public var differenceTimelines: [String] { timelineLabels(difference: true) }

    private func timelineLabels(difference: Bool) -> [String] {
        var out: [String] = []
        for model in models {
            for (label, t) in model.timelines.sorted(by: { $0.key < $1.key }) where t.difference == difference && !out.contains(label) {
                out.append(label)
            }
        }
        return out
    }

    public var variableNames: [String] {
        var out: [String] = []
        for model in models { for n in model.variableNames where !out.contains(n) { out.append(n) } }
        return out
    }

    /// Whether a timeline loops (has loop points).
    public func loops(_ label: String) -> Bool {
        guard let t = timeline(label) else { return false }
        return t.loopEnd > t.loopBegin && t.loopBegin >= 0
    }

    /// A timeline's length in frames (its last key).
    public func length(_ label: String) -> Double {
        guard let t = timeline(label) else { return 0 }
        return t.tracks.values.compactMap { $0.last?.time }.max() ?? 0
    }

    private func timeline(_ label: String) -> EmoteModel.Timeline? {
        for model in models { if let t = model.timelines[label] { return t } }
        return nil
    }

    // MARK: Commands

    public func setVariable(_ name: String, _ value: Double, frames: Double, easing: Double) {
        if frames > 0 {
            moving[name] = Move(from: currentVariable(name), to: value, start: clock, frames: frames, easing: easing)
        } else {
            moving[name] = nil
        }
        variables[name] = value
    }

    func currentVariable(_ name: String) -> Double {
        moving[name]?.value(at: clock) ?? variables[name] ?? 0
    }

    /// What the game reads back: the variable as set (moving included).
    public func variable(_ name: String) -> Double { currentVariable(name) }

    /// A plain motion file's motion playing by name (Motion.Player): object, motion, when it
    /// started on the clock.
    private(set) var motionPlay: (object: String, motion: String, start: Double)?

    /// Where the playing motion is (frames), looping or holding its last frame as it says.
    var motionTime: Double {
        guard let m = motionPlay, let motion = models.first?.objects[m.object]?[m.motion] else { return 0 }
        return EmoteRenderer.playTime(clock - m.start, of: motion)
    }

    /// Whether the playing motion has frames still to come (one that loops never ends).
    private var motionRunning: Bool {
        guard let m = motionPlay, let motion = models.first?.objects[m.object]?[m.motion] else { return false }
        return motion.loopTime >= 0 || clock - m.start < motion.lastTime
    }

    /// Plays `label` as one of the model's motions (the base object's first, else any
    /// object's), as Motion.Player.play does for a plain motion file. False when there is none.
    private func playMotion(_ label: String) -> Bool {
        guard let model = models.first else { return false }
        let object = model.objects[model.baseObject]?[label] != nil ? model.baseObject
            : model.objects.keys.sorted().first { model.objects[$0]?[label] != nil }
        guard let object else { return false }
        motionPlay = (object, label, clock)
        for r in renderers { r.object = object; r.motion = label }
        return true
    }

    public func play(_ label: String, flags: Int) {
        guard let t = timeline(label) else {
            _ = playMotion(label)
            return
        }
        var length = 0.0
        for keys in t.tracks.values { if let last = keys.last { length = max(length, last.time) } }
        let loops = t.loopEnd > t.loopBegin && t.loopBegin >= 0
        let state = Timeline(label: label, flags: flags | (t.difference ? 2 : 0), length: loops ? t.loopEnd : length,
                             loopStart: loops ? t.loopBegin : nil)
        timelines.removeAll { $0.label == label }
        timelines.append(state)
    }

    public func stop(_ label: String) {
        if label.isEmpty { timelines.removeAll() } else { timelines.removeAll { $0.label == label } }
    }

    public func fadeOut(_ label: String, frames: Double, easing: Double) {
        guard let i = timelines.firstIndex(where: { $0.label == label }) else { return }
        guard frames > 0 else { timelines.remove(at: i); return }
        let from = blendValue(timelines[i])
        timelines[i].blendMove = Move(from: from, to: 0, start: clock, frames: frames, easing: easing)
        timelines[i].blend = 0
        timelines[i].stopAtZero = true
    }

    public func setBlend(_ label: String, _ ratio: Double, frames: Double, easing: Double, stopAtZero: Bool) {
        guard let i = timelines.firstIndex(where: { $0.label == label }) else { return }
        if frames > 0 {
            timelines[i].blendMove = Move(from: blendValue(timelines[i]), to: ratio, start: clock, frames: frames, easing: easing)
        } else {
            timelines[i].blendMove = nil
        }
        timelines[i].blend = ratio
        timelines[i].stopAtZero = stopAtZero && ratio == 0
    }

    private func blendValue(_ t: Timeline) -> Double { t.blendMove?.value(at: clock) ?? t.blend }

    public func isPlaying(_ label: String) -> Bool {
        if let m = motionPlay, label.isEmpty || m.motion == label { return motionRunning }
        return timelines.contains { $0.label == label && $0.playing }
    }
    public func blend(_ label: String) -> Double { timelines.first { $0.label == label }.map(blendValue) ?? 0 }
    public var playingLabels: [String] { timelines.filter(\.playing).map(\.label) }
    public func playingFlags(_ index: Int) -> Int {
        let playing = timelines.filter(\.playing)
        return index < playing.count ? playing[index].flags : 0
    }

    /// Whether anything is still moving towards an end: a timeline that ends, a variable,
    /// place or blend on its way (looping timelines and the controllers don't count).
    public var animating: Bool {
        if let m = motionPlay, let motion = models.first?.objects[m.object]?[m.motion], motion.loopTime < 0,
           clock - m.start < motion.lastTime { return true }
        if timelines.contains(where: { $0.playing && ($0.loopStart == nil || $0.blendMove != nil) }) { return true }
        if !moving.isEmpty { return true }
        return [coordX, coordY, rotation, scale, red, green, blue, alpha].contains { $0.move != nil }
    }

    /// Ends every move and every timeline that ends (the game skipping ahead).
    public func skip() {
        for (k, m) in moving { variables[k] = m.to }
        moving.removeAll()
        for a in [\KrkrEmotePlayer.coordX, \.coordY, \.rotation, \.scale, \.red, \.green, \.blue, \.alpha] { self[keyPath: a].finish() }
        timelines = timelines.compactMap { t in
            var t = t
            if t.stopAtZero { return nil }
            t.blendMove = nil
            if t.loopStart == nil { t.time = t.length; t.playing = false }
            return t
        }
    }

    /// Moves time on by `frames`.
    public func progress(_ frames: Double) {
        guard frames > 0 else { return }
        clock += frames
        for (k, m) in moving where m.done(at: clock) { variables[k] = m.to; moving[k] = nil }
        for a in [\KrkrEmotePlayer.coordX, \.coordY, \.rotation, \.scale, \.red, \.green, \.blue, \.alpha] { self[keyPath: a].settle(clock) }
        timelines = timelines.compactMap { t in
            var t = t
            if let m = t.blendMove, m.done(at: clock) {
                t.blendMove = nil
                if t.stopAtZero && t.blend == 0 { return nil }
            }
            guard t.playing else { return t }
            t.time += frames
            if t.length > 0, t.time >= t.length {
                if let start = t.loopStart, t.length > start {
                    while t.time >= t.length { t.time -= t.length - start }
                } else {
                    t.time = t.length
                    t.playing = false
                }
            }
            return t
        }
        controllers.step(frames: frames, pose: pose(applyControllers: false), place: (coordX.current(clock), coordY.current(clock)),
                         scales: controlScales)
    }

    // MARK: Pose

    /// The variables to draw with: as set, then the timelines, then the controllers.
    func pose(applyControllers: Bool = true) -> [String: Double] {
        var v = variables
        for (k, m) in moving { v[k] = m.value(at: clock) }
        for t in timelines {
            guard let model = models.first(where: { $0.timelines[t.label] != nil }) else { continue }
            let values = model.timelineValues(t.label, at: t.time)
            let ratio = blendValue(t)
            for (k, value) in values {
                if t.difference { v[k, default: 0] += value * ratio }
                else { v[k] = (v[k] ?? 0) * (1 - ratio) + value * ratio }
            }
        }
        if applyControllers { controllers.apply(to: &v) }
        return v
    }

    // MARK: Drawing

    /// The affine from the character's own space (its origin) to the target, under the
    /// layer's `transform` (a, b, c, d, tx, ty).
    func placement(_ transform: [Double]) -> (a: Double, b: Double, c: Double, d: Double, tx: Double, ty: Double) {
        let s = scale.current(clock), r = rotation.current(clock)
        let x = coordX.current(clock), y = coordY.current(clock)
        // Character: p' = R·S·p + (x, y); then the layer's matrix.
        let ca = cos(r) * s, sa = sin(r) * s
        let (A, B, C, D, TX, TY) = (transform[0], transform[1], transform[2], transform[3], transform[4], transform[5])
        return (a: A * ca + C * sa, b: B * ca + D * sa,
                c: A * -sa + C * ca, d: B * -sa + D * ca,
                tx: A * x + C * y + TX, ty: B * x + D * y + TY)
    }

    /// Whether (x, y), in the player's coordinates (before the layer's matrix), is inside the
    /// shape layer `label` (a hit area such as `hit_head`) as currently posed; with no label,
    /// inside any shape layer.
    public func contains(_ label: String, x: Double, y: Double) -> Bool {
        let place = placement([1, 0, 0, 1, 0, 0])
        let pose = self.pose()
        for (i, renderer) in renderers.enumerated() {
            var variables = pose
            models[i].applySelectors(&variables)
            let shapes = renderer.shapes(variables: variables, transform: place)
            let candidates = label.isEmpty ? shapes.values.flatMap { $0 } : shapes[label] ?? []
            if candidates.contains(where: { $0.contains(x, y) }) { return true }
        }
        return false
    }

    var tint: (r: Double, g: Double, b: Double, a: Double) {
        (red.current(clock), green.current(clock), blue.current(clock), alpha.current(clock))
    }

    /// E-mote's colour multiplies with 0x80 as neutral for red, green and blue (games reset a
    /// character with 0xFF808080; values above 0x80 brighten). Alpha's 0xFF is opaque.
    public func setColor(_ rgba: UInt32, frames: Double, easing: Double) {
        let parts = [Double(rgba >> 24 & 0xFF) / 128, Double(rgba >> 16 & 0xFF) / 128, Double(rgba >> 8 & 0xFF) / 128,
                     Double(rgba & 0xFF) / 255]
        red.set(parts[0], frames: frames, easing: easing, clock: clock)
        green.set(parts[1], frames: frames, easing: easing, clock: clock)
        blue.set(parts[2], frames: frames, easing: easing, clock: clock)
        alpha.set(parts[3], frames: frames, easing: easing, clock: clock)
    }
}
