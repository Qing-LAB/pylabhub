#pragma once
/**
 * @file hub_inbox_queue_workers.h
 * @brief Workers for `pylabhub::hub::InboxQueue` + `InboxClient` tests
 *        (Phase 3 Inbox Facility; Pattern 3).
 */

namespace pylabhub::tests::worker
{
namespace hub_inbox_queue
{

int bind_and_connect_basic();
int recv_one_timeout_returns_null();
int multiple_messages();
int double_stop_no_throw();
int sender_uid_is_preserved();
int bad_magic_drops();
int replay_and_skew_dropped();
int ack_code_3_handler_error();
int not_started_recv_returns_null();
int empty_schema_factory_fails();
int empty_schema_client_factory_fails();
int item_size_matches_schema();
int schema_mismatch_different_type_drops_frame();
int schema_mismatch_different_size_drops_frame();
int checksum_enforced_roundtrip();
int checksum_manual_no_stamp_receiver_rejects();
int checksum_none_roundtrip();
int inbox_curve_authorized_delivers();
int inbox_curve_unknown_denied();

} // namespace hub_inbox_queue
} // namespace pylabhub::tests::worker
