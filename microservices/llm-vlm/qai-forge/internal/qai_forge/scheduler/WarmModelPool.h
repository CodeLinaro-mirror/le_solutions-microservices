// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/CancelResult.h"
#include "qai_forge/scheduler/ConversationMemoryCoordinator.h"
#include "qai_forge/scheduler/ModelLoadCoordinator.h"
#include "qai_forge/scheduler/ModelRuntime.h"
#include "qai_forge/scheduler/PlacementAdmissionGateway.h"
#include "qai_forge/scheduler/SubmitResult.h"
#include "qai_forge/scheduler/IRequestRoutingPolicy.h"
#include "qai_forge/backend/BackendFactory.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace scheduler {

class EvictionPolicy;

struct WarmModelPoolConfig {
    size_t max_active_models = 3;
    std::chrono::milliseconds idle_timeout = std::chrono::minutes(5);
    std::chrono::milliseconds cold_model_fairness_wait =
        std::chrono::seconds(120);
    std::chrono::milliseconds tool_response_timeout = std::chrono::seconds(30);
    size_t max_queue_depth_per_model = 0; // 0 = unlimited
    long memory_headroom_mb = 1024;
    // Maximum time a generative job may sit queued before being
    // proactively failed via on_error with INSUFFICIENT_MEMORY/503.
    // 0 = disabled (default). Enforced by
    // ModelRuntime::expireStaleQueuedJobs() from this pool's event loop.
    std::chrono::milliseconds queue_admission_timeout =
        std::chrono::milliseconds(0);
    // Maximum number of concurrently-resident replicas (distinct
    // RuntimeId::replica_id values) one model_id may occupy. 1 (default)
    // preserves single-replica-per-model behavior exactly: every model_id
    // maps to exactly one resident ModelRuntime. Values > 1 let two
    // physical HTPs
    // (or other provider-native targets — see
    // qai_forge::execution::IExecutionProvider::candidatePlans()) each run
    // their own replica of the same model_id concurrently. Session-sticky
    // routing (see SessionAffinity below) keeps a given session_id pinned
    // to one replica across turns; eviction is per-replica (EvictionPolicy
    // reasons on full RuntimeId).
    size_t max_replicas_per_model = 1;
};

struct ModelPoolRuntimeSnapshot {
    std::string model_id;
    // Full per-replica identity (model_id + replica_id). Added alongside
    // model_id (kept for back-compat logging/call sites that only ever
    // cared about the model as a whole) so EvictionPolicy and
    // IRequestRoutingPolicy can distinguish between multiple resident
    // replicas of the same model_id.
    qai_forge::execution::RuntimeId runtime_id;
    ModelRuntimeState state = ModelRuntimeState::NotResident;
    QueueSnapshot queue;
    QueueAdmissionCandidate candidate;
    bool has_running_job = false;
    std::string running_job_id;
    bool healthy = false;
    bool active_reserved = false;
    size_t reservation_count = 0;
    bool eviction_requested = false;
    bool tool_lease_active = false;
    std::string tool_chain_id;
    std::optional<std::chrono::steady_clock::time_point> tool_lease_until;
    std::chrono::steady_clock::time_point last_used_at;
    std::optional<std::chrono::steady_clock::time_point> idle_since;
};

struct ModelPoolSnapshot {
    size_t max_active_models = 0;
    size_t active_reserved_models = 0;
    size_t runtime_count = 0;
    std::vector<ModelPoolRuntimeSnapshot> runtimes;
};

// Tool-chain lease deadline bookkeeping for one resident runtime: how long
// a lease keeps WarmModelPool from draining/evicting it for chain_id.
struct ToolLeaseState {
    std::string chain_id;
    std::optional<std::chrono::steady_clock::time_point> lease_until;

    bool active() const noexcept { return lease_until.has_value(); }
    void clear() noexcept {
        chain_id.clear();
        lease_until.reset();
    }
};

