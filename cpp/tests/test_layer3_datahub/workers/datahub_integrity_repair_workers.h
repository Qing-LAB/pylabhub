// tests/test_layer3_datahub/workers/datahub_integrity_repair_workers.h
#pragma once

/**
 * Workers for integrity validation and repair tests.
 *
 * These tests exercise datablock_validate_integrity using DiagnosticHandle to inject
 * controlled corruption, verifying detection (repair=false) and repair (repair=true)
 * behavior for each corruption type.
 *
 * Checksum slot repair is not tested here because the repair path in validate_integrity
 * uses create_datablock_producer_impl (which reinitializes the header), making it
 * unsuitable for in-place repair testing via the standard test fixture.  That path is
 * tracked in TESTING_TODO.md § "Medium Priority".
 *
 * Secrets start at 78001.
 */
namespace pylabhub::tests::worker::integrity_repair
{

/** ChecksumPolicy::Enforced block, 2 slots written → validate_integrity returns SUCCESS. */
int validate_integrity_fresh_checksum_block_passes();
/** Corrupt the stored layout checksum → validate_integrity(false) and (true) both fail. */
int validate_integrity_detects_layout_checksum_mismatch();
/** Corrupt the magic number → validate_integrity returns FAILED. */
int validate_integrity_detects_magic_number_corruption();

} // namespace pylabhub::tests::worker::integrity_repair
