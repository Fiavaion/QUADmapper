#include <gtest/gtest.h>
#include "core/UUID.hpp"

namespace quadmapper {
namespace test {

TEST(UUIDTest, GenerateCreatesUniqueIds) {
    UUID id1 = UUID::generate();
    UUID id2 = UUID::generate();

    EXPECT_NE(id1, id2);
}

TEST(UUIDTest, DefaultConstructorCreatesNullUUID) {
    UUID id;
    EXPECT_TRUE(id.isNull());
}

TEST(UUIDTest, GeneratedUUIDIsNotNull) {
    UUID id = UUID::generate();
    EXPECT_FALSE(id.isNull());
}

TEST(UUIDTest, CopyConstructorWorks) {
    UUID id1 = UUID::generate();
    UUID id2(id1);

    EXPECT_EQ(id1, id2);
}

TEST(UUIDTest, AssignmentOperatorWorks) {
    UUID id1 = UUID::generate();
    UUID id2;
    id2 = id1;

    EXPECT_EQ(id1, id2);
}

TEST(UUIDTest, ToStringReturnsValidFormat) {
    UUID id = UUID::generate();
    std::string str = id.toString();

    // UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars)
    EXPECT_EQ(str.length(), 36);
    EXPECT_EQ(str[8], '-');
    EXPECT_EQ(str[13], '-');
    EXPECT_EQ(str[18], '-');
    EXPECT_EQ(str[23], '-');
}

TEST(UUIDTest, FromStringRoundTrip) {
    UUID id1 = UUID::generate();
    std::string str = id1.toString();
    UUID id2 = UUID::fromString(str);

    EXPECT_EQ(id1, id2);
}

TEST(UUIDTest, HashingWorks) {
    UUID id = UUID::generate();
    std::hash<UUID> hasher;

    size_t hash = hasher(id);
    EXPECT_NE(hash, 0); // Very unlikely to be 0
}

} // namespace test
} // namespace quadmapper
