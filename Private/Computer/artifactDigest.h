#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace revia::computer
{

// SHA-256, and the one canonical string this module hashes with it.
//
// Internal to Private/Computer on purpose. It exists for one job -- deciding whether a
// learned artifact still says what it said when it was evaluated -- and a general
// hashing utility in Public would invite uses that want a different set of guarantees.

[[nodiscard]] std::string Sha256Hex(const std::string& input);

// The bytes that decide how an artifact behaves, in a fixed order.
//
// Deliberately not the whole file. Hashing the JSON would mean reproducing one
// language's float formatting in another, which fails on the seventeenth digit of a
// legitimate artifact and calls it tampering -- a check that cries wolf is worse than
// no check, because it gets switched off.
//
// So this covers exactly what changes the decision: the feature contract, the weights,
// the abstention threshold and the qualified scope. `trained_at` and the timing figures
// are not here, because editing them changes nothing about what the artifact does.
// Tools/Computer/train.py builds this same string, with the same %.17g formatting,
// which is shortest-round-trip in both languages.
[[nodiscard]] std::string BehaviourDigestInput(
    std::uint32_t featureVersion,
    const std::vector<std::string>& featureNames,
    const std::vector<double>& weights,
    double abstainBelow,
    const std::vector<std::string>& applications,
    const std::vector<std::string>& intents);

} // namespace revia::computer
