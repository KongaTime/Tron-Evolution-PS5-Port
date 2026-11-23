// Reads an Xbox 360 disc image (XDVDFS) in place. From the Dante's Inferno PS5
// port (ps5/launcher), adapted from KongaTime/The-Darkness-Recomp-PS5.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace tron::ps5 {
struct IsoEntry {
    std::filesystem::path path;
    uint64_t offset = 0, size = 0;
    bool directory = false;
};
struct IsoPlan {
    std::filesystem::path source;
    uint64_t sourceSize = 0, bytes = 0;
    uint64_t executableHash = 0;
    std::vector<IsoEntry> entries;
};
using VerifyExecutable = std::function<void(std::span<const uint8_t>)>;
// Read-only and bounded: lists the image and hands default.xex to `verify`
// (which throws to reject it).
IsoPlan inspectGameIso(const std::filesystem::path&, const VerifyExecutable&);
// The .iso files directly inside `roots`, sorted.
std::vector<std::filesystem::path> findGameIsos(const std::vector<std::filesystem::path>& roots);
}
