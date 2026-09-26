#include "OscPattern.hpp"

#include <algorithm>
#include <cctype>

namespace quadmapper::control {

namespace {

unsigned char lower(char c) {
    return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)));
}

bool isSpecial(char c) {
    return c == '?' || c == '*' || c == '[' || c == ']' || c == '{' || c == '}';
}

// Split "/a/b/c" into its parts after the leading '/'. Empty when there is no '/'.
std::vector<std::string_view> splitParts(std::string_view path) {
    std::vector<std::string_view> parts;
    if (path.empty() || path[0] != '/') return parts;
    size_t start = 1;
    while (true) {
        const size_t slash = path.find('/', start);
        if (slash == std::string_view::npos) {
            parts.push_back(path.substr(start));
            break;
        }
        parts.push_back(path.substr(start, slash - start));
        start = slash + 1;
    }
    return parts;
}

}  // namespace

bool OscPattern::hasWildcards(std::string_view address) {
    return std::any_of(address.begin(), address.end(), isSpecial);
}

std::optional<OscPattern::Part> OscPattern::compilePart(std::string_view text) {
    Part part;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        Token token;
        if (c == '?') {
            token.kind = Token::Kind::AnyOne;
            ++i;
        } else if (c == '*') {
            ++i;
            if (!part.empty() && part.back().kind == Token::Kind::AnyRun) continue;  // ** = *
            token.kind = Token::Kind::AnyRun;
        } else if (c == '[') {
            size_t j = i + 1;
            bool negate = false;
            if (j < text.size() && text[j] == '!') {
                negate = true;
                ++j;
            }
            const size_t close = text.find(']', j);
            if (close == std::string_view::npos || close == j) return std::nullopt;  // unclosed / empty
            const std::string_view members = text.substr(j, close - j);
            for (size_t k = 0; k < members.size(); ++k) {
                const char m = members[k];
                if (m == '[' || m == '{' || m == '}') return std::nullopt;
                if (k + 2 < members.size() && members[k + 1] == '-') {
                    unsigned char from = static_cast<unsigned char>(m);
                    unsigned char to = static_cast<unsigned char>(members[k + 2]);
                    if (from > to) std::swap(from, to);
                    for (unsigned v = from; v <= to; ++v) token.set.set(lower(static_cast<char>(v)));
                    k += 2;
                } else {
                    token.set.set(lower(m));
                }
            }
            if (negate) token.set.flip();
            token.kind = Token::Kind::Set;
            i = close + 1;
        } else if (c == '{') {
            const size_t close = text.find('}', i + 1);
            if (close == std::string_view::npos) return std::nullopt;
            const std::string_view list = text.substr(i + 1, close - i - 1);
            std::string choice;
            for (char m : list) {
                if (isSpecial(m)) return std::nullopt;  // nested { or a wildcard inside {}
                if (m == ',') {
                    token.choices.push_back(choice);
                    choice.clear();
                } else {
                    choice.push_back(static_cast<char>(lower(m)));
                }
            }
            token.choices.push_back(choice);
            std::sort(token.choices.begin(), token.choices.end());
            token.choices.erase(std::unique(token.choices.begin(), token.choices.end()),
                                token.choices.end());
            token.kind = Token::Kind::Choice;
            i = close + 1;
        } else if (c == ']' || c == '}') {
            return std::nullopt;  // stray closer
        } else {
            token.kind = Token::Kind::Literal;
            token.literal = static_cast<char>(lower(c));
            ++i;
        }
        part.push_back(std::move(token));
    }
    return part;
}

std::optional<OscPattern> OscPattern::compile(std::string_view pattern) {
    if (pattern.size() > kMaxLength || pattern.empty() || pattern[0] != '/') return std::nullopt;
    OscPattern compiled;
    for (std::string_view text : splitParts(pattern)) {
        auto part = compilePart(text);
        if (!part) return std::nullopt;
        compiled.parts_.push_back(std::move(*part));
    }
    return compiled;
}

bool OscPattern::matchPart(size_t index, std::string_view text) const {
    if (index >= parts_.size()) return false;
    const Part& part = parts_[index];
    const size_t n = text.size();

    // reach[j]: the tokens so far can consume exactly text[0, j).
    std::vector<char> reach(n + 1, 0);
    std::vector<char> next(n + 1, 0);
    reach[0] = 1;
    for (const Token& token : part) {
        std::fill(next.begin(), next.end(), 0);
        bool any = false;
        switch (token.kind) {
            case Token::Kind::Literal:
                for (size_t j = 0; j < n; ++j) {
                    if (reach[j] && lower(text[j]) == static_cast<unsigned char>(token.literal)) next[j + 1] = 1;
                }
                break;
            case Token::Kind::AnyOne:
                for (size_t j = 0; j < n; ++j) {
                    if (reach[j]) next[j + 1] = 1;
                }
                break;
            case Token::Kind::Set:
                for (size_t j = 0; j < n; ++j) {
                    if (reach[j] && token.set.test(lower(text[j]))) next[j + 1] = 1;
                }
                break;
            case Token::Kind::AnyRun: {
                bool on = false;  // every position at or after the first reachable one
                for (size_t j = 0; j <= n; ++j) {
                    on = on || reach[j];
                    next[j] = on ? 1 : 0;
                }
                break;
            }
            case Token::Kind::Choice:
                for (size_t j = 0; j <= n; ++j) {
                    if (!reach[j]) continue;
                    for (const std::string& choice : token.choices) {
                        if (choice.size() > n - j) continue;
                        bool equal = true;
                        for (size_t k = 0; k < choice.size() && equal; ++k) {
                            equal = lower(text[j + k]) == static_cast<unsigned char>(choice[k]);
                        }
                        if (equal) next[j + choice.size()] = 1;
                    }
                }
                break;
        }
        for (char v : next) any = any || v;
        if (!any) return false;
        reach.swap(next);
    }
    return reach[n] != 0;
}

bool OscPattern::matches(std::string_view address) const {
    const std::vector<std::string_view> parts = splitParts(address);
    if (parts.size() != parts_.size()) return false;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!matchPart(i, parts[i])) return false;
    }
    return true;
}

}  // namespace quadmapper::control
