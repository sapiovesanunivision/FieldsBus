// Persistent device identity set over DCP (NameOfStation, "IP set" flag) in a small INI file.
#pragma once

#include <string>

namespace softpn {

struct StationData {
    std::string name;  // "" = unnamed
    bool ipSet = false;
};

class StationStore {
public:
    explicit StationStore(std::string path) : path_(std::move(path)) {}

    // Returns false if the file does not exist or cannot be read (out is left unchanged).
    bool load(StationData& out) const;
    // Atomic replace: writes <path>.tmp, then renames over <path>.
    bool save(const StationData& data, std::string* error = nullptr) const;
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

} // namespace softpn
