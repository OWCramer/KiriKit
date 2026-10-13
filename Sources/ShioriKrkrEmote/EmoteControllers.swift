// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.

import CatSystemKit
import Foundation

/// An E-mote model's own controllers, from its PSB `metadata`: automatic blinking
/// (`eyeControl`) and hair, bust and parts swing (`hairControl`, `bustControl`,
/// `partsControl`). They write their variables after the timelines each frame.
///
/// the user's design from Shiori's AetherKrkr extension (patch 0009, ShioriEmoteExtension.cpp),
/// re-homed here in Swift; matched against the shipped games' footage, not M2's SDK:
/// - a blink closes the eye from where it is to the control's `endFrame` and opens it
///   again over `blinkFrameCount` frames, every `blinkIntervalMin`…`Max` frames, and only
///   while the eye is ordinarily open (the control's first `edge` range, below closed);
/// - each hair or parts strand is a two-segment pendulum hanging from the character, each
///   bust a damped spring; they swing when the character's head, body or place moves.
struct EmoteControllers {
    struct Eye {
        let label: String
        var closed = 10.0, frames = 16.0, intervalMin = 30.0, intervalMax = 180.0
        var openFrom = -10.0
    }

    struct Spring {
        enum Kind { case bust, strand }
        var kind = Kind.strand
        var lr = "", lrm = "", ud = ""
        var gravity = 0.2, frictionX = 0.03125, frictionY = 0.03125, stiffness = 0.03125
        var length = (64.0, 48.0), scaleX = (1.0, 1.0), scaleY = (1.0, 1.0)
        /// 0 bust, 1 hair, 2 parts: which of the game's scales applies.
        var group = 0
    }

    struct EyeState {
        var wait = 0.0
        var phase = -1.0
        var base = 0.0
        var applied = false
    }

    struct SpringState {
        var angle = (0.0, 0.0), velocity = (0.0, 0.0)
        var y = 0.0, vy = 0.0
        var anchor: (x: Double, y: Double)?
        var anchorV = (0.0, 0.0)
    }

    var eyes: [Eye] = []
    var springs: [Spring] = []
    var eyeStates: [EyeState] = []
    var springStates: [SpringState] = []
    var scales = (mesh: 1.0, bust: 1.0, hair: 1.0, parts: 1.0)
    var blinking = true
    var physics = true

    init(metadata: [PSBFile.Value]) {
        for m in metadata {
            for e in Self.enabled(m["eyeControl"]) where (e["blinkEnabled"]?.number ?? 0) != 0 {
                guard let label = e["label"]?.string, !label.isEmpty else { continue }
                var eye = Eye(label: label)
                eye.closed = e["endFrame"]?.number ?? eye.closed
                eye.frames = max(2, e["blinkFrameCount"]?.number ?? eye.frames)
                eye.intervalMin = e["blinkIntervalMin"]?.number ?? eye.intervalMin
                eye.intervalMax = max(eye.intervalMin, e["blinkIntervalMax"]?.number ?? eye.intervalMax)
                eye.openFrom = e["beginFrame"]?.number ?? 0
                if let first = e["edge"]?.list?.first?.numbers?.first { eye.openFrom = first }
                if !eyes.contains(where: { $0.label == label }) { eyes.append(eye) }
            }
            for (key, group) in [("bustControl", 0), ("hairControl", 1), ("partsControl", 2)] {
                for s in Self.enabled(m[key]) {
                    var spring = Spring()
                    spring.group = group
                    spring.kind = group == 0 ? .bust : .strand
                    spring.lr = s["var_lr"]?.string ?? ""
                    spring.lrm = s["var_lrm"]?.string ?? ""
                    spring.ud = s["var_ud"]?.string ?? ""
                    spring.gravity = s["gravity"]?.number ?? spring.gravity
                    let friction = s["friction"]?.number ?? spring.frictionX
                    spring.frictionX = s["friction_x"]?.number ?? friction
                    spring.frictionY = s["friction_y"]?.number ?? friction
                    spring.stiffness = s["spring"]?.number ?? spring.stiffness
                    spring.length = Self.pair(s["length"], spring.length)
                    spring.scaleX = Self.pair(s["scale_x"], spring.scaleX)
                    spring.scaleY = Self.pair(s["scale_y"], spring.scaleY)
                    if !spring.lr.isEmpty || !spring.ud.isEmpty { springs.append(spring) }
                }
            }
        }
        eyeStates = eyes.map { eye in
            var s = EyeState()
            s.wait = Double.random(in: eye.intervalMin...eye.intervalMax) * 0.5
            return s
        }
        springStates = Array(repeating: SpringState(), count: springs.count)
    }

