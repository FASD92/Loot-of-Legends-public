#include <lol/transport/rudp/ReliableQueue.hpp>
#include <lol/transport/rudp/RttEstimator.hpp>
#include <lol/transport/rudp/RudpBindingRegistry.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace std::chrono_literals;
using namespace lol::transport::rudp;
constexpr auto start = ReliableQueue::Clock::time_point{};

bool check(bool value, const char *message) {
  if (!value)
    std::cerr << message << '\n';
  return value;
}

bool estimatorTracksPath() {
  RttEstimator order;
  if (!check(order.observe(100ms) && order.observe(200ms) &&
                 order.snapshot().srtt == 112500us &&
                 order.snapshot().rttvar == 62500us &&
                 order.snapshot().rto == 363ms,
             "variance must use the previous SRTT"))
    return false;
  RttEstimator estimator;
  if (!check(estimator.snapshot().rto == 200ms, "bootstrap") ||
      !check(estimator.observe(100ms), "first sample") ||
      !check(estimator.snapshot().srtt == 100ms &&
                 estimator.snapshot().rttvar == 50ms &&
                 estimator.snapshot().rto == 300ms,
             "initial variance") ||
      !check(estimator.observe(100ms) && estimator.snapshot().rto == 250ms,
             "variance updated before srtt") ||
      !check(estimator.observe(100ms) && estimator.snapshot().rto == 213ms,
             "ceil fractional milliseconds"))
    return false;
  for (int i = 0; i != 40; ++i) {
    if (!estimator.observe(20ms))
      return false;
  }
  if (!check(estimator.snapshot().rto == 200ms, "stable low RTT floor") ||
      !check(estimator.observe(500ms) && estimator.snapshot().rto > 500ms,
             "jitter expands timeout"))
    return false;
  const auto samples = estimator.snapshot().samples;
  if (!check(!estimator.observe(0us) && !estimator.observe(-1us) &&
                 !estimator.observe(5s) &&
                 estimator.snapshot().samples == samples,
             "invalid samples must not change estimator"))
    return false;
  RttEstimator high;
  RttEstimator slow;
  return check(high.observe(400ms) && high.snapshot().rto == 1000ms,
               "high RTT clamp") &&
         check(slow.observe(1500ms) && slow.snapshot().srtt == 1500ms &&
                   slow.snapshot().rttvar == 750ms &&
                   slow.snapshot().rto == 1000ms,
               "valid RTT above one second is preserved; only RTO is clamped");
}

bool enqueue(ReliableQueue &queue, std::uint32_t sequence,
             ReliableLane lane = ReliableLane::Application) {
  return queue.enqueue(sequence, {std::byte{1}}, lane, start) ==
         ReliableQueueAdmission::Accepted;
}

bool queueSamplesAndBackoff() {
  ReliableQueue queue;
  if (!enqueue(queue, 1) || !enqueue(queue, 2, ReliableLane::Control) ||
      !enqueue(queue, 3) || queue.poll(start + 100ms).transmissions.size() != 3)
    return false;
  for (std::uint32_t seq = 1; seq <= 3; ++seq) {
    if (!queue.recordSend(seq, 1, start + 100ms + seq * 10ms, true))
      return false;
  }
  auto ack = queue.acknowledge(3, 3, start + 530ms);
  if (!check(ack.removed == 3 && ack.sample == 400ms && ack.coalesced == 2 &&
                 queue.empty(),
             "one sample from latest send, excludes enqueue/poll delay") ||
      !check(queue.acknowledge(3, 3, start + 600ms).removed == 0,
             "duplicate ACK has no new sample"))
    return false;
  if (!enqueue(queue, 4) ||
      queue.poll(start, 300ms).transmissions.size() != 1 ||
      !queue.recordSend(4, 1, start, true))
    return false;
  if (!check(queue.poll(start + 299ms, 200ms).transmissions.empty(),
             "pending timer frozen") ||
      !check(queue.poll(start + 300ms, 200ms).transmissions.size() == 1,
             "timeout") ||
      !check(queue.poll(start + 899ms, 200ms).transmissions.empty(),
             "backoff retained"))
    return false;
  ack = queue.acknowledge(4, 0, start + 900ms);
  return check(ack.removed == 1 && !ack.sample && ack.retransmitted == 1,
               "Karn: even an unconfirmed retransmission excludes RTT");
}

