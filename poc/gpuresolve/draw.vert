#if MODE == 0
layout(set = 1, binding = 0, std140) uniform R { uvec4 tex[4]; uvec4 buf[4]; vec4 c[16]; } r;
#endif
layout(location = 0) out vec2 uv;

void main() {
    vec4 rect;
#if MODE == 0
    rect = r.c[0];
#elif MODE == 1
    rect = RBlock(pc.arg).c[0];
#else
    uint64_t srt = xlate(pc.arg);
    Words cw = Words(xlate(ld64(srt, 4u)));
    rect = vec4(uintBitsToFloat(cw.w[0]), uintBitsToFloat(cw.w[1]), uintBitsToFloat(cw.w[2]), uintBitsToFloat(cw.w[3]));
#endif
    uint g = pc.a1;
    uint v = uint(gl_VertexIndex);
    uint cell = v / 6u;
    uint corner = v % 6u;
    uvec2 cxy = uvec2(cell % g, cell / g);
    const uvec2 offs[6] = uvec2[6](uvec2(0, 0), uvec2(1, 0), uvec2(0, 1), uvec2(0, 1), uvec2(1, 0), uvec2(1, 1));
    vec2 t = vec2(cxy + offs[corner]) / float(g);
    uv = t;
    gl_Position = vec4(rect.xy + t * rect.zw, 0.0, 1.0);
}
