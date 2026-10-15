#include "cpu_topology.h"
#include <algorithm>
#include <cstdio>

std::vector<int> bigCores() {
    std::vector<std::pair<long,int>> v;
    for (int i = 0; i < 16; i++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        FILE* f = fopen(p, "r");
        if (!f) continue;
        long khz = 0;
        if (fscanf(f, "%ld", &khz) != 1) khz = 0;
        fclose(f);
        v.push_back({khz, i});
    }
    std::vector<int> out;
    if (v.empty()) return out;
    long top = std::max_element(v.begin(), v.end())->first;
    long minKhz = top * 70 / 100;
    for (auto& e : v) if (e.first >= minKhz) out.push_back(e.second);
    return out;
}

cpu_set_t bigCoreMask() {
    cpu_set_t s;
    CPU_ZERO(&s);
    auto c = bigCores();
    if (c.empty()) { for (int i = 0; i < 8; i++) CPU_SET(i, &s); }
    for (int i : c) CPU_SET(i, &s);
    return s;
}
