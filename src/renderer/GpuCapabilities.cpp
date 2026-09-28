#include "GpuCapabilities.hpp"
#include "core/Logger.hpp"

#include <glad/glad.h>

#include <cstring>

namespace quadmapper {

GpuCapabilities& GpuCapabilities::get() {
    static GpuCapabilities instance;
    return instance;
}

void GpuCapabilities::queryOnce() {
    if (queried_) {
        return;
    }
    queried_ = true;

    // GL 3.3 core has no glGetString(GL_EXTENSIONS); enumerate via the indexed
    // query instead.
    GLint numExtensions = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);

    for (GLint i = 0; i < numExtensions; ++i) {
        const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (!ext) {
            continue;
        }
        if (std::strcmp(ext, "GL_EXT_texture_compression_s3tc") == 0) {
            s3tc = true;
        } else if (std::strcmp(ext, "GL_ARB_texture_compression_bptc") == 0) {
            bptc = true;
        }
    }

    Logger::info("GPU texture compression: s3tc(BC1/BC3)={}  bptc(BC7)={}",
                 s3tc ? "yes" : "no", bptc ? "yes" : "no");
}

bool GpuCapabilities::supports(GpuCompressedFormat fmt) const {
    switch (fmt) {
        case GpuCompressedFormat::BC1_RGB:
        case GpuCompressedFormat::BC3_RGBA:
        case GpuCompressedFormat::BC3_YCoCg:
            return s3tc;
        case GpuCompressedFormat::BC7_RGBA:
            return bptc;
        case GpuCompressedFormat::None:
        default:
            return false;
    }
}

} // namespace quadmapper
