#include "control/ControlDispatcher.hpp"
#include "control/ControlRegistry.hpp"
#include "control/OscPattern.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <map>

using namespace quadmapper::control;

namespace {

bool match(const std::string& pattern, const std::string& address) {
    const auto compiled = OscPattern::compile(pattern);
    return compiled && compiled->matches(address);
}

struct Case {
    const char* pattern;
    const char* address;
    bool expected;
};

}  // namespace

// ---- Matching table (OSC 1.0 wildcards) -------------------------------------------

TEST(OscPatternTest, MatchesTheWildcardTable) {
    const Case cases[] = {
        // literal (case-insensitive, like the registry)
        {"/qm/blackout", "/qm/blackout", true},
        {"/QM/Blackout", "/qm/blackout", true},
        {"/qm/blackout", "/qm/blackou", false},
        {"/qm/blackout", "/qm/blackoutx", false},
        // ?
        {"/qm/quad/?/opacity", "/qm/quad/A/opacity", true},
        {"/qm/quad/?/opacity", "/qm/quad/AB/opacity", false},
        {"/qm/quad/?/opacity", "/qm/quad//opacity", false},
        {"/a??c", "/abbc", true},
        // *
        {"/qm/quad/*/opacity", "/qm/quad/Left wall/opacity", true},
        {"/qm/quad/*/opacity", "/qm/quad//opacity", true},  // * matches nothing too
        {"/qm/*", "/qm/quad/x/opacity", false},             // never crosses '/'
        {"/qm/*/*/*", "/qm/quad/x/opacity", true},
        {"/*", "/qm", true},
        {"/a*b*c", "/aXXbYYc", true},
        {"/a*b*c", "/aXXcYYb", false},
        {"/**", "/anything", true},
        {"/*x", "/x", true},
        {"/x*", "/x", true},
        // [abc] [a-z] [!...]
        {"/qm/quad/[LR]*/opacity", "/qm/quad/left/opacity", true},
        {"/qm/quad/[LR]*/opacity", "/qm/quad/Right/opacity", true},
        {"/qm/quad/[LR]*/opacity", "/qm/quad/Top/opacity", false},
        {"/ch[0-9]", "/ch7", true},
        {"/ch[0-9]", "/chx", false},
        {"/ch[9-0]", "/ch5", true},  // reversed range accepted
        {"/ch[!0-9]", "/chx", true},
        {"/ch[!0-9]", "/ch5", false},
        {"/x[-a]", "/x-", true},     // '-' first is literal
        {"/x[a-]", "/x-", true},     // '-' last is literal
        {"/x[a-]", "/xb", false},
        {"/x[a!]", "/x!", true},     // '!' not first is literal
        {"/x[A-C]", "/xb", true},    // case-insensitive set
        // {foo,bar}
        {"/qm/master/{volume,mute}", "/qm/master/volume", true},
        {"/qm/master/{volume,mute}", "/qm/master/Mute", true},
        {"/qm/master/{volume,mute}", "/qm/master/muted", false},
        {"/x{,y}z", "/xz", true},     // an empty alternative
        {"/x{,y}z", "/xyz", true},
        {"/{a,ab}{c,bc}", "/abc", true},  // needs both ways of splitting
        // combinations
        {"/qm/{quad,source}/*/[ov]*", "/qm/source/clip/volume", true},
        {"/qm/{quad,source}/*/[ov]*", "/qm/scene/clip/volume", false},
        // part counts must agree
        {"/qm/quad/*", "/qm/quad/a/opacity", false},
        {"/qm/quad/*/opacity/*", "/qm/quad/a/opacity", false},
        {"/qm/", "/qm/", true},
        {"/qm/", "/qm", false},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(match(c.pattern, c.address), c.expected) << c.pattern << "  vs  " << c.address;
    }
}

