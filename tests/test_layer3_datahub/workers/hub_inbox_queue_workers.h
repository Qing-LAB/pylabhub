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
int sender_name_comes_from_the_key();
int wrong_frame_count_drops();
int gap_count_tracks_dropped_sends();
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
int inbox_curve_no_authority_denies();
int inbox_curve_unknown_denied();
int inbox_backpressure_bounded_and_edge_logged();
int inbox_stale_ack_not_attributed_to_next_send();

} // namespace hub_inbox_queue
} // namespace pylabhub::tests::worker
