// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

/**
 * @file test_response_store.cpp
 * @brief Unit tests for ResponseStore.
 *
 * Build with BUILD_TESTS=ON:
 *   cmake -S /path/to/responses -B build -DBUILD_TESTS=ON
 *   cmake --build build
 *   ctest --test-dir build
 *
 * No external test framework required — only nlohmann_json is linked.
 */

#include "ResponseStore.h"

#include <cassert>
#include <iostream>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal test harness
// ─────────────────────────────────────────────────────────────────────────────

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) static void name()
#define RUN(name)                                          \
    do {                                                   \
        ResponseStore::getInstance().clearForTest();       \
        std::cout << "  [ RUN ] " #name "\n";             \
        try {                                              \
            name();                                        \
            std::cout << "  [ OK  ] " #name "\n";         \
            ++g_passed;                                    \
        } catch (const std::exception& e) {               \
            std::cout << "  [FAIL] " #name ": "           \
                      << e.what() << "\n";                 \
            ++g_failed;                                    \
        } catch (...) {                                    \
            std::cout << "  [FAIL] " #name ": unknown\n"; \
            ++g_failed;                                    \
        }                                                  \
    } while (0)

#define EXPECT(cond)                                                        \
    do {                                                                    \
        if (!(cond)) {                                                      \
            throw std::runtime_error("Assertion failed: " #cond            \
                                     " at line " + std::to_string(__LINE__)); \
        }                                                                   \
    } while (0)

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static ResponseStoreJson makeMessages(const std::string& role,
                                      const std::string& content) {
    return ResponseStoreJson::array(
        {{{"role", role}, {"content", content}}});
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests: beginResponse
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_begin_root_response_ok) {
    auto& store = ResponseStore::getInstance();

    auto result = store.beginResponse(
        "resp_001", "gpt-4o", "",
        ResponseStoreJson::array(),
        makeMessages("user", "Hello"),
        ResponseStoreJson::object());

    EXPECT(result.ok);
    EXPECT(result.response_id == "resp_001");
    EXPECT(!result.session_id.empty());
    EXPECT(result.ancestor_messages.empty());
    EXPECT(result.current_request_messages.size() == 1);
}

TEST(test_begin_duplicate_response_id_returns_409) {
    auto& store = ResponseStore::getInstance();

    store.beginResponse("resp_dup", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "First"),
                        ResponseStoreJson::object());

    auto result = store.beginResponse("resp_dup", "gpt-4o", "",
                                      ResponseStoreJson::array(),
                                      makeMessages("user", "Second"),
                                      ResponseStoreJson::object());

    EXPECT(!result.ok);
    EXPECT(result.http_status == 409);
}

TEST(test_begin_unknown_previous_response_returns_404) {
    auto& store = ResponseStore::getInstance();

    auto result = store.beginResponse(
        "resp_child", "gpt-4o", "resp_nonexistent",
        ResponseStoreJson::array(),
        makeMessages("user", "Follow-up"),
        ResponseStoreJson::object());

    EXPECT(!result.ok);
    EXPECT(result.http_status == 404);
}

TEST(test_begin_previous_in_progress_returns_409) {
    auto& store = ResponseStore::getInstance();

    // Create a root response but do NOT complete it (stays InProgress)
    store.beginResponse("resp_parent", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Parent"),
                        ResponseStoreJson::object());

    auto result = store.beginResponse(
        "resp_child", "gpt-4o", "resp_parent",
        ResponseStoreJson::array(),
        makeMessages("user", "Child"),
        ResponseStoreJson::object());

    EXPECT(!result.ok);
    EXPECT(result.http_status == 409);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests: completeResponse
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_complete_response_ok) {
    auto& store = ResponseStore::getInstance();

    store.beginResponse("resp_c", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Hi"),
                        ResponseStoreJson::object());

    bool ok = store.completeResponse(
        "resp_c",
        makeMessages("assistant", "Hello!"),
        ResponseStoreJson::array(),
        ResponseStoreJson::object(),
        ResponseStoreJson::object());

    EXPECT(ok);

    auto stored = store.getResponse("resp_c");
    EXPECT(stored.has_value());
    EXPECT(stored->status == StoredResponseStatus::Completed);
}

TEST(test_complete_unknown_response_returns_false) {
    auto& store = ResponseStore::getInstance();

    bool ok = store.completeResponse(
        "resp_ghost",
        makeMessages("assistant", "?"),
        ResponseStoreJson::array(),
        ResponseStoreJson::object(),
        ResponseStoreJson::object());

    EXPECT(!ok);
}

