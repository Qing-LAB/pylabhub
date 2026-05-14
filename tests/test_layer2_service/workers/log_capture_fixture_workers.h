#pragma once
/**
 * @file log_capture_fixture_workers.h
 * @brief Workers for `LogCaptureFixture` self-tests (Pattern 3).
 *
 * Tests the framework's own log-capture helper.  Because the
 * **subject under test IS LogCaptureFixture**, each worker body
 * constructs and owns its own `LogCaptureFixture` instance — the
 * wave's usual "outer LogCaptureFixture wrapping the body" pattern
 * would collide with the test body's inner instance (both redirect
 * the same singleton `Logger` sink).  No outer wrap.
 */

namespace pylabhub::tests::worker
{
namespace log_capture_fixture
{

int expect_log_warn_permissive_allows_no_fail();
int expect_log_warn_permissive_silently_ok_when_warn_does_not_fire();
int expect_log_warn_undeclared_warn_fails();
int expect_log_error_undeclared_error_fails();
int must_fire_warn_fired_no_fail();
int must_fire_warn_not_fired_fails();
int must_fire_error_fired_no_fail();
int must_fire_error_not_fired_fails();
int must_fire_warn_also_satisfies_allowlist();
int must_fire_multiple_emissions_count_as_one_match();
int must_fire_two_distinct_needles_both_must_match();
int must_fire_two_distinct_needles_both_fire_no_fail();
int must_fire_distinct_needles_each_consume_one_line();

} // namespace log_capture_fixture
} // namespace pylabhub::tests::worker
