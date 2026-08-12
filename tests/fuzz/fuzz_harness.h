// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file fuzz_harness.h
/// @brief Deterministic, MSVC-native mutation-fuzz core for first-party parsers.
///
/// The campaign's measured gap (docs/CODEX_REVIEW_5_REMEDIATION.md, G14) is that
/// every parser here consumes attacker-supplied bytes and none is fuzzed, while
/// the toolchain has no libFuzzer/AFL on MSVC. This header is the reusable answer:
/// a seed corpus is expanded by byte mutations under a fixed-seed splitmix64 PRNG,
/// and each produced input is handed to a Target invariant. Because the PRNG is
/// seeded from a constant (never the wall clock or std::random_device), a failing
/// iteration is reproducible byte-for-byte -- the harness is a regression gate, not
/// a flaky. A nightly can widen it via SAK_FUZZ_ITERS / SAK_FUZZ_SEED without any
/// code change; the default run is short enough to sit in the normal ctest suite.
///
/// The Target contract is deliberately fail-closed: it returns an empty QString
/// when the invariant holds and a human-readable reason otherwise. The runner stops
/// at the first violation and hands back the exact bytes, so the test can print a
/// hex reproducer. "No crash / no hang" is asserted implicitly -- a target that
/// faults or spins never returns the empty string, and a hang trips the ctest
/// timeout with the seeds recorded here.

#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

namespace sak::fuzz {

// splitmix64 mixing constants (Steele/Lea). Named so the algorithm reads clearly
// and so a future --include-tests magic-number pass finds nothing bare.
inline constexpr uint64_t kSplitMixGamma = 0x9E'37'79'B9'7F'4A'7C'15ULL;
inline constexpr uint64_t kSplitMixMultiplierA = 0xBF'58'47'6D'1C'E4'E5'B9ULL;
inline constexpr uint64_t kSplitMixMultiplierB = 0x94'D0'49'BB'13'31'11'EBULL;
inline constexpr int kSplitMixShift1 = 30;
inline constexpr int kSplitMixShift2 = 27;
inline constexpr int kSplitMixShift3 = 31;

/// Deterministic splitmix64 PRNG. Reproducible from its seed alone.
class Prng {
public:
    explicit Prng(uint64_t seed) : m_state(seed) {}

    uint64_t next() {
        m_state += kSplitMixGamma;
        uint64_t z = m_state;
        z = (z ^ (z >> kSplitMixShift1)) * kSplitMixMultiplierA;
        z = (z ^ (z >> kSplitMixShift2)) * kSplitMixMultiplierB;
        return z ^ (z >> kSplitMixShift3);
    }

    /// Value in [0, bound). Caller guarantees bound > 0.
    uint32_t below(uint32_t bound) { return static_cast<uint32_t>(next() % bound); }

