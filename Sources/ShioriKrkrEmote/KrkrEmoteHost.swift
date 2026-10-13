// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.

import CatSystemKit
import CShioriKrkr
import Foundation
import Metal

/// The E-mote side of ShioriKrkr's host interface (`shiorikrkr_emote_host_t`): models read
/// with CatSystemKit, played by `KrkrEmotePlayer` and drawn with CatSystemKit's Metal
/// renderer into the engine's layer pixels. One per process; used on the engine's thread.
public final class KrkrEmoteHost {
    public nonisolated(unsafe) static let shared = KrkrEmoteHost()

    /// Engine-log lines from the host side (failures reading a model).
    public var log: ((String) -> Void)?

    private let gpu: EmoteGPU?
    private let queue: MTLCommandQueue?
    private var target: MTLTexture?
    /// The title's PSB seed, once a model has given it (every model of a game shares it).
    private var seed: UInt32?
    /// Recently read models by file (a character is loaded again for each new layer).
    private var cache: [String: (model: EmoteModel, metadata: PSBFile.Value)] = [:]
    private var cacheOrder: [String] = []
    static let trace = ProcessInfo.processInfo.environment["SHIORIKRKR_TRACE_EMOTE"] != nil
    private var traceCount = 0

    init() {
        gpu = EmoteGPU(pixelFormat: .rgba8Unorm)
        queue = gpu?.device.makeCommandQueue()
    }