// Owns warm model residency. It coordinates which ModelRuntime may load/unload,
// but ModelRuntime still owns actual per-model execution.
//
// Replica model: one model_id may have 1..max_replicas_per_model
// concurrently-resident ModelRuntime instances, each identified by a
// distinct qai_forge::execution::RuntimeId{model_id, replica_id}. Which
// replica a given reserve() call is routed to is decided first by session
// affinity (a session_id previously routed to a still-live replica stays
// pinned to it — required because backends hold live per-session KV-cache
// state inside one specific worker subprocess), then by routing_policy_
// (see IRequestRoutingPolicy.h) when there is no affinity to honor. With
// the default max_replicas_per_model == 1 this class behaves identically
// to the single-runtime-per-model design.
class WarmModelPool {
public:
    explicit WarmModelPool(WarmModelPoolConfig config,
                           ModelRuntimePairFactory runtime_factory = {},
                           ModelRuntimeEvents runtime_events = {},
                           std::shared_ptr<ConversationMemoryCoordinator>
                               memory_coordinator = {},
                           std::shared_ptr<ModelLoadCoordinator>
                               load_coordinator = {},
                           std::shared_ptr<PlacementAdmissionGateway> placement = {},
                           std::shared_ptr<IRequestRoutingPolicy>
                               routing_policy = {});
    ~WarmModelPool();

    WarmModelPool(const WarmModelPool&) = delete;
    WarmModelPool& operator=(const WarmModelPool&) = delete;
    WarmModelPool(WarmModelPool&&) = delete;
    WarmModelPool& operator=(WarmModelPool&&) = delete;

    void start();
    // session_id empty => no affinity (routing_policy_ decides, same as
    // pre-affinity behavior). Non-empty => sticky: a live prior mapping for
    // session_id is honored and bypasses routing_policy_ entirely; a fresh
    // mapping is recorded after routing resolves. See
    // sessionAffinityTargetLocked().
    ModelRuntime* reserve(const std::string& model_id,
                          const std::string& session_id = std::string());
    SubmitResult submit(ModelRuntime* runtime, GenerativeJobPtr job);
    void releaseReservation(ModelRuntime* runtime) noexcept;
    CancelResult cancel(const std::string& job_id);
    // session_id empty => broadcast to every resident replica of model_id
    // (legacy/defensive behavior). Non-empty with a live affinity mapping
    // => target only that one replica.
    bool openToolLease(const std::string& model_id,
                       const std::string& chain_id,
                       std::chrono::milliseconds ttl,
                       const std::string& session_id = std::string());
    bool renewToolLease(const std::string& model_id,
                        const std::string& chain_id,
                        std::chrono::milliseconds ttl,
                        const std::string& session_id = std::string());
    bool closeToolLease(const std::string& model_id,
                        const std::string& chain_id,
                        const std::string& session_id = std::string());
    void checkpoint();
    ModelPoolSnapshot snapshot() const;
    void stop(bool force = false);

    /**
     * Forward clearSession(session_id) to the single replica this
     * session_id has affinity to, if any; otherwise broadcasts to every
     * resident replica of model_id (legacy/defensive behavior — no-op if
     * model_id has no resident runtime). Intended for LiteRT-LM
     * Conversation API cleanup; all other backends ignore it (default
     * no-op in qai_forge::IGenerativeBackend).
     */
    void clearSession(const std::string& model_id, const std::string& session_id);

private:
    struct RuntimeRecord {
        std::unique_ptr<ModelRuntime> runtime;
        ModelRuntimeState state = ModelRuntimeState::NotResident;
        bool active_reserved = false;
        size_t reservation_count = 0;
        bool eviction_requested = false;
        ToolLeaseState tool_lease;
        std::chrono::steady_clock::time_point last_used_at =
            std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::time_point> idle_since;
    };

    // Sticky session -> replica mapping. Required because stateful
    // backends (GenIE/LlamaCpp/LiteRT-LM) hold live per-session KV-cache
    // state inside one specific worker subprocess — routing a session's
    // 2nd+ turn to a different replica than its 1st would silently
    // corrupt/lose conversation state.
    //
    // Keyed by (model_id, session_id) rather than session_id alone — see
    // session_affinity_'s doc comment below for why: session_id can be
    // reused by the same caller across different model_ids (e.g. the
    // OpenAI "user" field or previous_response_id), and keying by
    // session_id alone would let a stale affinity entry recorded for one
    // model_id be incorrectly returned for a request against a different
    // model_id, routing it to the wrong model's resident ModelRuntime.
    struct SessionAffinity {
        qai_forge::execution::RuntimeId runtime_id;
        std::chrono::steady_clock::time_point last_used;
    };

    enum class PoolActionType {
        Activate,
        Drain,
        Reject
    };

    struct PoolAction {
        PoolActionType type;
        qai_forge::execution::RuntimeId runtime_id;
        ModelRuntime* runtime = nullptr;
        ModelLoadCoordinator::LoadReservation load_reservation;
    };

