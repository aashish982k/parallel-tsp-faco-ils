#pragma once

#include <bits/stdc++.h>
#include <omp.h>

using namespace std;

struct ExperimentSettings {
    string instance_name = "pr2392";
    int thread_count = min(4, omp_get_num_procs());
    vector<int> seeds = {11, 23, 37};
    int faco_iterations = 2000;
    int ant_count = 128;
    double ils_seconds = 10;
    int check_rounds = 1000;
};
