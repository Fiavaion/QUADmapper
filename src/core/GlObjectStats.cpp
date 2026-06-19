#include "GlObjectStats.hpp"

namespace quadmapper {

std::atomic<long> GlObjectStats::s_textures{0};
std::atomic<long> GlObjectStats::s_framebuffers{0};
std::atomic<long> GlObjectStats::s_vertexArrays{0};
std::atomic<long> GlObjectStats::s_vertexBuffers{0};
std::atomic<long> GlObjectStats::s_shaderPrograms{0};

} // namespace quadmapper
