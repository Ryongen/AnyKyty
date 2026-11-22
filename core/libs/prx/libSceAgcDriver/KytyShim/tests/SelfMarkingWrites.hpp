#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_KYTYSHIM_TESTS_SELFMARKINGWRITES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_KYTYSHIM_TESTS_SELFMARKINGWRITES_HPP

#include "SceTypes.hpp"

#include <cstdint>

struct KytySelfMarkingAgc {
    std::uint32_t* (APS5_VABI* nop)(CommandBuffer* buffer, std::uint32_t dwords);
    std::uint32_t* (APS5_VABI* rewind)(CommandBuffer* buffer, std::uint32_t state);
    int (APS5_VABI* patch_rewind)(std::uint32_t* packet, std::uint8_t state);
};

extern "C" int RunKytySelfMarkingWritesTest(const KytySelfMarkingAgc* agc);

#endif
