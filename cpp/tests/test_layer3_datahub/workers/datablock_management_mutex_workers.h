#pragma once

#include <string>

namespace pylabhub::tests::worker::datablock_management_mutex
{
int acquire_and_release_creator(const std::string &shm_name);
int acquire_and_release_creator_hold_long(const std::string &shm_name);
int acquire_and_release_attacher(const std::string &shm_name);
int acquire_and_release_attacher_delayed(const std::string &shm_name);
int zombie_creator_acquire_then_exit(const std::string &shm_name);
int zombie_attacher_recovers(const std::string &shm_name);
int attach_nonexistent_fails(const std::string &shm_name);
} // namespace pylabhub::tests::worker::datablock_management_mutex
