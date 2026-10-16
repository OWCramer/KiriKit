// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.

import Metal

/// Turns a drawn character (premultiplied, as CatSystemKit's renderer leaves it) into what
/// KiriKiri's layers take, on the GPU: straight alpha with the game's colour multiplied in,
/// and the rectangle that holds any pixels, so only that much is read back and composed. Or,
/// when the engine composes on the GPU too, straight into the layer's own buffer.
final class EmoteFinisher {
    private let pipeline: MTLComputePipelineState
    private let intoBuffer: MTLComputePipelineState
    private let bounds: MTLBuffer

    static let source = """
    #include <metal_stdlib>
    using namespace metal;

    kernel void finish(texture2d<float, access::read> drawn [[texture(0)]],
                       texture2d<float, access::write> out [[texture(1)]],
                       constant float4 &tint [[buffer(0)]],
                       device atomic_uint *bounds [[buffer(1)]],
                       uint2 p [[thread_position_in_grid]]) {
        if (p.x >= drawn.get_width() || p.y >= drawn.get_height()) return;
        float4 c = drawn.read(p);
        if (c.a <= 0.0) { out.write(float4(0.0), p); return; }
        out.write(float4(min(c.rgb / c.a, 1.0) * tint.rgb, c.a * tint.a), p);
        atomic_fetch_min_explicit(&bounds[0], p.x, memory_order_relaxed);
        atomic_fetch_min_explicit(&bounds[1], p.y, memory_order_relaxed);
        atomic_fetch_max_explicit(&bounds[2], p.x + 1, memory_order_relaxed);
        atomic_fetch_max_explicit(&bounds[3], p.y + 1, memory_order_relaxed);
    }

    struct Into { uint pitch; uint width; uint height; uint blend; float4 tint; };

    // Into a layer's buffer (R, G, B, A bytes, straight alpha): in place of its pixels, or over
    // them.
    kernel void finishInto(texture2d<float, access::read> drawn [[texture(0)]],
                           device uchar4 *out [[buffer(0)]],
                           constant Into &into [[buffer(1)]],
                           uint2 p [[thread_position_in_grid]]) {
        if (p.x >= into.width || p.y >= into.height) return;
        float4 c = drawn.read(p);
        float4 s = c.a > 0.0 ? float4(min(c.rgb / c.a, 1.0) * into.tint.rgb, c.a * into.tint.a) : float4(0.0);
        device uchar4 &d = out[p.y * into.pitch + p.x];
        if (into.blend != 0) {
            if (s.a <= 0.0) return;
            float4 dst = float4(d) / 255.0;
            float da = dst.a * (1.0 - s.a), oa = s.a + da;
            s = float4((s.rgb * s.a + dst.rgb * da) / oa, oa);
        }
        d = uchar4(round(clamp(s, 0.0, 1.0) * 255.0));
    }
    """

    init?(device: MTLDevice) {
        guard let library = try? device.makeLibrary(source: Self.source, options: nil),
              let function = library.makeFunction(name: "finish"),
              let pipeline = try? device.makeComputePipelineState(function: function),
              let intoFunction = library.makeFunction(name: "finishInto"),
              let intoBuffer = try? device.makeComputePipelineState(function: intoFunction),
              let bounds = device.makeBuffer(length: 16, options: .storageModeShared) else { return nil }
        self.pipeline = pipeline
        self.intoBuffer = intoBuffer
        self.bounds = bounds
    }

    /// Encodes the pass from `drawn` into `out` (same size).
    func encode(_ buffer: MTLCommandBuffer, drawn: MTLTexture, out: MTLTexture, tint: SIMD4<Float>) {
        let b = bounds.contents().assumingMemoryBound(to: UInt32.self)
        b[0] = .max; b[1] = .max; b[2] = 0; b[3] = 0
        guard let encoder = buffer.makeComputeCommandEncoder() else { return }
        var t = tint
        encoder.setComputePipelineState(pipeline)
        encoder.setTexture(drawn, index: 0)
        encoder.setTexture(out, index: 1)
        encoder.setBytes(&t, length: 16, index: 0)
        encoder.setBuffer(bounds, offset: 0, index: 1)
        let w = pipeline.threadExecutionWidth, h = max(1, pipeline.maxTotalThreadsPerThreadgroup / w)
        encoder.dispatchThreadgroups(MTLSize(width: (drawn.width + w - 1) / w, height: (drawn.height + h - 1) / h, depth: 1),
                                     threadsPerThreadgroup: MTLSize(width: w, height: h, depth: 1))
        encoder.endEncoding()
    }

    /// Encodes the pass from `drawn` into a layer's buffer (width x height pixels, rows of
    /// `pitch` pixels): replacing them, or over them when `blend`.
    func encode(_ buffer: MTLCommandBuffer, drawn: MTLTexture, into out: MTLBuffer, pitch: Int, width: Int, height: Int,
                tint: SIMD4<Float>, blend: Bool) {
        struct Into { var pitch: UInt32; var width: UInt32; var height: UInt32; var blend: UInt32; var tint: SIMD4<Float> }
        var into = Into(pitch: UInt32(pitch), width: UInt32(width), height: UInt32(height), blend: blend ? 1 : 0, tint: tint)
        guard let encoder = buffer.makeComputeCommandEncoder() else { return }
        encoder.setComputePipelineState(intoBuffer)
        encoder.setTexture(drawn, index: 0)
        encoder.setBuffer(out, offset: 0, index: 0)
        encoder.setBytes(&into, length: MemoryLayout<Into>.stride, index: 1)
        let w = intoBuffer.threadExecutionWidth, h = max(1, intoBuffer.maxTotalThreadsPerThreadgroup / w)
        encoder.dispatchThreadgroups(MTLSize(width: (width + w - 1) / w, height: (height + h - 1) / h, depth: 1),
                                     threadsPerThreadgroup: MTLSize(width: w, height: h, depth: 1))
        encoder.endEncoding()
    }

    /// The covered rectangle after the pass has run (left, top, right, bottom), or nil.
    var covered: (Int, Int, Int, Int)? {
        let b = bounds.contents().assumingMemoryBound(to: UInt32.self)
        guard b[2] > b[0], b[3] > b[1] else { return nil }
        return (Int(b[0]), Int(b[1]), Int(b[2]), Int(b[3]))
    }
}