bool failedSendAndExpiry() {
  ReliableQueue queue;
  if (!enqueue(queue, 1) || queue.poll(start).transmissions.size() != 1 ||
      !queue.recordSend(1, 1, start, false))
    return false;
  auto ack = queue.acknowledge(1, 0, start + 100ms);
  if (!check(ack.removed == 1 && !ack.sample && ack.sendUnconfirmed == 1,
             "failed send ACK does not train") ||
      !enqueue(queue, 2) || queue.poll(start).transmissions.size() != 1 ||
      !queue.recordSend(2, 1, start, true))
    return false;
  if (!check(!queue.recordSend(2, 1, start + 1ms, true),
             "duplicate send feedback rejected"))
    return false;
  ack = queue.acknowledge(2, 0, start);
  if (!check(!ack.sample && ack.nonpositive == 1, "zero RTT") ||
      !enqueue(queue, 3) || queue.poll(start).transmissions.size() != 1 ||
      !queue.recordSend(3, 1, start, true))
    return false;
  ack = queue.acknowledge(3, 0, start + 5s);
  if (!check(!ack.sample && ack.expired == 1, "expired sample rejected"))
    return false;
  if (!enqueue(queue, 4) || queue.poll(start).transmissions.size() != 1)
    return false;
  for (int i = 1; i < 5; ++i) {
    auto poll = queue.poll(start + i * 1s);
    if (!check(poll.transmissions.size() == 1 &&
                   poll.transmissions[0].attempt == i + 1,
               "five attempts at one-second intervals"))
      return false;
  }
  const auto expired = queue.poll(start + 5s);
  return check(expired.transmissions.empty() &&
                   expired.expiredSequences.size() == 1 && queue.empty(),
               "expiry wins, no sixth attempt");
}

bool sequenceWrapSamples() {
  ReliableQueue queue;
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  if (!enqueue(queue, maximum) || !enqueue(queue, 1) ||
      queue.poll(start).transmissions.size() != 2 ||
      !queue.recordSend(maximum, 1, start + 20ms, true) ||
      !queue.recordSend(1, 1, start + 10ms, true))
    return false;
  const auto ack = queue.acknowledge(1, 2, start + 100ms);
  return check(ack.removed == 2 && ack.sample == 80ms && ack.coalesced == 1,
               "wrap: select timestamp, not largest sequence");
}

bool delayedAckAndSampleStarvation() {
  ReliableQueue queue;
  if (!enqueue(queue, 1) || queue.poll(start).transmissions.size() != 1 ||
      !queue.recordSend(1, 1, start, true) ||
      queue.poll(start + 1000ms).transmissions.size() != 1)
    return false;
  if (!check(!queue.recordSend(1, 1, start + 1000ms, true),
             "late attempt-one feedback rejected"))
    return false;
  auto ack = queue.acknowledge(1, 0, start + 1200ms);
  RttEstimator estimator;
  if (ack.sample && !estimator.observe(*ack.sample))
    return false;
  if (!check(ack.retransmitted == 1 && estimator.snapshot().samples == 0 &&
                 estimator.snapshot().rto == 200ms,
             "RTT above bootstrap remains unmeasured"))
    return false;
  if (!enqueue(queue, 2) || queue.poll(start).transmissions.size() != 1 ||
      !queue.recordSend(2, 1, start, true) || !enqueue(queue, 3) ||
      queue.poll(start + 20ms).transmissions.size() != 1 ||
      !queue.recordSend(3, 1, start + 20ms, true))
    return false;
  ack = queue.acknowledge(3, 0, start + 120ms);
  if (!check(ack.sample == 100ms && ack.removed == 1 && queue.contains(2),
             "selective ACK"))
    return false;
  ack = queue.acknowledge(2, 0, start + 400ms);
  return check(ack.sample == 400ms && queue.empty(),
               "reordered older ACK can sample unsent-again packet");
}

