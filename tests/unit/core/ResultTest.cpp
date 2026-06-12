#include <gtest/gtest.h>
#include "core/Result.hpp"

namespace quadmapper {
namespace test {

TEST(ResultTest, SuccessResultContainsValue) {
    Result<int, std::string> result = Result<int, std::string>::success(42);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_FALSE(result.isError());
    EXPECT_EQ(result.value(), 42);
}

TEST(ResultTest, ErrorResultContainsError) {
    Result<int, std::string> result = Result<int, std::string>::error("failed");

    EXPECT_FALSE(result.isSuccess());
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error(), "failed");
}

TEST(ResultTest, ValueOrReturnsValueOnSuccess) {
    Result<int, std::string> result = Result<int, std::string>::success(42);

    EXPECT_EQ(result.valueOr(0), 42);
}

TEST(ResultTest, ValueOrReturnsDefaultOnError) {
    Result<int, std::string> result = Result<int, std::string>::error("failed");

    EXPECT_EQ(result.valueOr(0), 0);
}

TEST(ResultTest, MapTransformsValue) {
    Result<int, std::string> result = Result<int, std::string>::success(21);

    auto mapped = result.map([](int v) { return v * 2; });

    EXPECT_TRUE(mapped.isSuccess());
    EXPECT_EQ(mapped.value(), 42);
}

TEST(ResultTest, MapPreservesError) {
    Result<int, std::string> result = Result<int, std::string>::error("failed");

    auto mapped = result.map([](int v) { return v * 2; });

    EXPECT_TRUE(mapped.isError());
    EXPECT_EQ(mapped.error(), "failed");
}

TEST(ResultTest, AndThenChainsSuccesses) {
    auto divide = [](int v) -> Result<int, std::string> {
        if (v == 0) return Result<int, std::string>::error("division by zero");
        return Result<int, std::string>::success(100 / v);
    };

    Result<int, std::string> result = Result<int, std::string>::success(10);
    auto chained = result.andThen(divide);

    EXPECT_TRUE(chained.isSuccess());
    EXPECT_EQ(chained.value(), 10);
}

TEST(ResultTest, AndThenStopsOnError) {
    auto divide = [](int v) -> Result<int, std::string> {
        if (v == 0) return Result<int, std::string>::error("division by zero");
        return Result<int, std::string>::success(100 / v);
    };

    Result<int, std::string> result = Result<int, std::string>::success(0);
    auto chained = result.andThen(divide);

    EXPECT_TRUE(chained.isError());
    EXPECT_EQ(chained.error(), "division by zero");
}

} // namespace test
} // namespace quadmapper
