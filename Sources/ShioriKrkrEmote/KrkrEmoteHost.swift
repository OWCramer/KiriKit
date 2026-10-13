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
    private var finished: MTLTexture?
    /// The drawing target for GPU drawing (into the engine's buffers).
    private var gpuTarget: MTLTexture?
    private let finisher: EmoteFinisher?
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
        finisher = gpu.flatMap { EmoteFinisher(device: $0.device) }
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
        t.draw_gpu = { context, player, buffer, width, height, pitch, transform, commandBuffer, blend in
            guard let context, let player, let buffer, let transform, let commandBuffer else { return 0 }
            let host = Unmanaged<KrkrEmoteHost>.fromOpaque(context).takeUnretainedValue()
            let p = Unmanaged<KrkrEmotePlayer>.fromOpaque(player).takeUnretainedValue()
            let out = Unmanaged<AnyObject>.fromOpaque(buffer).takeUnretainedValue() as! MTLBuffer
            let commands = Unmanaged<AnyObject>.fromOpaque(commandBuffer).takeUnretainedValue() as! MTLCommandBuffer
            let m = (0..<6).map { transform[$0] }
            return host.drawGPU(p, into: out, width: Int(width), height: Int(height), pitch: Int(pitch), transform: m,
                                commands: commands, blend: blend != 0) ? 1 : 0
        }
        t.draw = { context, player, rgba, width, height, pitch, transform, bounds in
            guard let context, let player, let rgba, let transform, let bounds else { return 0 }
            let host = Unmanaged<KrkrEmoteHost>.fromOpaque(context).takeUnretainedValue()
            let p = Unmanaged<KrkrEmotePlayer>.fromOpaque(player).takeUnretainedValue()
            let m = (0..<6).map { transform[$0] }
            guard let r = host.draw(p, into: rgba, width: Int(width), height: Int(height), pitch: Int(pitch), transform: m) else { return 0 }
            bounds[0] = Int32(r.0); bounds[1] = Int32(r.1); bounds[2] = Int32(r.2); bounds[3] = Int32(r.3)
            return 1
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
            let bytes = LZ4Frame.isFrame(file.bytes) ? try LZ4Frame.decode(file.bytes) : file.bytes
            let psb = try PSBFile(psz: bytes, bodySeed: seed)
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
            case "variables": return (Double(p.variableNames.count), nil)
            default: return (Double(p.playingLabels.count), nil)
            }
        case "label":
            let list = label == "main" ? p.mainTimelines : label == "diff" ? p.differenceTimelines
                : label == "variables" ? p.variableNames : p.playingLabels
            let i = Int(n.first ?? 0)
            return (0, i >= 0 && i < list.count ? list[i] : "")
        case "flags": return (Double(p.playingFlags(Int(n.first ?? 0))), nil)
        case "info":
            let model = p.models.first
            return (0, label == "chara" ? model?.baseObject ?? "" : model?.baseMotion ?? "")
        case "loop": return (p.loops(label) ? 1 : 0, nil)
        case "length": return (p.length(label), nil)
        case "contains":
            let hit = n.count >= 2 && p.contains(label, x: n[0], y: n[1])
            if trace { FileHandle.standardError.write("emote host: contains \(label) (\(n)) -> \(hit)\n".data(using: .utf8)!) }
            return (hit ? 1 : 0, nil)
        default: return (0, nil)
        }
    }

    // MARK: Drawing

    /// Encodes `p` into a layer's buffer on the engine's command buffer (nothing waits): every
    /// pixel replaced (clear where the character isn't), or the character over them when
    /// `blend`. False when nothing was encoded.
    func drawGPU(_ p: KrkrEmotePlayer, into out: MTLBuffer, width: Int, height: Int, pitch: Int, transform: [Double],
                 commands: MTLCommandBuffer, blend: Bool) -> Bool {
        guard let gpu, let finisher, width > 0, height > 0, pitch % 4 == 0 else { return false }
        let tint = p.tint
        let visible = p.shown && tint.a > 0
        if !visible && blend { return false }
        if gpuTarget == nil || gpuTarget!.width != width || gpuTarget!.height != height {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: width, height: height, mipmapped: false)
            d.usage = [.renderTarget, .shaderRead]
            d.storageMode = .private
            gpuTarget = gpu.device.makeTexture(descriptor: d)
        }
        guard let target = gpuTarget else { return false }
        var prepared: [EmoteGPU.Prepared] = []
        if visible {
            let pose = p.pose()
            let place = p.placement(transform)
            for (i, renderer) in p.renderers.enumerated() {
                var variables = pose
                p.models[i].applySelectors(&variables)
                let list = renderer.drawList(variables: variables, transform: place)
                if let ready = gpu.prepare(list, model: p.models[i], buffer: commands) { prepared.append(ready) }
            }
        }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pass.colorAttachments[0].storeAction = .store
        guard let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else { return false }
        for ready in prepared { gpu.draw(ready, encoder: encoder, width: width, height: height) }
        encoder.endEncoding()
        finisher.encode(commands, drawn: target, into: out, pitch: pitch / 4, width: width, height: height,
                        tint: SIMD4(Float(tint.r), Float(tint.g), Float(tint.b), Float(tint.a)), blend: blend)
        return true
    }

    /// Draws `p` and copies the covered part into `rgba` (straight alpha); returns that
    /// part's rectangle, or nil when nothing was drawn.
    func draw(_ p: KrkrEmotePlayer, into rgba: UnsafeMutablePointer<UInt8>, width: Int, height: Int, pitch: Int,
              transform: [Double]) -> (Int, Int, Int, Int)? {
        guard p.shown, let gpu, let queue, let finisher, width > 0, height > 0 else {
            if Self.trace { FileHandle.standardError.write("emote host: not drawn (shown \(p.shown), gpu \(self.gpu != nil))\n".data(using: .utf8)!) }
            return nil
        }
        let tint = p.tint
        guard tint.a > 0 else {
            if Self.trace { FileHandle.standardError.write("emote host: not drawn (tint \(tint))\n".data(using: .utf8)!) }
            return nil
        }
        if target == nil || target!.width != width || target!.height != height {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: width, height: height, mipmapped: false)
            d.usage = [.renderTarget, .shaderRead]
            d.storageMode = .private
            target = gpu.device.makeTexture(descriptor: d)
            let o = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: width, height: height, mipmapped: false)
            o.usage = [.shaderWrite]
            o.storageMode = .shared
            finished = gpu.device.makeTexture(descriptor: o)
        }
        guard let target, let finished, let buffer = queue.makeCommandBuffer() else { return nil }
        let started = Self.trace ? Date() : nil
        let pose = p.pose()
        let place = p.placement(transform)
        var prepared: [EmoteGPU.Prepared] = []
        for (i, renderer) in p.renderers.enumerated() {
            var variables = pose
            p.models[i].applySelectors(&variables)
            let list = renderer.drawList(variables: variables, transform: place)
            if let ready = gpu.prepare(list, model: p.models[i], buffer: buffer) { prepared.append(ready) }
        }
        let evaluated = Self.trace ? Date() : nil
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pass.colorAttachments[0].storeAction = .store
        guard let encoder = buffer.makeRenderCommandEncoder(descriptor: pass) else { return nil }
        for ready in prepared { gpu.draw(ready, encoder: encoder, width: width, height: height) }
        encoder.endEncoding()
        finisher.encode(buffer, drawn: target, out: finished,
                        tint: SIMD4(Float(tint.r), Float(tint.g), Float(tint.b), Float(tint.a)))
        buffer.commit()
        buffer.waitUntilCompleted()
        let rendered = Self.trace ? Date() : nil
        guard let r = finisher.covered else {
            if Self.trace {
                traceCount += 1
                if traceCount % 120 == 1 {
                    FileHandle.standardError.write("emote host: nothing covered; items \(prepared.map { $0.itemCount }) place \(place) tint \(tint) timelines \(p.playingLabels) pose \(pose.filter { $0.value != 0 }.count)/\(pose.count) models \(p.models.count) base \(p.models[0].baseObject)/\(p.models[0].baseMotion) has \(p.models[0].objects[p.models[0].baseObject]?[p.models[0].baseMotion] != nil) objects \(p.models[0].objects.keys.sorted().prefix(5))\n".data(using: .utf8)!)
                }
            }
            return nil
        }
        finished.getBytes(rgba + r.1 * pitch + r.0 * 4, bytesPerRow: pitch,
                          from: MTLRegionMake2D(r.0, r.1, r.2 - r.0, r.3 - r.1), mipmapLevel: 0)
        if Self.trace, let started, let evaluated, let rendered {
            traceCount += 1
            if traceCount % 120 == 1 {
                let v = pose.filter { $0.value != 0 }.sorted { $0.key < $1.key }.map { "\($0.key)=\(String(format: "%.2f", $0.value))" }
                let ms = { (a: Date, b: Date) in String(format: "%.1f", b.timeIntervalSince(a) * 1000) }
                FileHandle.standardError.write(("emote host: eval \(ms(started, evaluated)) ms, GPU \(ms(evaluated, rendered)) ms, copy \(ms(rendered, Date())) ms; rect \(r) items \(prepared.map { $0.itemCount }) timelines \(p.playingLabels) \(v)\n").data(using: .utf8)!)
            }
        }
        return r
    }
}