bool bindingOwnsEstimator() {
  RudpBindingRegistry bindings;
  RudpEndpoint endpoint{.address = {}, .port = 42000, .scopeId = 0};
  const auto bind = [&](std::uint64_t generation) {
    auto cap = bindings.requestCapability(1, generation, start);
    if (!cap)
      return RudpBindResult{};
    return bindings.bind(RudpHeader{.flag = RudpFlag::Reliable,
                                    .sessionId = 1,
                                    .sessionGeneration = generation,
                                    .transportEpoch = 0,
                                    .sequence = 1,
                                    .ack = 0,
                                    .ackBits = 0,
                                    .messageId = 22},
                         RudpBindHello{.capability = *cap}, endpoint, start);
  };
  auto first = bind(1);
  if (first.status != RudpBindStatus::Accepted ||
      !bindings.recordRtt(1, 1, first.transportEpoch, endpoint, 100ms))
    return false;
  auto estimate = bindings.rtt(1, 1, first.transportEpoch, endpoint);
  if (!check(estimate && estimate->samples == 1 && estimate->rto == 300ms,
             "binding keeps estimate without a queue"))
    return false;
  auto wrongEndpoint = endpoint;
  ++wrongEndpoint.port;
  if (!check(
          !bindings.recordRtt(1, 1, first.transportEpoch, wrongEndpoint, 10ms),
          "wrong endpoint cannot train"))
    return false;
  const auto peerCapability = bindings.requestCapability(2, 1, start);
  if (!peerCapability)
    return false;
  const auto peer = bindings.bind(RudpHeader{.flag = RudpFlag::Reliable,
                                             .sessionId = 2,
                                             .sessionGeneration = 1,
                                             .transportEpoch = 0,
                                             .sequence = 1,
                                             .ack = 0,
                                             .ackBits = 0,
                                             .messageId = 22},
                                  RudpBindHello{.capability = *peerCapability},
                                  wrongEndpoint, start);
  if (peer.status != RudpBindStatus::Accepted ||
      !bindings.recordRtt(2, 1, peer.transportEpoch, wrongEndpoint, 400ms) ||
      bindings.rtt(1, 1, first.transportEpoch, endpoint)->rto != 300ms ||
      bindings.rtt(2, 1, peer.transportEpoch, wrongEndpoint)->rto != 1000ms)
    return false;
  auto rebound = bind(1);
  estimate = bindings.rtt(1, 1, rebound.transportEpoch, endpoint);
  if (!check(
          estimate && estimate->samples == 0 &&
              rebound.transportEpoch != first.transportEpoch &&
              !bindings.recordRtt(1, 1, first.transportEpoch, endpoint, 10ms),
          "same-generation epoch rebind resets estimate"))
    return false;
  auto second = bind(2);
  estimate = bindings.rtt(1, 2, second.transportEpoch, endpoint);
  if (!check(
          estimate && estimate->samples == 0 && estimate->rto == 200ms &&
              !bindings.recordRtt(1, 1, first.transportEpoch, endpoint, 10ms),
          "replacement resets and stale feedback rejected"))
    return false;
  static_cast<void>(bindings.expireTimedOut(start + 5s));
  return check(bindings.rttSnapshots().empty(), "expiry removes estimator");
}

bool bindingRecovery() {
  RudpBindingRegistry bindings;
  RudpEndpoint endpoint{.address = {}, .port = 42000, .scopeId = 0};
  const auto bindPeer = [&](std::uint64_t id) {
    const auto cap = bindings.requestCapability(id, 1, start);
    return bindings.bind(RudpHeader{.flag = RudpFlag::Reliable,
                                    .sessionId = id,
                                    .sessionGeneration = 1,
                                    .transportEpoch = 0,
                                    .sequence = 1,
                                    .ack = 0,
                                    .ackBits = 0,
                                    .messageId = 22},
                         RudpBindHello{.capability = *cap}, endpoint, start);
  };
  const auto bound = bindPeer(1);
  const auto other = bindPeer(2);
  const auto policy = [&] {
    return *bindings.rtoPolicy(1, 1, bound.transportEpoch, endpoint);
  };
  const auto feedback = [&](ReliablePollResult::Timeout t) {
    return bindings.recordTimeout(1, 1, bound.transportEpoch, endpoint,
                                  t.revision, t.interval);
  };
  if (!bindings.recordRtt(1, 1, bound.transportEpoch, endpoint, 60ms))
    return false;
  const auto original = policy();
  ReliableQueue queue;
  for (std::uint32_t i = 1; i <= 3; ++i)
    if (!enqueue(queue, i))
      return false;
  static_cast<void>(queue.poll(start, original.initialRto, original.revision));
  for (std::uint32_t i = 1; i <= 3; ++i)
    if (!queue.recordSend(i, 1, start, true))
      return false;
  const auto timedOut =
      queue.poll(start + 200ms, original.initialRto, original.revision);
  if (!check(timedOut.timeouts.size() == 3 && feedback(timedOut.timeouts[0]),
             "timeout feedback"))
    return false;
  if (!check(!feedback(timedOut.timeouts[1]) &&
                 !feedback(timedOut.timeouts[2]) &&
                 policy().initialRto == 400ms,
             "same-flight burst increases only once"))
    return false;
  if (!check(bindings.rtoPolicy(2, 1, other.transportEpoch, endpoint)
                     ->initialRto == 200ms,
             "recovery is isolated per binding"))
    return false;
  const auto recovery = policy();
  if (!bindings.recordRtt(1, 1, bound.transportEpoch, endpoint, 60ms,
                          original.revision) ||
      !check(policy().recoveryFloor == 400ms,
             "old valid sample cannot clear recovery"))
    return false;
  const auto oldAck = queue.acknowledge(3, 3, start + 300ms);
  if (!check(oldAck.retransmitted == 3 && !oldAck.sample && queue.empty(),
             "Karn still removes ACKs"))
    return false;
  // A fresh packet after an empty queue can now observe a 300ms path without
  // retry.
  if (!enqueue(queue, 4))
    return false;
  static_cast<void>(
      queue.poll(start + 350ms, recovery.initialRto, recovery.revision));
  if (!queue.recordSend(4, 1, start + 350ms, true) ||
      !queue.poll(start + 550ms, 200ms).transmissions.empty())
    return false;
  const auto fresh = queue.acknowledge(4, 0, start + 650ms);
  if (!check(fresh.sample == 300ms && fresh.sampleRevision == recovery.revision,
             "fresh 300ms RTT is measurable after recovery"))
    return false;
  if (!bindings.recordRtt(1, 1, bound.transportEpoch, endpoint, *fresh.sample,
                          fresh.sampleRevision) ||
      !check(policy().recoveryFloor == 0ms && policy().initialRto >= 300ms,
             "fresh sample resets floor and updates estimator") ||
      !check(!feedback({recovery.revision, 400ms}) &&
                 policy().recoveryFloor == 0ms,
             "late timeout cannot reinstate old recovery"))
    return false;
  // Additional loss can raise the floor only for a current-policy flight, up to
  // the cap.
  for (const auto interval : {200ms, 400ms, 800ms}) {
    const auto current = policy();
    if (!feedback({current.revision, interval}))
      return false;
  }
  if (!check(policy().initialRto == 1000ms &&
                 !feedback({policy().revision, 1000ms}),
             "cap does not grow on silence"))
    return false;
  const auto capped = policy();
  if (!bindings.recordRtt(1, 1, bound.transportEpoch, endpoint, 800ms,
                          capped.revision))
    return false;
  for (int i = 0; i < 80; ++i)
    if (!bindings.recordRtt(1, 1, bound.transportEpoch, endpoint, 60ms,
                            policy().revision))
      return false;
  if (!check(policy().initialRto == 200ms,
             "recovered fast path converges back to minimum"))
    return false;
  const auto replaced = bindPeer(1);
  if (!check(!feedback({capped.revision, 200ms}),
             "replaced identity rejects timeout"))
    return false;
  return check(bindings.rtoPolicy(1, 1, replaced.transportEpoch, endpoint)
                           ->recoveryFloor == 0ms &&
                   bindings.rtoPolicy(1, 1, replaced.transportEpoch, endpoint)
                           ->initialRto == 200ms,
               "rebind discards all recovery state");
}

