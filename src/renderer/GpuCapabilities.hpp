#pragma once

#include "source/FrameData.hpp"

namespace quadmapper {

/**
 * @brief Runtime GL texture-compression capability gate (ADR-044).
 *
 * GL 3.3 core is QUADmapper's floor; the BCn formats HAP needs are extensions
 * (s3tc for BC1/BC3, bptc for BC7). We query them once at GL init and gate the
 * compressed-upload path on them. When a clip needs an unavailable format the
 * upload is skipped (texture stays black) and logged once — never a crash
 * (ADR-044 §4 + the "graceful skip" deviation note).
 */
struct GpuCapabilities {
    bool s3tc = false;  // GL_EXT_texture_compression_s3tc  -> BC1 / BC3
    bool bptc = false;  // GL_ARB_texture_compression_bptc  -> BC7

    /// Process-wide singleton (populated by queryOnce()).
    static GpuCapabilities& get();

    /// Query the current GL context's extensions once and log the result.
    /// Safe to call more than once; only the first call queries GL.
    void queryOnce();

    /// True if the running GL context can sample the given compressed format.
    bool supports(GpuCompressedFormat fmt) const;

private:
    bool queried_ = false;
};

} // namespace quadmapper
