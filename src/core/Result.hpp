#pragma once

#include <variant>
#include <string>
#include <optional>
#include <functional>

namespace quadmapper {

// Result type with two template parameters: Value and Error type
template<typename T, typename E>
class Result {
public:
    // Static factory methods
    static Result success(T value) {
        Result r;
        r.data_ = std::move(value);
        return r;
    }

    static Result error(E err) {
        Result r;
        r.data_ = std::move(err);
        return r;
    }

    bool isSuccess() const { return std::holds_alternative<T>(data_); }
    bool isError() const { return std::holds_alternative<E>(data_); }

    // Get value (throws if error)
    T& value() { return std::get<T>(data_); }
    const T& value() const { return std::get<T>(data_); }

    // Get error (throws if success)
    E& error() { return std::get<E>(data_); }
    const E& error() const { return std::get<E>(data_); }

    T valueOr(const T& defaultValue) const {
        if (isSuccess()) {
            return std::get<T>(data_);
        }
        return defaultValue;
    }

    // Map - transform the success value
    template<typename F>
    auto map(F&& f) const -> Result<decltype(f(std::declval<T>())), E> {
        using U = decltype(f(std::declval<T>()));
        if (isSuccess()) {
            return Result<U, E>::success(f(std::get<T>(data_)));
        }
        return Result<U, E>::error(std::get<E>(data_));
    }

    // AndThen - chain operations that return Result
    template<typename F>
    auto andThen(F&& f) const -> decltype(f(std::declval<T>())) {
        using ResultType = decltype(f(std::declval<T>()));
        if (isSuccess()) {
            return f(std::get<T>(data_));
        }
        return ResultType::error(std::get<E>(data_));
    }

    // Bool conversion
    explicit operator bool() const { return isSuccess(); }

private:
    Result() = default;
    std::variant<T, E> data_;
};

// Specialization for void success type
template<typename E>
class Result<void, E> {
public:
    static Result success() {
        Result r;
        r.error_ = std::nullopt;
        return r;
    }

    static Result error(E err) {
        Result r;
        r.error_ = std::move(err);
        return r;
    }

    bool isSuccess() const { return !error_.has_value(); }
    bool isError() const { return error_.has_value(); }

    E& error() { return *error_; }
    const E& error() const { return *error_; }

    explicit operator bool() const { return isSuccess(); }

private:
    Result() = default;
    std::optional<E> error_;
};

// The error type of Result<T, Error>.
struct Error {
    std::string message;
    int code = 0;

    Error() = default;
    explicit Error(std::string msg, int c = 0) : message(std::move(msg)), code(c) {}
};

// Shorthand for building a Result<T, Error>.
template<typename T>
Result<T, Error> Ok(T value) {
    return Result<T, Error>::success(std::move(value));
}

inline Result<void, Error> Ok() {
    return Result<void, Error>::success();
}

template<typename T = void>
Result<T, Error> Err(std::string message, int code = 0) {
    return Result<T, Error>::error(Error(std::move(message), code));
}

} // namespace quadmapper
