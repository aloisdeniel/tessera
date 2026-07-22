// MetalPresenter.swift (iOS) — draws an RGBA frame into a CAMetalLayer.
//
// Identical Metal logic to the macOS presenter (Metal is the same on both Apple
// platforms). See macos/.../MetalPresenter.swift for notes.
//
// NOTE: reference implementation for the iOS build; not compiled in this
// environment. The C bridge and shaders ARE verified.
import Metal
import QuartzCore

final class MetalPresenter {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private var pipeline: MTLRenderPipelineState!
    private var sampler: MTLSamplerState!
    private var texture: MTLTexture!
    private var texW = 0
    private var texH = 0

    init?() {
        guard let dev = MTLCreateSystemDefaultDevice(),
              let q = dev.makeCommandQueue() else { return nil }
        device = dev
        queue = q
        guard buildPipeline() else { return nil }
    }

    private func buildPipeline() -> Bool {
        let src = """
        #include <metal_stdlib>
        using namespace metal;
        struct VOut { float4 pos [[position]]; float2 uv; };
        vertex VOut v_main(uint vid [[vertex_id]]) {
            float2 p[3] = { float2(-1, -3), float2(-1, 1), float2(3, 1) };
            float2 xy = p[vid];
            VOut o;
            o.pos = float4(xy, 0, 1);
            o.uv = float2((xy.x + 1) * 0.5, 1.0 - (xy.y + 1) * 0.5);
            return o;
        }
        fragment float4 f_main(VOut in [[stage_in]],
                               texture2d<float> tex [[texture(0)]],
                               sampler s [[sampler(0)]]) {
            return tex.sample(s, in.uv);
        }
        """
        do {
            let lib = try device.makeLibrary(source: src, options: nil)
            let desc = MTLRenderPipelineDescriptor()
            desc.vertexFunction = lib.makeFunction(name: "v_main")
            desc.fragmentFunction = lib.makeFunction(name: "f_main")
            desc.colorAttachments[0].pixelFormat = .bgra8Unorm
            pipeline = try device.makeRenderPipelineState(descriptor: desc)
            let sd = MTLSamplerDescriptor()
            sd.minFilter = .linear
            sd.magFilter = .linear
            sampler = device.makeSamplerState(descriptor: sd)
            return sampler != nil
        } catch {
            NSLog("flutter_tessera: Metal pipeline build failed: \(error)")
            return false
        }
    }

    private func ensureTexture(_ w: Int, _ h: Int) {
        if texture != nil && texW == w && texH == h { return }
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: max(1, w), height: max(1, h),
            mipmapped: false)
        d.usage = [.shaderRead]
        texture = device.makeTexture(descriptor: d)
        texW = w
        texH = h
    }

    func present(rgba: UnsafeRawPointer, width: Int, height: Int, layer: CAMetalLayer) {
        guard width > 0, height > 0 else { return }
        ensureTexture(width, height)
        guard let texture = texture else { return }

        texture.replace(
            region: MTLRegionMake2D(0, 0, width, height),
            mipmapLevel: 0,
            withBytes: rgba,
            bytesPerRow: width * 4)

        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        guard let drawable = layer.nextDrawable(),
              let cmd = queue.makeCommandBuffer() else { return }

        let rp = MTLRenderPassDescriptor()
        rp.colorAttachments[0].texture = drawable.texture
        rp.colorAttachments[0].loadAction = .clear
        rp.colorAttachments[0].clearColor = MTLClearColor(red: 0.03, green: 0.04, blue: 0.06, alpha: 1)
        rp.colorAttachments[0].storeAction = .store

        if let enc = cmd.makeRenderCommandEncoder(descriptor: rp) {
            enc.setRenderPipelineState(pipeline)
            enc.setFragmentTexture(texture, index: 0)
            enc.setFragmentSamplerState(sampler, index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            enc.endEncoding()
        }
        cmd.present(drawable)
        cmd.commit()
    }
}
