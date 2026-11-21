#if MODE == 0
layout(set = 1, binding = 0, std140) uniform R { uvec4 tex[4]; uvec4 buf[4]; vec4 c[16]; } r;
#endif
layout(set = 0, binding = 0) uniform texture2D heap[NTEX];
layout(set = 0, binding = 1) uniform sampler smp;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;

void main() {
    vec4 acc = vec4(0.0);
#if MODE == 0
    for (uint i = 0u; i < uint(KT); i++) {
        uint idx = r.tex[i >> 2][i & 3u];
        float wgt = r.c[2u + ((i % uint(CR)) >> 2)][(i % uint(CR)) & 3u];
        acc += texture(sampler2D(heap[nonuniformEXT(idx)], smp), uv * (1.0 + float(i))) * wgt;
    }
    for (uint j = 0u; j < uint(KB); j++) {
        uvec4 b = r.buf[j >> 1];
        uint64_t d = ((j & 1u) == 0u) ? packUint2x32(b.xy) : packUint2x32(b.zw);
        Words w = Words(d);
        acc += vec4(uintBitsToFloat(w.w[0]), uintBitsToFloat(w.w[1]), uintBitsToFloat(w.w[2]), uintBitsToFloat(w.w[3]));
    }
    for (uint c = 0u; c < uint(CR); c++) {
        acc.a += r.c[4u + (c >> 2)][c & 3u];
    }
#elif MODE == 1
    RBlock rb = RBlock(pc.arg);
    for (uint i = 0u; i < uint(KT); i++) {
        uint idx = rb.tex[i >> 2][i & 3u];
        float wgt = rb.c[2u + ((i % uint(CR)) >> 2)][(i % uint(CR)) & 3u];
        acc += texture(sampler2D(heap[nonuniformEXT(idx)], smp), uv * (1.0 + float(i))) * wgt;
    }
    for (uint j = 0u; j < uint(KB); j++) {
        uvec4 b = rb.buf[j >> 1];
        uint64_t d = ((j & 1u) == 0u) ? packUint2x32(b.xy) : packUint2x32(b.zw);
        Words w = Words(d);
        acc += vec4(uintBitsToFloat(w.w[0]), uintBitsToFloat(w.w[1]), uintBitsToFloat(w.w[2]), uintBitsToFloat(w.w[3]));
    }
    for (uint c = 0u; c < uint(CR); c++) {
        acc.a += rb.c[4u + (c >> 2)][c & 3u];
    }
#else
    uint64_t srt = xlate(pc.arg);
    uint64_t tt = xlate(ld64(srt, 0u));
    uint64_t bt = xlate(ld64(srt, 2u));
    Words cw = Words(xlate(ld64(srt, 4u)));
    for (uint i = 0u; i < uint(KT); i++) {
        uint idx = lookup(ld64(tt, i * 8u), ld64(tt, i * 8u + 2u));
        float wgt = uintBitsToFloat(cw.w[8u + (i % uint(CR))]);
        acc += texture(sampler2D(heap[nonuniformEXT(idx)], smp), uv * (1.0 + float(i))) * wgt;
    }
    for (uint j = 0u; j < uint(KB); j++) {
        Words w = Words(xlate(ld64(bt, j * 4u)));
        acc += vec4(uintBitsToFloat(w.w[0]), uintBitsToFloat(w.w[1]), uintBitsToFloat(w.w[2]), uintBitsToFloat(w.w[3]));
    }
    for (uint c = 0u; c < uint(CR); c++) {
        acc.a += uintBitsToFloat(cw.w[16u + c]);
    }
#endif
    o = acc;
}
