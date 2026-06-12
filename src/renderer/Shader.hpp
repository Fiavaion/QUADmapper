#pragma once
#include <string>
namespace quadmapper {
class Shader {
public:
    Shader() = default;
    bool loadFromSource(const std::string& vertexSrc, const std::string& fragmentSrc);
    void use();
};
} // namespace quadmapper