    void eventLoop();
    std::vector<PoolAction> planActionsLocked(
        std::chrono::steady_clock::time_point now);
    void applyActions(std::vector<PoolAction> actions);
    ModelPoolSnapshot snapshotLocked() const;
    void refreshTimedStateLocked(std::chrono::steady_clock::time_point now);
    void handleRuntimeStateChanged(const qai_forge::execution::RuntimeId& runtime_id,
                                ModelRuntimeState state);

    // Resolves (and lazily creates, up to max_replicas_per_model) the
    // replica this reserve() call should be routed to, per routing_policy_.
    RuntimeRecord& routeOrCreateRuntimeLocked(
        const std::string& model_id,
        std::chrono::steady_clock::time_point now);
    RuntimeRecord& getOrCreateRuntimeLocked(
        const qai_forge::execution::RuntimeId& runtime_id,
        std::chrono::steady_clock::time_point now);
    // First (lowest replica_id) resident-or-pending replica record for
    // model_id, or nullptr if none exist yet. EvictionPolicy/
    // planActionsLocked reason about one representative replica per
    // model_id — see the class-level comment above.
    RuntimeRecord* firstReplicaRecordLocked(const std::string& model_id);
    const std::vector<qai_forge::execution::RuntimeId>* replicasForModelLocked(
        const std::string& model_id) const;

    // Looks up session_affinity_[sessionAffinityKey(model_id, session_id)];
    // if found but the target RuntimeId no longer exists in runtimes_ (or
    // is in a terminal state), erases the stale entry and returns
    // nullptr. Does not create or update any affinity entry. model_id is
    // part of the lookup key — see session_affinity_'s doc comment for
    // why a session_id-only key would be incorrect.
    RuntimeRecord* sessionAffinityTargetLocked(const std::string& model_id,
                                                const std::string& session_id);
    // Builds the composite (model_id, session_id) key used by
    // session_affinity_. Plain string concatenation with a separator
    // byte (0x1f, "unit separator") that cannot appear in either
    // component under normal usage — model_id and session_id are both
    // user/config-controlled identifiers, not arbitrary binary data.
    static std::string sessionAffinityKey(const std::string& model_id,
                                          const std::string& session_id);
    // Prunes session_affinity_ entries whose last_used predates
    // now - config_.idle_timeout. Piggybacked on planActionsLocked()'s
    // tick cadence — no dedicated thread/condvar.
    void pruneSessionAffinityLocked(std::chrono::steady_clock::time_point now);

    size_t activeReservedCountLocked() const;
    long availableMemoryMb() const;
    long modelMemoryMb(const std::string& model_id) const;
    bool runtimeHasQueuedWorkLocked(const RuntimeRecord& record) const;
    void clearToolLeaseLocked(RuntimeRecord& record);
    std::chrono::steady_clock::time_point nextPolicyDeadlineLocked(
        std::chrono::steady_clock::time_point now) const;

    static bool isActiveReservedState(ModelRuntimeState state);
    static bool isTerminalState(ModelRuntimeState state);

    WarmModelPoolConfig config_;
    ModelRuntimePairFactory runtime_factory_;
    ModelRuntimeEvents runtime_events_;
    std::shared_ptr<ConversationMemoryCoordinator> memory_coordinator_;
    std::shared_ptr<ModelLoadCoordinator> load_coordinator_;
    bool owns_load_coordinator_ = false;
    std::shared_ptr<PlacementAdmissionGateway> placement_;
    std::shared_ptr<IRequestRoutingPolicy> routing_policy_;
    std::unique_ptr<EvictionPolicy> eviction_policy_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread event_thread_;
    bool started_ = false;
    bool shutdown_requested_ = false;
    bool event_pending_ = false;

    std::unordered_map<qai_forge::execution::RuntimeId, RuntimeRecord,
                       qai_forge::execution::RuntimeIdHash> runtimes_;
    // Enumerates resident/pending replica RuntimeIds per model_id, in
    // ascending replica_id order. Mirrors runtimes_'s keys grouped by
    // model_id — kept as a separate index since several call sites
    // (reserve(), openToolLease(), clearSession(), eviction's
    // one-representative-per-model lookups) need "all replicas of
    // model_id" rather than a single RuntimeId lookup.
    std::unordered_map<std::string, std::vector<qai_forge::execution::RuntimeId>>
        replicas_by_model_;
    // Sticky (model_id, session_id) -> replica mapping, keyed by
    // sessionAffinityKey(model_id, session_id). See SessionAffinity's doc
    // comment for why model_id must be part of the key. Guarded by
    // mutex_, pruned from planActionsLocked()'s tick.
    std::unordered_map<std::string, SessionAffinity> session_affinity_;
};

} // namespace scheduler