TEST(OscPatternTest, MalformedPatternsDoNotCompile) {
    for (const char* bad : {"", "qm/x", "/x[", "/x[]", "/x[!]", "/x[a", "/x{a,b", "/x{a{b}}",
                            "/x{a*}", "/x{a[b]}", "/x]", "/x}", "/x[a[b]"}) {
        EXPECT_FALSE(OscPattern::compile(bad).has_value()) << bad;
    }
    EXPECT_FALSE(OscPattern::compile("/" + std::string(OscPattern::kMaxLength, '*')).has_value());
    EXPECT_TRUE(OscPattern::compile("/" + std::string(OscPattern::kMaxLength - 1, '*')).has_value());
}

TEST(OscPatternTest, DetectsWildcards) {
    EXPECT_FALSE(OscPattern::hasWildcards("/qm/quad/Left/opacity"));
    for (const char* p : {"/a?", "/a*", "/[a]", "/{a}", "/a]", "/a}"}) {
        EXPECT_TRUE(OscPattern::hasWildcards(p)) << p;
    }
}

// Patterns that make a backtracking matcher take exponential time finish at once here.
TEST(OscPatternTest, PathologicalPatternsFinishFast) {
    const std::string longText(4000, 'a');
    std::string stars = "/";
    for (int k = 0; k < 120; ++k) stars += "*a";
    stars += "b";  // never matches: forces every split to be tried by a backtracker
    ASSERT_LE(stars.size(), OscPattern::kMaxLength);
    const auto pattern = OscPattern::compile(stars);
    ASSERT_TRUE(pattern.has_value());

    std::string braces = "/";
    for (int k = 0; k < 20; ++k) braces += "{a,aa,aaa}";
    braces += "b";
    ASSERT_LE(braces.size(), OscPattern::kMaxLength);
    const auto choice = OscPattern::compile(braces);
    ASSERT_TRUE(choice.has_value());

    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(pattern->matchPart(0, longText));
    EXPECT_FALSE(choice->matchPart(0, longText));
    EXPECT_FALSE(pattern->matches("/" + longText));
    // 1000 addresses of registry size, as one wildcard message against a big show.
    for (int k = 0; k < 1000; ++k) {
        EXPECT_FALSE(pattern->matchPart(0, "11111111-aaaa-bbbb-cccc-000000000001aaaaaaaaaaaaaaa"));
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(ms, 500.0) << "took " << ms << " ms";  // debug build; exponential would never finish

    // And the last "b" does match when present.
    EXPECT_TRUE(pattern->matchPart(0, std::string(120, 'a') + "b"));
    EXPECT_TRUE(choice->matchPart(0, std::string(20, 'a') + "b"));
    EXPECT_TRUE(choice->matchPart(0, std::string(60, 'a') + "b"));
    EXPECT_FALSE(choice->matchPart(0, std::string(61, 'a') + "b"));
}

// ---- Registry and dispatcher ------------------------------------------------------

namespace {

struct Show {
    ControlRegistry registry;
    ControlDispatcher dispatcher{registry};
    std::vector<MidiMapping> mappings;
    std::map<std::string, float> opacity;
    std::map<std::string, bool> visible;
    std::map<std::string, float> volume;
    float masterVolume = 1.0f;
    bool masterMute = false;
    int toggles = 0;

    Show() {
        registry.addGlobal("/qm/master/volume",
                           {"", TargetKind::Float, 0, 1, true,
                            [this](const std::string&, float v) { masterVolume = v; },
                            [this](const std::string&) { return std::optional<float>(masterVolume); }});
        registry.addGlobal("/qm/master/mute",
                           {"", TargetKind::Bool, 0, 1, true,
                            [this](const std::string&, float v) { masterMute = v >= 0.5f; },
                            [this](const std::string&) { return std::optional<float>(masterMute ? 1.f : 0.f); }});
        registry.addEntityTarget("quad",
                                 {"opacity", TargetKind::Float, 0, 1, true,
                                  [this](const std::string& id, float v) { opacity[id] = v; }, nullptr});
        registry.addEntityTarget("quad",
                                 {"visible", TargetKind::Bool, 0, 1, true,
                                  [this](const std::string& id, float v) {
                                      visible[id] = v >= 0.5f;
                                      ++toggles;
                                  },
                                  [this](const std::string& id) { return std::optional<float>(visible[id] ? 1.f : 0.f); }});
        registry.addEntityTarget("source",
                                 {"volume", TargetKind::Float, 0, 1, true,
                                  [this](const std::string& id, float v) { volume[id] = v; }, nullptr});
        registry.setEntities("quad", {{"q1", "Left"}, {"q2", "Right"}, {"q3", "Top"}});
        registry.setEntities("source", {{"s1", "clip"}});
    }

    void osc(const std::string& address, std::vector<OscArg> args = {}) {
        ControlEvent e;
        e.source = ControlEvent::Source::Osc;
        e.osc.address = address;
        e.osc.args = std::move(args);
        dispatcher.dispatch(e, mappings, 1.0);
    }
};

OscArg fa(float v) { OscArg a; a.type = OscArg::Type::Float; a.f = v; return a; }

}  // namespace

TEST(OscPatternTest, RegistryExpandsPatternsInListOrder) {
    Show show;
    auto paths = [&](const std::string& address) {
        std::vector<std::string> out;
        for (const auto& r : show.registry.resolveAll(address)) out.push_back(r.canonicalPath);
        return out;
    };
    EXPECT_EQ(paths("/qm/quad/*/opacity"),
              (std::vector<std::string>{"/qm/quad/q1/opacity", "/qm/quad/q2/opacity", "/qm/quad/q3/opacity"}));
    EXPECT_EQ(paths("/qm/quad/[lr]*/opacity"),
              (std::vector<std::string>{"/qm/quad/q1/opacity", "/qm/quad/q2/opacity"}));
    EXPECT_EQ(paths("/qm/quad/q?/visible"),  // by id
              (std::vector<std::string>{"/qm/quad/q1/visible", "/qm/quad/q2/visible", "/qm/quad/q3/visible"}));
    EXPECT_EQ(paths("/qm/quad/Top/*"),  // entity literal, action wildcard
              (std::vector<std::string>{"/qm/quad/q3/opacity", "/qm/quad/q3/visible"}));
    EXPECT_EQ(paths("/qm/master/*"), (std::vector<std::string>{"/qm/master/volume", "/qm/master/mute"}));
    EXPECT_EQ(paths("/qm/*/*/volume"), (std::vector<std::string>{"/qm/source/s1/volume"}));
    EXPECT_EQ(paths("/qm/*/*"), (std::vector<std::string>{"/qm/master/volume", "/qm/master/mute"}));
    // An entity matched by both its id and its name is listed once.
    EXPECT_EQ(paths("/qm/quad/{q1,Left}/opacity"), (std::vector<std::string>{"/qm/quad/q1/opacity"}));
    // Literal addresses are unchanged.
    EXPECT_EQ(paths("/qm/quad/Right/opacity"), (std::vector<std::string>{"/qm/quad/q2/opacity"}));
    EXPECT_TRUE(paths("/qm/quad/*/nothing").empty());
    EXPECT_TRUE(paths("/qm/quad/[/opacity").empty());  // malformed
    EXPECT_TRUE(paths("/zz/*").empty());
}

TEST(OscPatternTest, DispatcherAppliesAPatternToEveryMatch) {
    Show show;
    show.osc("/qm/quad/*/opacity", {fa(0.25f)});
    EXPECT_EQ(show.opacity["q1"], 0.25f);
    EXPECT_EQ(show.opacity["q2"], 0.25f);
    EXPECT_EQ(show.opacity["q3"], 0.25f);
    EXPECT_EQ(show.dispatcher.unknownAddresses(), 0u);
    EXPECT_EQ(show.dispatcher.badArguments(), 0u);
    EXPECT_GE(show.dispatcher.lastActivity("/qm/quad/q2/opacity"), 0.0);

    // No argument toggles each Bool target on its own.
    show.visible["q1"] = true;
    show.osc("/qm/quad/*/visible");
    EXPECT_FALSE(show.visible["q1"]);
    EXPECT_TRUE(show.visible["q2"]);
    EXPECT_TRUE(show.visible["q3"]);
    EXPECT_EQ(show.toggles, 3);
}

TEST(OscPatternTest, APatternWithNoMatchCountsAsUnknown) {
    Show show;
    show.osc("/qm/quad/*/nothing", {fa(1.0f)});
    show.osc("/qm/quad/[/opacity", {fa(1.0f)});  // malformed = no match
    EXPECT_EQ(show.dispatcher.unknownAddresses(), 2u);
    ASSERT_FALSE(show.dispatcher.recentUnknown().empty());
    EXPECT_EQ(show.dispatcher.recentUnknown().front(), "/qm/quad/[/opacity");
    EXPECT_TRUE(show.opacity.empty());
}

TEST(OscPatternTest, BadArgumentsCountOnceAndAcceptingTargetsStillAct) {
    Show show;
    // No argument: the Float targets reject it (one bad-argument count), the Bool acts.
    show.osc("/qm/quad/Top/*");
    EXPECT_EQ(show.dispatcher.badArguments(), 1u);
    EXPECT_TRUE(show.visible["q3"]);
    EXPECT_TRUE(show.opacity.empty());
    // A string argument is bad for all.
    OscArg text;
    text.type = OscArg::Type::String;
    text.s = "x";
    show.osc("/qm/quad/*/opacity", {text});
    EXPECT_EQ(show.dispatcher.badArguments(), 2u);
    EXPECT_TRUE(show.opacity.empty());
}

TEST(OscPatternTest, SeveralExclusiveTargetsAreRefusedOneIsApplied) {
    Show show;
    std::vector<std::string> taken;
    ControlTarget take{"take", TargetKind::Trigger, 0, 1, true,
                       [&](const std::string& id, float) { taken.push_back(id); }, nullptr, true};
    show.registry.addEntityTarget("scene", take);
    show.registry.setEntities("scene", {{"c1", "Intro"}, {"c2", "Outro"}});

    show.osc("/qm/scene/*/take");
    EXPECT_TRUE(taken.empty());
    EXPECT_EQ(show.dispatcher.refusals(), 1u);
    EXPECT_EQ(show.dispatcher.lastRefused(), "/qm/scene/*/take: matches 2 scene takes; address a single scene");
    EXPECT_EQ(show.dispatcher.unknownAddresses(), 0u);
    EXPECT_EQ(show.dispatcher.badArguments(), 0u);

    show.osc("/qm/scene/I*/take");
    show.osc("/qm/scene/Outro/take");
    EXPECT_EQ(taken, (std::vector<std::string>{"c1", "c2"}));
    EXPECT_EQ(show.dispatcher.refusals(), 1u);
}

TEST(OscPatternTest, OscAppliedHookReportsEachTargetWithTheSentValue) {
    Show show;
    std::vector<std::pair<std::string, float>> seen;
    uint32_t seenIp = 0;
    show.dispatcher.setOscAppliedHook(
        [&](const ControlRegistry::Resolved& r, float v, uint32_t ip, uint16_t) {
            seen.emplace_back(r.canonicalPath, v);
            seenIp = ip;
        });
    ControlEvent e;
    e.source = ControlEvent::Source::Osc;
    e.osc.address = "/qm/quad/[lr]*/opacity";
    e.osc.args = {fa(2.0f)};  // clamped to 1 by the target; the hook gets what was sent
    e.oscSenderIp = 0x7F000001;
    show.dispatcher.dispatch(e, show.mappings, 1.0);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], (std::pair<std::string, float>{"/qm/quad/q1/opacity", 2.0f}));
    EXPECT_EQ(seen[1], (std::pair<std::string, float>{"/qm/quad/q2/opacity", 2.0f}));
    EXPECT_EQ(seenIp, 0x7F000001u);
    EXPECT_EQ(show.opacity["q1"], 1.0f);

    // A message with no value (a toggle) does not call it: the sender does not know
    // the result.
    seen.clear();
    show.osc("/qm/quad/Left/visible");
    EXPECT_TRUE(seen.empty());
}
