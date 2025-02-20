
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ungive/utility/example.h"

TEST(Example, x_equals_100)
{
    EXPECT_EQ(ungive::utility::x, 100);
    //
}
