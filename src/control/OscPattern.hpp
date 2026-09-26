#pragma once

#include <bitset>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quadmapper::control {

// An OSC 1.0 address pattern, compiled once and matched against many addresses.
//   ?        any one character
//   *        any run of characters, including none
//   [abc]    one of the listed characters; a-z is a range, a leading ! negates, a
//            '-' first or last is literal
//   {foo,bar} one of the listed literal strings
// Wildcards never cross '/': the pattern and the address are compared part by part
// and must have the same number of parts. Characters compare without case, as the
// registry's literal paths do.
//
// Matching is a set-of-positions simulation (no backtracking): a part of m tokens
// against a text of n characters costs O(m * n) plus O(n * total brace length), so a
// hostile pattern cannot blow up. Patterns longer than kMaxLength, and malformed
// ones (an unclosed [ or {, an empty [], a nested {, a wildcard inside {}, a stray
// ] or }), do not compile and so match nothing.
class OscPattern {
public:
    static constexpr size_t kMaxLength = 256;

    static std::optional<OscPattern> compile(std::string_view pattern);

    // True when `address` holds any of ? * [ ] { } (so it is a pattern to expand).
    static bool hasWildcards(std::string_view address);

    size_t partCount() const { return parts_.size(); }
    // Does part `index` of the pattern match `text` (one address part, no '/')?
    bool matchPart(size_t index, std::string_view text) const;
    // Does the whole pattern match `address` ("/a/b/c")?
    bool matches(std::string_view address) const;

private:
    struct Token {
        enum class Kind { Literal, AnyOne, AnyRun, Set, Choice };
        Kind kind = Kind::Literal;
        char literal = '\0';                  // Literal (lower-cased)
        std::bitset<256> set;                 // Set: lower-cased members, negation applied
        std::vector<std::string> choices;     // Choice: lower-cased alternatives
    };
    using Part = std::vector<Token>;

    static std::optional<Part> compilePart(std::string_view text);

    std::vector<Part> parts_;
};

}  // namespace quadmapper::control