    private static func enabled(_ v: PSBFile.Value?) -> [PSBFile.Value] {
        (v?.list ?? []).filter { ($0["enabled"]?.number ?? 0) != 0 }
    }

    private static func pair(_ v: PSBFile.Value?, _ fallback: (Double, Double)) -> (Double, Double) {
        if let n = v?.numbers, let first = n.first { return (first, n.count > 1 ? n[1] : first) }
        if let x = v?.number { return (x, x) }
        return fallback
    }

    private func value(_ pose: [String: Double], _ name: String) -> Double { pose[name] ?? 0 }

    /// Moves the controllers on by `frames`, the character's pose (before them) and place
    /// in hand.
    mutating func step(frames dt: Double, pose: [String: Double], place: (Double, Double),
                       scales: (mesh: Double, bust: Double, hair: Double, parts: Double)) {
        self.scales = scales
        guard dt > 0 else { return }
        for i in eyes.indices {
            if eyeStates[i].phase >= 0 {
                eyeStates[i].phase += dt
                if eyeStates[i].phase >= eyes[i].frames {
                    eyeStates[i].phase = -1
                    eyeStates[i].wait = Double.random(in: eyes[i].intervalMin...eyes[i].intervalMax)
                }
            } else {
                eyeStates[i].wait -= dt
                if eyeStates[i].wait <= 0 {
                    if blinking {
                        eyeStates[i].phase = 0
                    } else {
                        eyeStates[i].wait = Double.random(in: eyes[i].intervalMin...eyes[i].intervalMax)
                    }
                }
            }
        }
        guard physics, !springs.isEmpty else { return }
        // Strands and busts hang from the character: they follow its place and the head and
        // body variables its breathing and gestures animate. Busts follow the body only.
        let bodyX = value(pose, "body_LR"), bodyY = value(pose, "body_UD")
        let headX = value(pose, "head_LR"), headY = value(pose, "head_UD")
        let moveX = value(pose, "move_LR") + place.0, moveY = value(pose, "move_UD") + place.1
        let steps = max(1, Int(dt.rounded(.up)))
        let h = dt / Double(steps)
        for i in springs.indices {
            let spring = springs[i]
            var state = springStates[i]
            let bust = spring.kind == .bust
            let ax0 = moveX + bodyX + (bust ? 0 : headX), ay0 = moveY + bodyY + (bust ? 0 : headY)
            let anchor = state.anchor ?? (ax0, ay0)
            let stepX = (ax0 - anchor.x) / Double(steps), stepY = (ay0 - anchor.y) / Double(steps)
            for _ in 0..<steps {
                // The anchor's velocity is smoothed first: uneven frame times would turn a raw
                // second difference into jitter.
                let blend = 1 - pow(1 - 0.15, h)
                let vx = state.anchorV.0 + (stepX / h - state.anchorV.0) * blend
                let vy = state.anchorV.1 + (stepY / h - state.anchorV.1) * blend
                let ax = (vx - state.anchorV.0) / h, ay = (vy - state.anchorV.1) / h
                state.anchorV = (vx, vy)
                Self.integrate(spring, &state, ax: ax, ay: ay, h: h)
            }
            state.anchor = (ax0, ay0)
            springStates[i] = state
        }
    }

