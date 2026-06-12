#pragma once
#include <string>
namespace quadmapper { class Panel { public: Panel(const std::string& title) : title_(title) {} virtual ~Panel() = default; virtual void render() = 0; const std::string& getTitle() const { return title_; } bool isVisible() const { return visible_; } void setVisible(bool v) { visible_ = v; } protected: std::string title_; bool visible_ = true; }; }