bool timeoutRequiresSuccessfulSend() {
  for (int mode = 0; mode < 3; ++mode) {
    ReliableQueue q;
    if (!enqueue(q, 1))
      return false;
    static_cast<void>(q.poll(start, 200ms, 7));
    if (mode != 0 && !q.recordSend(1, 1, start, mode == 2))
      return false;
    const auto retry = q.poll(start + 200ms, 200ms, 8);
    if (!check(retry.timeouts.size() == (mode == 2 ? 1u : 0u),
               "only successful prior attempt yields timeout"))
      return false;
    if (mode == 2 && !check(retry.timeouts[0].interval == 200ms &&
                                retry.timeouts[0].revision == 7,
                            "timeout uses expired interval and frozen policy, "
                            "not doubled next interval"))
      return false;
    if (!q.recordSend(1, 2, start + 200ms, false))
      return false;
    if (!check(q.poll(start + 600ms).timeouts.empty(),
               "failed retry is not a path timeout"))
      return false;
    if (!check(q.poll(start + 5s).timeouts.empty() && q.empty(),
               "expiry precedes recovery"))
      return false;
  }
  // Wire wrap does not change the local policy revision or latest-sample
  // selection.
  ReliableQueue q;
  if (!enqueue(q, std::numeric_limits<std::uint32_t>::max()) || !enqueue(q, 1))
    return false;
  static_cast<void>(q.poll(start, 400ms, 9));
  if (!q.recordSend(std::numeric_limits<std::uint32_t>::max(), 1, start,
                    true) ||
      !q.recordSend(1, 1, start + 1ms, true))
    return false;
  const auto ack = q.acknowledge(1, 2, start + 301ms);
  return check(ack.removed == 2 && ack.sampleRevision == 9 &&
                   ack.sample == 300ms && ack.coalesced == 1,
               "wrap/coalescing preserves selected revision");
}

int main() {
  return bindingRecovery() && timeoutRequiresSuccessfulSend() &&
                 estimatorTracksPath() && queueSamplesAndBackoff() &&
                 failedSendAndExpiry() && sequenceWrapSamples() &&
                 delayedAckAndSampleStarvation() && bindingOwnsEstimator()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