    private static func integrate(_ c: Spring, _ s: inout SpringState, ax: Double, ay: Double, h: Double) {
        if c.kind == .bust {
            // A damped spring pushed against the body's acceleration.
            let k = c.stiffness
            s.velocity.0 += (-k * s.angle.0 - ax * 0.5) * h
            s.velocity.0 *= pow(1 - c.frictionX, h)
            s.angle.0 += s.velocity.0 * h
            s.vy += (-k * s.y - ay * 0.5) * h
            s.vy *= pow(1 - c.frictionY, h)
            s.y += s.vy * h
            return
        }
        // Two pendulum segments: the first hangs from the head, the second from the first's
        // tip; gravity pulls each back towards hanging straight.
        var pivot = ax
        for segment in 0..<2 {
            let length = max(1, segment == 0 ? c.length.0 : c.length.1)
            var angle = segment == 0 ? s.angle.0 : s.angle.1
            var velocity = segment == 0 ? s.velocity.0 : s.velocity.1
            let accel = -(c.gravity / length) * sin(angle) * 8 - (pivot / length) * cos(angle)
            velocity += accel * h
            velocity *= pow(1 - c.frictionX, h)
            angle = min(0.8, max(-0.8, angle + velocity * h))
            if segment == 0 { s.angle.0 = angle; s.velocity.0 = velocity } else { s.angle.1 = angle; s.velocity.1 = velocity }
            pivot = ax + accel * length * cos(angle)
        }
        // The strand's vertical bounce.
        let k = c.gravity / max(1, c.length.0) * 8
        s.vy += (-k * s.y - ay * 0.5) * h
        s.vy *= pow(1 - c.frictionY, h)
        s.y += s.vy * h
    }

    /// Writes the blink and the swing into the pose.
    mutating func apply(to v: inout [String: Double]) {
        for i in eyes.indices {
            let eye = eyes[i]
            if eyeStates[i].phase < 0 { eyeStates[i].applied = false; continue }
            if !eyeStates[i].applied {
                // Only ordinarily open eyes blink; closed, smiling and special eyes don't.
                let base = v[eye.label] ?? 0
                if base < eye.openFrom - 0.001 || base >= eye.closed {
                    eyeStates[i].phase = -1
                    eyeStates[i].wait = Double.random(in: eye.intervalMin...eye.intervalMax)
                    continue
                }
                eyeStates[i].base = base
                eyeStates[i].applied = true
            }
            // As the PC game blinks: a quick close (~30%), held shut (~25%), then a slower
            // opening, eased so the half-closed states pass quickly.
            let t = eyeStates[i].phase / eye.frames
            func ease(_ x: Double) -> Double { let x = min(1, max(0, x)); return x * x * (3 - 2 * x) }
            let closed = t < 0.3 ? ease(t / 0.3) : t < 0.55 ? 1 : 1 - ease((t - 0.55) / 0.45)
            let base = eyeStates[i].base
            v[eye.label] = base + (eye.closed - base) * closed
        }
        guard physics else { return }
        let degrees = 180 / Double.pi
        for i in springs.indices {
            let c = springs[i], s = springStates[i]
            let scale = c.group == 0 ? scales.bust : c.group == 1 ? scales.hair : scales.parts
            func out(_ label: String, _ value: Double) {
                guard !label.isEmpty else { return }
                v[label] = min(30, max(-30, value * scale))
            }
            if c.kind == .bust {
                out(c.lr, s.angle.0 * c.scaleX.0)
                out(c.ud, s.y * c.scaleY.0)
            } else {
                out(c.lr, s.angle.0 * degrees * c.scaleX.0)
                out(c.lrm, (s.angle.1 - s.angle.0) * degrees * c.scaleX.1)
                out(c.ud, s.y * c.scaleY.0)
            }
        }
    }
}
