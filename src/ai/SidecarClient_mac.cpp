// The AI engine link is not ported to macOS yet (ADR-062: deferred with AI Assist).
// This is the whole macOS half of SidecarClient: the engine always reports "not
// installed", so the app starts no process and opens no connection, and every request
// answers "nothing". SidecarClient_win.cpp is the real client.

#include "SidecarClient.hpp"

#include <chrono>

namespace quadmapper::ai {

std::optional<SegmentResult> parseSegmentResponse(const std::string&, int) {
    return std::nullopt;
}

std::optional<GrayPatternsResult> parseGrayPatternsResponse(const std::string&, int) {
    return std::nullopt;
}

std::optional<GrayCodeResult> parseGrayCodeResponse(const std::string&, int) {
    return std::nullopt;
}

std::optional<GrayMapResult> parseGrayMapResponse(const std::string&, int, size_t) {
    return std::nullopt;
}

std::vector<std::string> parseCapabilities(const std::string&) {
    return {};
}

struct SidecarClient::Impl {
    SidecarConfig config;
    SidecarState state = SidecarState::Stopped;
    SidecarFailure failure = SidecarFailure::None;
    std::chrono::steady_clock::time_point startedAt{};
};

namespace {
// The real client shows Starting while its worker looks for the engine, and the app
// reports "not installed" when it sees that state end. The stand-in shows Starting for
// this long so the app sees the same sequence.
constexpr auto STARTING_SHOWN_FOR = std::chrono::milliseconds(250);
}  // namespace

SidecarClient::SidecarClient(SidecarConfig config) : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
}

SidecarClient::~SidecarClient() = default;

bool SidecarClient::start() {
    impl_->state = SidecarState::Failed;
    impl_->failure = SidecarFailure::NotInstalled;
    return false;
}

void SidecarClient::startAsync() {
    impl_->state = SidecarState::Starting;
    impl_->failure = SidecarFailure::None;
    impl_->startedAt = std::chrono::steady_clock::now();
}

void SidecarClient::stop() {
    impl_->state = SidecarState::Stopped;
    impl_->failure = SidecarFailure::None;
}

bool SidecarClient::isHealthy() const {
    return false;
}

SidecarState SidecarClient::state() const {
    if (impl_->state == SidecarState::Starting &&
        std::chrono::steady_clock::now() - impl_->startedAt >= STARTING_SHOWN_FOR) {
        impl_->state = SidecarState::Failed;
        impl_->failure = SidecarFailure::NotInstalled;
    }
    return impl_->state;
}

SidecarFailure SidecarClient::failure() const {
    state();  // a start that has run its course reports its failure
    return impl_->failure;
}

int SidecarClient::port() const {
    return impl_->config.port;
}

std::vector<std::string> SidecarClient::capabilities() const {
    return {};
}

std::optional<SurfaceResult> SidecarClient::requestSurface(const std::string&) {
    return std::nullopt;
}

std::optional<SurfaceResult> SidecarClient::requestOneFlash(
    const std::string&, const std::array<std::array<float, 2>, 4>*) {
    return std::nullopt;
}

SegmentReply SidecarClient::requestSegment(const std::string&, const std::array<float, 2>&) {
    return {};
}

GrayReply<GrayPatternsResult> SidecarClient::requestGrayPatterns(const std::string&, int, int) {
    return {};
}

GrayReply<GrayCodeResult> SidecarClient::requestGrayCode(const std::string&, int, int) {
    return {};
}

GrayReply<GrayMapResult> SidecarClient::requestGrayMap(const std::string&,
                                                       const std::vector<std::array<float, 2>>&) {
    return {};
}

}  // namespace quadmapper::ai