TEST(test_complete_already_completed_returns_false) {
    auto& store = ResponseStore::getInstance();

    store.beginResponse("resp_cc", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Hi"),
                        ResponseStoreJson::object());
    store.completeResponse("resp_cc",
                           makeMessages("assistant", "Hello!"),
                           ResponseStoreJson::array(),
                           ResponseStoreJson::object(),
                           ResponseStoreJson::object());

    // Second complete on an already-completed response must fail
    bool ok = store.completeResponse("resp_cc",
                                     makeMessages("assistant", "Again"),
                                     ResponseStoreJson::array(),
                                     ResponseStoreJson::object(),
                                     ResponseStoreJson::object());
    EXPECT(!ok);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests: failResponse
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_fail_response_ok) {
    auto& store = ResponseStore::getInstance();

    store.beginResponse("resp_f", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Hi"),
                        ResponseStoreJson::object());

    bool ok = store.failResponse(
        "resp_f",
        {{"code", "server_error"}, {"message", "Something went wrong"}});

    EXPECT(ok);

    auto stored = store.getResponse("resp_f");
    EXPECT(stored.has_value());
    EXPECT(stored->status == StoredResponseStatus::Failed);
    EXPECT(!stored->error.is_null());
}

TEST(test_fail_unknown_response_returns_false) {
    auto& store = ResponseStore::getInstance();
    bool ok = store.failResponse("resp_ghost", ResponseStoreJson::object());
    EXPECT(!ok);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests: cancelResponse
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_cancel_in_progress_response_ok) {
    auto& store = ResponseStore::getInstance();

    store.beginResponse("resp_cancel", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Hi"),
                        ResponseStoreJson::object());

    auto outcome = store.cancelResponse("resp_cancel");
    EXPECT(outcome == CancelOutcome::Cancelled);

    auto stored = store.getResponse("resp_cancel");
    EXPECT(stored.has_value());
    EXPECT(stored->status == StoredResponseStatus::Cancelled);
    EXPECT(stored->active_job_id.empty());
}

TEST(test_cancel_unknown_response_returns_not_found) {
    auto& store = ResponseStore::getInstance();
    auto outcome = store.cancelResponse("resp_ghost");
    EXPECT(outcome == CancelOutcome::NotFound);
}

TEST(test_cancel_completed_response_returns_invalid_state) {
    auto& store = ResponseStore::getInstance();

    store.beginResponse("resp_done", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Hi"),
                        ResponseStoreJson::object());
    store.completeResponse("resp_done",
                           makeMessages("assistant", "Hello!"),
                           ResponseStoreJson::array(),
                           ResponseStoreJson::object(),
                           ResponseStoreJson::object());

    auto outcome = store.cancelResponse("resp_done");
    EXPECT(outcome == CancelOutcome::InvalidState);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests: deleteCascade
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_delete_completed_response_ok) {
    auto& store = ResponseStore::getInstance();

    store.beginResponse("resp_del", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Hi"),
                        ResponseStoreJson::object());
    store.completeResponse("resp_del",
                           makeMessages("assistant", "Hello!"),
                           ResponseStoreJson::array(),
                           ResponseStoreJson::object(),
                           ResponseStoreJson::object());

    auto result = store.deleteCascade("resp_del");
    EXPECT(result.ok);
    EXPECT(!store.getResponse("resp_del").has_value());
}

TEST(test_delete_unknown_response_returns_404) {
    auto& store = ResponseStore::getInstance();
    auto result = store.deleteCascade("resp_ghost");
    EXPECT(!result.ok);
    EXPECT(result.http_status == 404);
}

TEST(test_delete_in_progress_response_returns_409) {
    auto& store = ResponseStore::getInstance();

    store.beginResponse("resp_active", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Hi"),
                        ResponseStoreJson::object());

    auto result = store.deleteCascade("resp_active");
    EXPECT(!result.ok);
    EXPECT(result.http_status == 409);
}

