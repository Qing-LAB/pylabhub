#pragma once
/**
 * @file datahub_schema_loader_workers.h
 * @brief Workers for the file-walker entry-point of the schema loader
 *        (HEP-CORE-0034 §2.4 I2; Pattern 3).  Suite 1 (the 12 plain
 *        TESTs for `SchemaLibrary::load_from_string`) stays Pattern 1
 *        in the parent file; only the 4 file-loading TEST_F bodies
 *        are migrated to subprocess workers.
 */

namespace pylabhub::tests::worker
{
namespace datahub_schema_loader
{

int load_all_from_dirs_single_file();
int load_all_from_dirs_nested_path();
int load_all_from_dirs_invalid_json_skipped();
int load_all_from_dirs_first_match_wins_across_dirs();

} // namespace datahub_schema_loader
} // namespace pylabhub::tests::worker
