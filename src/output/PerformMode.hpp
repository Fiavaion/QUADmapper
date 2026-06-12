#pragma once
namespace quadmapper { class PerformMode { public: PerformMode() = default; void enter(); void exit(); bool isActive() const { return active_; } private: bool active_ = false; }; }
