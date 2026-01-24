
#include "plh_base.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::format_tools;

// Test fixture for format tools tests
class FormatToolsTest : public ::testing::Test
{
};

TEST_F(FormatToolsTest, ExtractValueFromString)
{
    // Basic test case
    std::string input1 = "key1=value1;key2=value2;key3=value3";
    EXPECT_EQ(extract_value_from_string("key2", input1, ';', '='), "value2");

    // Test with spaces
    std::string input2 = " key1 = value1 ;  key2= value2  ; key3 =value3 ";
    EXPECT_EQ(extract_value_from_string("key1", input2, ';', '='), "value1");
    EXPECT_EQ(extract_value_from_string("key2", input2, ';', '='), "value2");
    EXPECT_EQ(extract_value_from_string("key3", input2, ';', '='), "value3");

    // Test with different separators
    std::string input3 = "key1:value1|key2:value2|key3:value3";
    EXPECT_EQ(extract_value_from_string("key2", input3, '|', ':'), "value2");

    // Test with value containing spaces
    std::string input4 = "message=hello world";
    EXPECT_EQ(extract_value_from_string("message", input4, ';', '='), "hello world");

    // Test when key is not found
    std::string input5 = "key1=value1;key2=value2";
    EXPECT_FALSE(extract_value_from_string("key4", input5, ';', '=').has_value());

    // Test with empty input string
    std::string input6 = "";
    EXPECT_FALSE(extract_value_from_string("key1", input6, ';', '=').has_value());

    // Test with malformed segments
    std::string input7 = "key1=value1;key2;key3=value3";
    EXPECT_FALSE(extract_value_from_string("key2", input7, ';', '=').has_value());

    // Test with empty value
    std::string input8 = "key1=value1;key2=;key3=value3";
    EXPECT_EQ(extract_value_from_string("key2", input8, ';', '='), "");

    // Test first and last keys
    std::string input9 = "first=1;middle=2;last=3";
    EXPECT_EQ(extract_value_from_string("first", input9, ';', '='), "1");
    EXPECT_EQ(extract_value_from_string("last", input9, ';', '='), "3");

    // Test with default arguments
    std::string input10 = "default_key=default_value;another_key=another_value";
    EXPECT_EQ(extract_value_from_string("default_key", input10), "default_value");
}

TEST_F(FormatToolsTest, FormattedTime)
{
    auto now = std::chrono::system_clock::now();
    std::string formatted = formatted_time(now);

    // Expected format: YYYY-MM-DD HH:MM:SS.ffffff
    // Example: 2023-11-20 14:30:55.123456
    ASSERT_EQ(formatted.length(), 26);

    // Check separators
    EXPECT_EQ(formatted[4], '-');
    EXPECT_EQ(formatted[7], '-');
    EXPECT_EQ(formatted[10], ' ');
    EXPECT_EQ(formatted[13], ':');
    EXPECT_EQ(formatted[16], ':');
    EXPECT_EQ(formatted[19], '.');

    // Helper lambda to check if a substring contains only digits
    auto is_all_digits = [&](size_t start, size_t len)
    {
        for (size_t i = 0; i < len; ++i)
        {
            if (!std::isdigit(static_cast<unsigned char>(formatted[start + i])))
            {
                return false;
            }
        }
        return true;
    };

    // Check numeric parts
    EXPECT_TRUE(is_all_digits(0, 4));  // Year
    EXPECT_TRUE(is_all_digits(5, 2));  // Month
    EXPECT_TRUE(is_all_digits(8, 2));  // Day
    EXPECT_TRUE(is_all_digits(11, 2)); // Hour
    EXPECT_TRUE(is_all_digits(14, 2)); // Minute
    EXPECT_TRUE(is_all_digits(17, 2)); // Second
    EXPECT_TRUE(is_all_digits(20, 6)); // Microseconds
}