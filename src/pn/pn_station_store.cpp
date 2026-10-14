#include "pn_station_store.hpp"

#include <cstdio>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace softpn {
namespace {

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

bool StationStore::load(StationData& out) const
{
    std::ifstream f(path_);
    if (!f)
        return false;
    StationData d;
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (section != "station" || eq == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (key == "name")
            d.name = value;
        else if (key == "ipSet")
            d.ipSet = value == "1";
    }
    out = d;
    return true;
}

bool StationStore::save(const StationData& data, std::string* error) const
{
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            if (error)
                *error = "cannot write " + tmp;
            return false;
        }
        f << "; written by softpn (PROFINET device): values set by the IO-Controller over DCP\n";
        f << "[station]\n";
        f << "name=" << data.name << "\n";
        f << "ipSet=" << (data.ipSet ? 1 : 0) << "\n";
        if (!f.flush()) {
            if (error)
                *error = "write failed: " + tmp;
            return false;
        }
    }
#ifdef _WIN32
    const bool ok = MoveFileExA(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    const bool ok = std::rename(tmp.c_str(), path_.c_str()) == 0;
#endif
    if (!ok && error)
        *error = "cannot replace " + path_;
    return ok;
}

} // namespace softpn
