
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
    EXPECT_EQ(extract_value_from_string("key2", ';', input1, '='), "value2");

    // Test with spaces
    std::string input2 = " key1 = value1 ;  key2= value2  ; key3 =value3 ";
    EXPECT_EQ(extract_value_from_string("key1", ';', input2, '='), "value1");
    EXPECT_EQ(extract_value_from_string("key2", ';', input2, '='), "value2");
    EXPECT_EQ(extract_value_from_string("key3", ';', input2, '='), "value3");

    // Test with different separators
    std::string input3 = "key1:value1|key2:value2|key3:value3";
    EXPECT_EQ(extract_value_from_string("key2", '|', input3, ':'), "value2");

    // Test with value containing spaces
    std::string input4 = "message=hello world";
    EXPECT_EQ(extract_value_from_string("message", ';', input4, '='), "hello world");

    // Test when key is not found
    std::string input5 = "key1=value1;key2=value2";
    EXPECT_FALSE(extract_value_from_string("key4", ';', input5, '=').has_value());

    // Test with empty input string
    std::string input6 = "";
    EXPECT_FALSE(extract_value_from_string("key1", ';', input6, '=').has_value());

    // Test with malformed segments
    std::string input7 = "key1=value1;key2;key3=value3";
    EXPECT_FALSE(extract_value_from_string("key2", ';', input7, '=').has_value());

    // Test with empty value
    std::string input8 = "key1=value1;key2=;key3=value3";
    EXPECT_EQ(extract_value_from_string("key2", ';', input8, '='), "");

    // Test first and last keys
    std::string input9 = "first=1;middle=2;last=3";
    EXPECT_EQ(extract_value_from_string("first", ';', input9, '='), "1");
    EXPECT_EQ(extract_value_from_string("last", ';', input9, '='), "3");

    // Test with default arguments
    std::string input10 = "default_key=default_value;another_key=another_value";
    EXPECT_EQ(extract_value_from_string("default_key", ';', input10), "default_value");
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

TEST_F(FormatToolsTest, FormattedTime_DashSpacer)
{
    // Filesystem-safe form: date-time space and time-field colons replaced
    // with '-'. The fractional '.' is preserved so that filenames
    // containing this timestamp sort lexicographically in chronological
    // order (digits < 'l' in the trailing ".log" extension).
    // Expected format: YYYY-MM-DD-HH-MM-SS.ffffff   (26 chars)
    auto now = std::chrono::system_clock::now();
    std::string formatted = formatted_time(now, /*use_dash_spacer=*/true);

    ASSERT_EQ(formatted.length(), 26u);

    // Date-time separators are '-'.
    EXPECT_EQ(formatted[4],  '-'); // year-month
    EXPECT_EQ(formatted[7],  '-'); // month-day
    EXPECT_EQ(formatted[10], '-'); // date-time (was ' ' in human form)
    EXPECT_EQ(formatted[13], '-'); // hour-minute (was ':')
    EXPECT_EQ(formatted[16], '-'); // minute-second (was ':')

    // Fractional separator stays '.' (lex-chrono-sortable in filenames).
    EXPECT_EQ(formatted[19], '.');

    // No space or colon — filesystem-safe guarantee.
    EXPECT_EQ(formatted.find(' '), std::string::npos);
    EXPECT_EQ(formatted.find(':'), std::string::npos);

    auto is_all_digits = [&](size_t start, size_t len)
    {
        for (size_t i = 0; i < len; ++i)
        {
            if (!std::isdigit(static_cast<unsigned char>(formatted[start + i])))
                return false;
        }
        return true;
    };

    EXPECT_TRUE(is_all_digits(0, 4));  // Year
    EXPECT_TRUE(is_all_digits(5, 2));  // Month
    EXPECT_TRUE(is_all_digits(8, 2));  // Day
    EXPECT_TRUE(is_all_digits(11, 2)); // Hour
    EXPECT_TRUE(is_all_digits(14, 2)); // Minute
    EXPECT_TRUE(is_all_digits(17, 2)); // Second
    EXPECT_TRUE(is_all_digits(20, 6)); // Microseconds
}

TEST_F(FormatToolsTest, FormattedTime_DashAndHumanSameTimestamp)
{
    // Both forms must represent the same instant: the digit content at
    // each numeric position must be identical; only separators differ.
    auto now = std::chrono::system_clock::now();
    std::string human = formatted_time(now, /*use_dash_spacer=*/false);
    std::string dash  = formatted_time(now, /*use_dash_spacer=*/true);

    ASSERT_EQ(human.length(), 26u);
    ASSERT_EQ(dash.length(),  26u);

    // Digits at every non-separator position must match.
    for (size_t i : {0u, 1u, 2u, 3u,       // YYYY
                     5u, 6u,               // MM
                     8u, 9u,               // DD
                     11u, 12u,             // HH
                     14u, 15u,             // mm
                     17u, 18u,             // SS
                     20u, 21u, 22u, 23u, 24u, 25u})  // uuuuuu
    {
        EXPECT_EQ(human[i], dash[i]) << "mismatch at digit position " << i;
    }
}

// ============================================================================
// make_buffer / make_buffer_rt
// ============================================================================

TEST_F(FormatToolsTest, MakeBuffer_BasicFormat)
{
    auto buf = make_buffer("{} + {}", 1, 2);
    std::string result(buf.data(), buf.size());
    EXPECT_EQ(result, "1 + 2");
}

TEST_F(FormatToolsTest, MakeBuffer_EmptyFormat)
{
    auto buf = make_buffer("");
    EXPECT_EQ(buf.size(), 0u);
}

TEST_F(FormatToolsTest, MakeBufferRt_BasicFormat)
{
    auto buf = make_buffer_rt("{} items", 42);
    std::string result(buf.data(), buf.size());
    EXPECT_EQ(result, "42 items");
}

TEST_F(FormatToolsTest, MakeBufferRt_EmptyFormat)
{
    auto buf = make_buffer_rt("");
    EXPECT_EQ(buf.size(), 0u);
}

// ============================================================================
// filename_only
// ============================================================================

TEST_F(FormatToolsTest, FilenameOnly_UnixPath)
{
    EXPECT_EQ(filename_only("/foo/bar/baz.cpp"), "baz.cpp");
}

TEST_F(FormatToolsTest, FilenameOnly_WindowsPath)
{
    // filename_only handles both / and \ separators on all platforms
    EXPECT_EQ(filename_only("C:\\foo\\bar.cpp"), "bar.cpp");
}

TEST_F(FormatToolsTest, FilenameOnly_NoSeparator)
{
    EXPECT_EQ(filename_only("file.cpp"), "file.cpp");
}

TEST_F(FormatToolsTest, FilenameOnly_EmptyString)
{
    EXPECT_EQ(filename_only(""), "");
}