#pragma once
// tests/test_layer3_datahub/workers/datahub_engine_workers.h
//
// L3 engine round-trip tests: engine → RoleAPIBase → SHM → consumer reads back.

namespace pylabhub::tests::worker::engine_roundtrip
{

/** Python engine: produce multifield slot to SHM, consumer verifies. */
int python_shm_roundtrip(int argc, char **argv);

/** Lua engine: produce multifield slot to SHM, consumer verifies. */
int lua_shm_roundtrip(int argc, char **argv);

/** Native engine: produce multifield slot to SHM, consumer verifies. */
int native_shm_roundtrip(int argc, char **argv);

} // namespace pylabhub::tests::worker::engine_roundtrip
