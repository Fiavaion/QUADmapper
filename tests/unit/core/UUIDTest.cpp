#include <gtest/gtest.h>
#include "core/UUID.hpp"

#include <stdexcept>

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

TEST(UUIDTest, FromStringTakesOnlyTheCanonicalShape) {
    EXPECT_EQ(UUID::fromString("0A1B2C3D-0000-4000-8000-00000000000F").toString(),
              "0a1b2c3d-0000-4000-8000-00000000000f");
    for (const char* text : {
             "0a1b2c3d0-000-4000-8000-000000000001",    // a dash in the wrong place
             "0a1b2c3g-0000-4000-8000-000000000001",    // not a hex digit
             " a1b2c3d-0000-4000-8000-000000000001",    // leading space
             "+a1b2c3d-0000-4000-8000-000000000001",    // sign
             "0xa1b2c3-0000-4000-8000-000000000001",    // hex prefix
             "0a1b2c3d-0000-4000-8000-00000000001"}) {  // too short
        EXPECT_THROW(UUID::fromString(text), std::invalid_argument) << text;
    }
}

TEST(UUIDTest, HashingWorks) {
    UUID id = UUID::generate();
    std::hash<UUID> hasher;

    size_t hash = hasher(id);
    EXPECT_NE(hash, 0); // Very unlikely to be 0
}

} // namespace test
} // namespace quadmapper
