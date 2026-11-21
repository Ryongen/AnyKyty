#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define GUEST_BASE 0x2000000000ul
#define HMASK 4095u
#define RWORDS 96u
#define RSTRIDE 512ul

layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer PageTable { uint64_t e[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer Words { uint w[]; };
struct Slot { uint64_t k0; uint64_t k1; uint idx; uint used; uint p0; uint p1; };
layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer HashTab { Slot s[]; };
layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer DrawList { uint64_t srt[]; };
layout(buffer_reference, std140, buffer_reference_align = 16) readonly buffer RBlock { uvec4 tex[4]; uvec4 buf[4]; vec4 c[16]; };

layout(push_constant, scalar) uniform PC { uint64_t pt; uint64_t hash; uint64_t mirror; uint64_t outp; uint64_t arg; uint a0; uint a1; } pc;

uint64_t xlate(uint64_t a) {
    uint64_t o = a - GUEST_BASE;
#if PT
    return PageTable(pc.pt).e[uint(o >> 12)] + (o & 0xffful);
#else
    return pc.mirror + o;
#endif
}

uint64_t ld64(uint64_t dev, uint i) {
    Words w = Words(dev);
    return packUint2x32(uvec2(w.w[i], w.w[i + 1u]));
}

uint lookup(uint64_t k0, uint64_t k1) {
    uint h = uint(k0) ^ (uint(k0 >> 32) * 0x9E3779B1u) ^ (uint(k1) * 0x85EBCA6Bu);
    h ^= h >> 15;
    HashTab t = HashTab(pc.hash);
    for (uint p = 0u; p < 8u; p++) {
        uint s = (h + p) & HMASK;
        if (t.s[s].k0 == k0 && t.s[s].k1 == k1) {
            return t.s[s].idx;
        }
    }
    return 0u;
}

uint sel(uint d, uint i, uint seed) {
    return (((d * 2654435761u) + (i * 40503u) + seed) >> 7) % uint(NTEX);
}
