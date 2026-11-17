#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"

source_directory=deps/faco/src
compiler_flags=(-std=c++17 -O3 -march=native -fopenmp -D_USE_MATH_DEFINES
                -include cstdint -include cstring -I "$source_directory")
shared_sources=("$source_directory/problem_instance.cpp"
                "$source_directory/local_search.cpp"
                "$source_directory/utils.cpp"
                "$source_directory/rand.cpp")

g++ "${compiler_flags[@]}" -DNDEBUG faco.cpp "${shared_sources[@]}" -o faco
# Keep assertions enabled for the ILS perturbation/refinement self-test.
g++ "${compiler_flags[@]}" focused_sharing.cpp "${shared_sources[@]}" -o focused_sharing
g++ "${compiler_flags[@]}" experiments.cpp -o experiments
echo "Built the FACO solver, ILS solver and C++ experiment runner."