TEST(test_delete_cascade_removes_children) {
    auto& store = ResponseStore::getInstance();

    // Parent
    store.beginResponse("resp_p", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Turn 1"),
                        ResponseStoreJson::object());
    store.completeResponse("resp_p",
                           makeMessages("assistant", "Reply 1"),
                           ResponseStoreJson::array(),
                           ResponseStoreJson::object(),
                           ResponseStoreJson::object());

    // Child
    store.beginResponse("resp_ch", "gpt-4o", "resp_p",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Turn 2"),
                        ResponseStoreJson::object());
    store.completeResponse("resp_ch",
                           makeMessages("assistant", "Reply 2"),
                           ResponseStoreJson::array(),
                           ResponseStoreJson::object(),
                           ResponseStoreJson::object());

    // Delete from root — should remove both
    auto result = store.deleteCascade("resp_p");
    EXPECT(result.ok);
    EXPECT(!store.getResponse("resp_p").has_value());
    EXPECT(!store.getResponse("resp_ch").has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests: expireStaleInProgress
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_expire_stale_in_progress) {
    auto& store = ResponseStore::getInstance();

    // TTL of 1 second
    store.beginResponse("resp_exp", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Hi"),
                        ResponseStoreJson::object(),
                        /*ttl_seconds=*/1);

    // Simulate time well past expiry
    int far_future = static_cast<int>(
        std::chrono::system_clock::now().time_since_epoch().count()
        / 1000000000LL) + 3600;

    auto expired = store.expireStaleInProgress(far_future);
    EXPECT(expired.size() == 1);
    EXPECT(expired[0].response_id == "resp_exp");

    auto stored = store.getResponse("resp_exp");
    EXPECT(stored.has_value());
    EXPECT(stored->status == StoredResponseStatus::Expired);
}

TEST(test_expire_does_not_affect_completed_responses) {
    auto& store = ResponseStore::getInstance();

    store.beginResponse("resp_comp", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Hi"),
                        ResponseStoreJson::object(),
                        /*ttl_seconds=*/1);
    store.completeResponse("resp_comp",
                           makeMessages("assistant", "Hello!"),
                           ResponseStoreJson::array(),
                           ResponseStoreJson::object(),
                           ResponseStoreJson::object());

    int far_future = static_cast<int>(
        std::chrono::system_clock::now().time_since_epoch().count()
        / 1000000000LL) + 3600;

    auto expired = store.expireStaleInProgress(far_future);
    EXPECT(expired.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests: accessors
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_get_response_returns_nullopt_for_unknown) {
    auto& store = ResponseStore::getInstance();
    EXPECT(!store.getResponse("resp_ghost").has_value());
}

TEST(test_get_active_job_returns_job_id_for_in_progress) {
    auto& store = ResponseStore::getInstance();

    store.beginResponse("resp_job", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Hi"),
                        ResponseStoreJson::object());

    auto job = store.getActiveJob("resp_job");
    EXPECT(job.has_value());
    EXPECT(*job == "resp_job");
}

TEST(test_get_active_job_returns_nullopt_after_complete) {
    auto& store = ResponseStore::getInstance();

    store.beginResponse("resp_job2", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Hi"),
                        ResponseStoreJson::object());
    store.completeResponse("resp_job2",
                           makeMessages("assistant", "Hello!"),
                           ResponseStoreJson::array(),
                           ResponseStoreJson::object(),
                           ResponseStoreJson::object());

    auto job = store.getActiveJob("resp_job2");
    EXPECT(!job.has_value());
}

TEST(test_get_input_items_returns_stored_items) {
    auto& store = ResponseStore::getInstance();

    ResponseStoreJson items = ResponseStoreJson::array(
        {{{"type", "message"}, {"content", "Hello"}}});

    store.beginResponse("resp_items", "gpt-4o", "",
                        items,
                        makeMessages("user", "Hello"),
                        ResponseStoreJson::object());

    auto retrieved = store.getInputItems("resp_items");
    EXPECT(retrieved.has_value());
    EXPECT(retrieved->size() == 1);
}

TEST(test_get_input_items_returns_nullopt_for_unknown) {
    auto& store = ResponseStore::getInstance();
    EXPECT(!store.getInputItems("resp_ghost").has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests: buildCandidateMessages
// ─────────────────────────────────────────────────────────────────────────────

TEST(test_build_candidate_no_previous_returns_empty_ancestors) {
    auto& store = ResponseStore::getInstance();

    auto result = store.buildCandidateMessages(
        "", makeMessages("user", "Hello"));

    EXPECT(result.ok);
    EXPECT(result.ancestor_messages.empty());
    EXPECT(result.current_request_messages.size() == 1);
}

TEST(test_build_candidate_unknown_previous_returns_404) {
    auto& store = ResponseStore::getInstance();

    auto result = store.buildCandidateMessages(
        "resp_ghost", makeMessages("user", "Follow-up"));

    EXPECT(!result.ok);
    EXPECT(result.http_status == 404);
}

TEST(test_build_candidate_in_progress_previous_returns_409) {
    auto& store = ResponseStore::getInstance();

    store.beginResponse("resp_ip", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Turn 1"),
                        ResponseStoreJson::object());

    auto result = store.buildCandidateMessages(
        "resp_ip", makeMessages("user", "Turn 2"));

    EXPECT(!result.ok);
    EXPECT(result.http_status == 409);
}

TEST(test_build_candidate_multi_turn_chain) {
    auto& store = ResponseStore::getInstance();

    // Turn 1
    store.beginResponse("resp_t1", "gpt-4o", "",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Turn 1 user"),
                        ResponseStoreJson::object());
    store.completeResponse("resp_t1",
                           makeMessages("assistant", "Turn 1 assistant"),
                           ResponseStoreJson::array(),
                           ResponseStoreJson::object(),
                           ResponseStoreJson::object());

    // Turn 2
    store.beginResponse("resp_t2", "gpt-4o", "resp_t1",
                        ResponseStoreJson::array(),
                        makeMessages("user", "Turn 2 user"),
                        ResponseStoreJson::object());
    store.completeResponse("resp_t2",
                           makeMessages("assistant", "Turn 2 assistant"),
                           ResponseStoreJson::array(),
                           ResponseStoreJson::object(),
                           ResponseStoreJson::object());

    // Build context for Turn 3
    auto result = store.buildCandidateMessages(
        "resp_t2", makeMessages("user", "Turn 3 user"));

    EXPECT(result.ok);
    // Ancestors: [t1 user, t1 assistant, t2 user, t2 assistant] = 4 messages
    EXPECT(result.ancestor_messages.size() == 4);
    EXPECT(result.current_request_messages.size() == 1);
}

TEST(test_session_id_shared_across_turns) {
    auto& store = ResponseStore::getInstance();

    auto r1 = store.beginResponse("resp_s1", "gpt-4o", "",
                                  ResponseStoreJson::array(),
                                  makeMessages("user", "Turn 1"),
                                  ResponseStoreJson::object());
    store.completeResponse("resp_s1",
                           makeMessages("assistant", "Reply 1"),
                           ResponseStoreJson::array(),
                           ResponseStoreJson::object(),
                           ResponseStoreJson::object());

    auto r2 = store.beginResponse("resp_s2", "gpt-4o", "resp_s1",
                                  ResponseStoreJson::array(),
                                  makeMessages("user", "Turn 2"),
                                  ResponseStoreJson::object());

    EXPECT(r1.ok);
    EXPECT(r2.ok);
    EXPECT(r1.session_id == r2.session_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== ResponseStore Unit Tests ===\n\n";

    // beginResponse
    RUN(test_begin_root_response_ok);
    RUN(test_begin_duplicate_response_id_returns_409);
    RUN(test_begin_unknown_previous_response_returns_404);
    RUN(test_begin_previous_in_progress_returns_409);

    // completeResponse
    RUN(test_complete_response_ok);
    RUN(test_complete_unknown_response_returns_false);
    RUN(test_complete_already_completed_returns_false);

    // failResponse
    RUN(test_fail_response_ok);
    RUN(test_fail_unknown_response_returns_false);

    // cancelResponse
    RUN(test_cancel_in_progress_response_ok);
    RUN(test_cancel_unknown_response_returns_not_found);
    RUN(test_cancel_completed_response_returns_invalid_state);

    // deleteCascade
    RUN(test_delete_completed_response_ok);
    RUN(test_delete_unknown_response_returns_404);
    RUN(test_delete_in_progress_response_returns_409);
    RUN(test_delete_cascade_removes_children);

    // expireStaleInProgress
    RUN(test_expire_stale_in_progress);
    RUN(test_expire_does_not_affect_completed_responses);

    // accessors
    RUN(test_get_response_returns_nullopt_for_unknown);
    RUN(test_get_active_job_returns_job_id_for_in_progress);
    RUN(test_get_active_job_returns_nullopt_after_complete);
    RUN(test_get_input_items_returns_stored_items);
    RUN(test_get_input_items_returns_nullopt_for_unknown);

    // buildCandidateMessages
    RUN(test_build_candidate_no_previous_returns_empty_ancestors);
    RUN(test_build_candidate_unknown_previous_returns_404);
    RUN(test_build_candidate_in_progress_previous_returns_409);
    RUN(test_build_candidate_multi_turn_chain);
    RUN(test_session_id_shared_across_turns);

    std::cout << "\n=== Results: " << g_passed << " passed, "
              << g_failed << " failed ===\n";

    return g_failed == 0 ? 0 : 1;
}
