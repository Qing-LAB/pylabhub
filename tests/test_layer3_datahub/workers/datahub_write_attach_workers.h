// tests/test_layer3_datahub/workers/datahub_write_attach_workers.h
#pragma once

/**
 * Workers for DataBlockOpenMode::WriteAttach tests.
 *
 * Scenarios verify that a source process can attach R/W to a hub-created segment,
 * perform writes, and that security/schema validation on WriteAttach works correctly.
 *
 * Secret numbers start at 76001.
 */
namespace pylabhub::tests::worker::write_attach
{

/** Creator initializes; WriteAttach writer connects and writes a slot; creator-side consumer reads it. */
int creator_then_writer_attach_basic();
/** WriteAttach with wrong shared_secret → attach fails (returns nullptr). */
int writer_attach_validates_secret();
/** WriteAttach with mismatched schema hash → attach fails (returns nullptr). */
int writer_attach_validates_schema();
/** Writer detaches; creator still holds segment; DiagnosticHandle opens successfully. */
int segment_persists_after_writer_detach();

} // namespace pylabhub::tests::worker::write_attach
