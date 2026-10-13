// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.

import Metal

/// Turns a drawn character (premultiplied, as CatSystemKit's renderer leaves it) into what
/// KiriKiri's layers take, on the GPU: straight alpha with the game's colour multiplied in,
/// and the rectangle that holds any pixels, so only that much is read back and composed.
final class EmoteFinisher {
    private let pipeline: MTLComputePipelineState
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
    """

    init?(device: MTLDevice) {
        guard let library = try? device.makeLibrary(source: Self.source, options: nil),
              let function = library.makeFunction(name: "finish"),
              let pipeline = try? device.makeComputePipelineState(function: function),
              let bounds = device.makeBuffer(length: 16, options: .storageModeShared) else { return nil }
        self.pipeline = pipeline
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

    /// The covered rectangle after the pass has run (left, top, right, bottom), or nil.
    var covered: (Int, Int, Int, Int)? {
        let b = bounds.contents().assumingMemoryBound(to: UInt32.self)
        guard b[2] > b[0], b[3] > b[1] else { return nil }
        return (Int(b[0]), Int(b[1]), Int(b[2]), Int(b[3]))
    }
}
