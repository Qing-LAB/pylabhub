#pragma once

#include <string>

namespace pylabhub::tests::worker::datablock_management_mutex
{
int acquire_and_release(const std::string &shm_name);
int try_acquire_non_blocking(const std::string &shm_name);
} // namespace pylabhub::tests::worker::datablock_management_mutex