    /// One random byte.
    uint8_t byte() { return static_cast<uint8_t>(next()); }

private:
    uint64_t m_state;
};

/// A fuzz target: return "" when the invariant holds for @p input, else a reason.
using Target = std::function<QString(const QByteArray& input)>;

/// Outcome of a run. ok() true means every seed and every mutant satisfied the
/// invariant; otherwise failing_input holds the exact reproducer.
struct FuzzOutcome {
    bool ok = true;
    int iterations_run = 0;
    QByteArray failing_input;
    QString failure_detail;
};

// Bytes that most often sit on a parser's boundary conditions: empty, max signed
// byte, sign bit, and all-ones. Steering some mutations onto these hits off-by-one
// and signedness bugs far faster than uniform-random bytes would.
inline constexpr uint8_t kInterestingBytes[] = {0x00u, 0x7Fu, 0x80u, 0xFFu};
inline constexpr int kInterestingByteCount = 4;

// How many distinct mutation operators applyOneMutation dispatches over.
inline constexpr uint32_t kMutationOperatorCount = 7;

/// Apply exactly one random mutation operator to @p data in place. Kept tiny and
/// branch-per-operator so the whole harness stays under the CCN gate.
inline void applyOneMutation(QByteArray& data, Prng& prng) {
    // Operators 0..5 all index an existing byte, so they require a non-empty buffer.
    // On an empty buffer only the insert operator is well defined, so force it and
    // never call prng.below(0) (its precondition is bound > 0).
    const uint32_t op = data.isEmpty() ? (kMutationOperatorCount - 1)
                                       : prng.below(kMutationOperatorCount);
    switch (op) {
    case 0: {  // flip one bit
        const int index = static_cast<int>(prng.below(static_cast<uint32_t>(data.size())));
        data[index] = static_cast<char>(data.at(index) ^ (1 << prng.below(8)));
        break;
    }
    case 1: {  // overwrite one byte with a random value
        const int index = static_cast<int>(prng.below(static_cast<uint32_t>(data.size())));
        data[index] = static_cast<char>(prng.byte());
        break;
    }
    case 2: {  // overwrite one byte with a boundary value
        const int index = static_cast<int>(prng.below(static_cast<uint32_t>(data.size())));
        data[index] = static_cast<char>(kInterestingBytes[prng.below(kInterestingByteCount)]);
        break;
    }
    case 3: {  // erase one byte
        data.remove(static_cast<int>(prng.below(static_cast<uint32_t>(data.size()))), 1);
        break;
    }
    case 4: {  // truncate the tail
        data.truncate(static_cast<int>(prng.below(static_cast<uint32_t>(data.size() + 1))));
        break;
    }
    case 5: {  // duplicate a byte (drives length growth toward bomb-shaped input)
        const int index = static_cast<int>(prng.below(static_cast<uint32_t>(data.size())));
        data.insert(index, data.at(index));
        break;
    }
    default: {  // insert a random byte at a random position
        const int index = static_cast<int>(prng.below(static_cast<uint32_t>(data.size() + 1)));
        data.insert(index, static_cast<char>(prng.byte()));
        break;
    }
    }
}

// Mutations stacked per mutant: at least one, at most this many.
inline constexpr uint32_t kMaxMutationsPerRound = 6;

/// Produce one mutant from @p seed by stacking 1..kMaxMutationsPerRound mutations.
inline QByteArray makeMutant(const QByteArray& seed, Prng& prng) {
    QByteArray out = seed;
    const uint32_t rounds = 1u + prng.below(kMaxMutationsPerRound);
    for (uint32_t i = 0; i < rounds; ++i) {
        applyOneMutation(out, prng);
    }
    return out;
}

/// Every seed must satisfy the invariant unmutated -- a corpus that fails clean is
/// a bug in the corpus or the target, reported before any mutation muddies it.
inline FuzzOutcome checkSeeds(const std::vector<QByteArray>& corpus, const Target& target) {
    FuzzOutcome outcome;
    for (const QByteArray& seed : corpus) {
        ++outcome.iterations_run;
        const QString detail = target(seed);
        if (!detail.isEmpty()) {
            outcome.ok = false;
            outcome.failing_input = seed;
            outcome.failure_detail = QStringLiteral("seed corpus entry: ") + detail;
            return outcome;
        }
    }
    return outcome;
}

/// Run @p iterations mutation rounds over @p corpus with the given @p seed. Seeds
/// are validated first, then mutants are generated deterministically and checked.
/// Stops and returns at the first invariant violation.
inline FuzzOutcome run(const std::vector<QByteArray>& corpus,
                       const Target& target,
                       int iterations,
                       uint64_t seed) {
    if (corpus.empty()) {
        return FuzzOutcome{false, 0, QByteArray(), QStringLiteral("empty fuzz corpus")};
    }
    FuzzOutcome outcome = checkSeeds(corpus, target);
    if (!outcome.ok) {
        return outcome;
    }
    Prng prng(seed);
    const auto corpus_size = static_cast<uint32_t>(corpus.size());
    for (int i = 0; i < iterations; ++i) {
        const QByteArray& base = corpus[prng.below(corpus_size)];
        const QByteArray mutant = makeMutant(base, prng);
        ++outcome.iterations_run;
        const QString detail = target(mutant);
        if (!detail.isEmpty()) {
            outcome.ok = false;
            outcome.failing_input = mutant;
            outcome.failure_detail = detail;
            return outcome;
        }
    }
    return outcome;
}

/// Hex reproducer, capped so a huge mutant does not flood the test log.
inline constexpr int kReproducerHexCap = 512;
inline QString reproducerHex(const QByteArray& input) {
    const QByteArray head = input.left(kReproducerHexCap);
    QString hex = QString::fromLatin1(head.toHex());
    if (input.size() > kReproducerHexCap) {
        hex += QStringLiteral("... (%1 bytes total)").arg(input.size());
    }
    return hex;
}

// Default short run for the in-suite gate; a nightly widens it via the env vars.
inline constexpr int kDefaultIterations = 2000;
inline constexpr uint64_t kDefaultSeed = 0xC0'FF'EEULL;

/// Iteration count, overridable by SAK_FUZZ_ITERS for a longer campaign.
inline int iterationsFromEnv() {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("SAK_FUZZ_ITERS", &ok);
    return (ok && value > 0) ? value : kDefaultIterations;
}

/// PRNG seed, overridable by SAK_FUZZ_SEED so a nightly explores fresh inputs.
inline uint64_t seedFromEnv() {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("SAK_FUZZ_SEED", &ok);
    return ok ? static_cast<uint64_t>(static_cast<unsigned>(value)) : kDefaultSeed;
}

}  // namespace sak::fuzz
