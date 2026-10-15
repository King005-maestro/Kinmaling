#pragma once
#include <sched.h>
#include <vector>
std::vector<int> bigCores();
cpu_set_t bigCoreMask();