    /// The C table to put in `shiorikrkr_config_t.emote`. It points at `shared`.
    public static func makeTable() -> shiorikrkr_emote_host_t {
        var t = shiorikrkr_emote_host_t()
        t.struct_size = UInt32(MemoryLayout<shiorikrkr_emote_host_t>.size)
        t.context = Unmanaged.passUnretained(shared).toOpaque()
        t.create = { context, data, sizes, names, count, errorOut, capacity in
            let host = Unmanaged<KrkrEmoteHost>.fromOpaque(context!).takeUnretainedValue()
            var files: [(name: String, bytes: [UInt8])] = []
            for i in 0..<Int(count) {
                guard let d = data?[i], let size = sizes?[i] else { continue }
                let name = names?[i].map { String(cString: $0) } ?? "model \(i)"
                files.append((name, Array(UnsafeBufferPointer(start: d, count: Int(size)))))
            }
            do {
                let player = try host.makePlayer(files)
                return Unmanaged.passRetained(player).toOpaque()
            } catch {
                if let errorOut, capacity > 0 {
                    let text = Array("\(error)".utf8.prefix(Int(capacity) - 1)) + [0]
                    text.withUnsafeBufferPointer { b in
                        b.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) { errorOut.update(from: $0, count: b.count) }
                    }
                }
                return nil
            }
        }
        t.clone = { _, player in
            guard let player else { return nil }
            let p = Unmanaged<KrkrEmotePlayer>.fromOpaque(player).takeUnretainedValue()
            return Unmanaged.passRetained(p.copy()).toOpaque()
        }
        t.destroy = { _, player in
            guard let player else { return }
            Unmanaged<KrkrEmotePlayer>.fromOpaque(player).release()
        }
        t.command = { _, player, verb, label, numbers, count in
            guard let player, let verb else { return }
            let p = Unmanaged<KrkrEmotePlayer>.fromOpaque(player).takeUnretainedValue()
            let n = (0..<Int(count)).map { numbers![$0] }
            KrkrEmoteHost.command(p, String(cString: verb), label.map { String(cString: $0) } ?? "", n)
        }
        t.query = { _, player, verb, label, numbers, count, out, capacity in
            guard let player, let verb else { return 0 }
            let p = Unmanaged<KrkrEmotePlayer>.fromOpaque(player).takeUnretainedValue()
            let n = (0..<Int(count)).map { numbers![$0] }
            let (value, text) = KrkrEmoteHost.query(p, String(cString: verb), label.map { String(cString: $0) } ?? "", n)
            if let text, let out, capacity > 0 {
                let bytes = Array(text.utf8.prefix(Int(capacity) - 1)) + [0]
                bytes.withUnsafeBufferPointer { b in
                    b.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) { out.update(from: $0, count: b.count) }
                }
            }
            return value
        }
        t.draw = { context, player, rgba, width, height, pitch, transform in
            guard let context, let player, let rgba, let transform else { return 0 }
            let host = Unmanaged<KrkrEmoteHost>.fromOpaque(context).takeUnretainedValue()
            let p = Unmanaged<KrkrEmotePlayer>.fromOpaque(player).takeUnretainedValue()
            let m = (0..<6).map { transform[$0] }
            return host.draw(p, into: rgba, width: Int(width), height: Int(height), pitch: Int(pitch), transform: m) ? 1 : 0
        }
        return t
    }

    // MARK: Models

    func makePlayer(_ files: [(name: String, bytes: [UInt8])]) throws -> KrkrEmotePlayer {
        var models: [EmoteModel] = []
        var metadata: [PSBFile.Value] = []
        for file in files {
            let key = "\(file.name)#\(file.bytes.count)"
            if let hit = cache[key] {
                models.append(hit.model)
                metadata.append(hit.metadata)
                continue
            }
            let psb = try PSBFile(psz: file.bytes, bodySeed: seed)
            if psb.seed != 0 { seed = psb.seed }
            let model = try EmoteModel(psb: psb)
            let meta = psb.root["metadata"] ?? .null
            if let gpu { _ = gpu.textures(for: model) }
            cache[key] = (model, meta)
            cacheOrder.append(key)
            if cacheOrder.count > 8 { cache[cacheOrder.removeFirst()] = nil }
            models.append(model)
            metadata.append(meta)
        }
        guard !models.isEmpty else { throw PSBFile.Failure.notPSB }
        return KrkrEmotePlayer(models: models, metadata: metadata)
    }

    // MARK: Commands

    static func command(_ p: KrkrEmotePlayer, _ verb: String, _ label: String, _ n: [Double]) {
        func at(_ i: Int, _ fallback: Double = 0) -> Double { i < n.count ? n[i] : fallback }
        switch verb {
        case "progress": p.progress(at(0))
        case "skip": p.skip()
        case "pass": p.skip()
        case "show": p.shown = true
        case "hide": p.shown = false
        case "coord":
            p.coordX.set(at(0), frames: at(2), easing: at(3), clock: p.clock)
            p.coordY.set(at(1), frames: at(2), easing: at(3), clock: p.clock)
        case "rot": p.rotation.set(at(0), frames: at(1), easing: at(2), clock: p.clock)
        case "scale": p.scale.set(at(0, 1), frames: at(1), easing: at(2), clock: p.clock)
        case "color": p.setColor(UInt32(truncatingIfNeeded: Int64(at(0, 0xFFFF_FFFF))), frames: at(1), easing: at(2))
        case "variable": p.setVariable(label, at(0), frames: at(1), easing: at(2))
        case "play": p.play(label, flags: Int(at(0)))
        case "stop": p.stop(label)
        case "fadeout": p.fadeOut(label, frames: at(0), easing: at(1))
        case "blend": p.setBlend(label, at(0, 1), frames: at(1), easing: at(2), stopAtZero: at(3) != 0)
        case "wind": p.wind = (at(0), at(1), at(2), at(3), at(4))
        case "windstop": p.wind = nil
        case "controls": p.controlScales = (at(0, 1), at(1, 1), at(2, 1), at(3, 1))
        default: break
        }
    }

    static func query(_ p: KrkrEmotePlayer, _ verb: String, _ label: String, _ n: [Double]) -> (Double, String?) {
        switch verb {
        case "variable": return (p.variable(label), nil)
        case "playing": return (p.isPlaying(label) ? 1 : 0, nil)
        case "blend": return (p.blend(label), nil)
        case "animating": return (p.animating ? 1 : 0, nil)
        case "count":
            switch label {
            case "main": return (Double(p.mainTimelines.count), nil)
            case "diff": return (Double(p.differenceTimelines.count), nil)
            default: return (Double(p.playingLabels.count), nil)
            }
        case "label":
            let list = label == "main" ? p.mainTimelines : label == "diff" ? p.differenceTimelines : p.playingLabels
            let i = Int(n.first ?? 0)
            return (0, i >= 0 && i < list.count ? list[i] : "")
        case "flags": return (Double(p.playingFlags(Int(n.first ?? 0))), nil)
        default: return (0, nil)
        }
    }

    // MARK: Drawing

    func draw(_ p: KrkrEmotePlayer, into rgba: UnsafeMutablePointer<UInt8>, width: Int, height: Int, pitch: Int,
              transform: [Double]) -> Bool {
        guard p.shown, let gpu, let queue, width > 0, height > 0 else { return false }
        let tint = p.tint
        guard tint.a > 0 else { return false }
        if target == nil || target!.width != width || target!.height != height {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: width, height: height, mipmapped: false)
            d.usage = [.renderTarget, .shaderRead]
            d.storageMode = .shared
            target = gpu.device.makeTexture(descriptor: d)
        }
        guard let target, let buffer = queue.makeCommandBuffer() else { return false }
        let pose = p.pose()
        let place = p.placement(transform)
        var prepared: [EmoteGPU.Prepared] = []
        for (i, renderer) in p.renderers.enumerated() {
            var variables = pose
            p.models[i].applySelectors(&variables)
            let list = renderer.drawList(variables: variables, transform: place)
            if let ready = gpu.prepare(list, model: p.models[i], buffer: buffer) { prepared.append(ready) }
        }
        if Self.trace {
            traceCount += 1
            if traceCount % 120 == 1 {
                let v = pose.filter { $0.value != 0 }.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }
                FileHandle.standardError.write(("emote host: place \(place) tint \(tint) coord \(p.coordX.current(p.clock)),\(p.coordY.current(p.clock)) scale \(p.scale.current(p.clock)) items \(prepared.map { $0.itemCount }) timelines \(p.playingLabels) \(v)\n").data(using: .utf8)!)
            }
        }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pass.colorAttachments[0].storeAction = .store
        guard let encoder = buffer.makeRenderCommandEncoder(descriptor: pass) else { return false }
        for ready in prepared { gpu.draw(ready, encoder: encoder, width: width, height: height) }
        encoder.endEncoding()
        buffer.commit()
        buffer.waitUntilCompleted()
        target.getBytes(rgba, bytesPerRow: pitch, from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
        // Premultiplied → straight alpha (the engine's layers), with the game's colour.
        let r = tint.r, g = tint.g, b = tint.b, a = tint.a
        let plain = r == 1 && g == 1 && b == 1 && a == 1
        for y in 0..<height {
            let row = UnsafeMutableRawPointer(rgba + y * pitch).assumingMemoryBound(to: UInt32.self)
            for x in 0..<width where row[x] != 0 {
                let px = UnsafeMutableRawPointer(row + x).assumingMemoryBound(to: UInt8.self)
                let alpha = Int(px[3])
                guard alpha > 0 else { row[x] = 0; continue }
                if alpha < 255 {
                    px[0] = UInt8(min(255, Int(px[0]) * 255 / alpha))
                    px[1] = UInt8(min(255, Int(px[1]) * 255 / alpha))
                    px[2] = UInt8(min(255, Int(px[2]) * 255 / alpha))
                }
                if !plain {
                    px[0] = UInt8(Double(px[0]) * r)
                    px[1] = UInt8(Double(px[1]) * g)
                    px[2] = UInt8(Double(px[2]) * b)
                    px[3] = UInt8(Double(px[3]) * a)
                }
            }
        }
        return true
    }
}
