# Parallel Traveling Salesperson Problem experiments in C++

This project compares FACO baseline with direct indexing and merged updates (A+B),
and normal parallel ILS with focused repair and selective sharing.

- [Project report](docs/Parallel_TSP_Project_Report.pdf)
- [Base research paper](papers/FACO_base_research_paper.pdf)
- [Colab notebook](https://colab.research.google.com/github/aashish982k/parallel-tsp-faco-ils/blob/main/parallel_tsp_colab.ipynb)
- [Recorded local validation](cpp_conversion_validation.json)

Open `parallel_tsp_colab.ipynb` in a CPU Colab runtime and run its cells in order.
All search, validation, experiment control, timing and result comparison code is
C++17. Notebook cells only write source files or run Bash setup/build commands.
No Python application code, packages or source-string patching is included.
The notebook retains Colab's usual kernel metadata so it opens normally.

## Files and execution

- `settings.hpp`: instance, threads, shared seeds, ants and search budgets.
- `faco.cpp`: Focused Ant Colony Optimization (FACO), baseline and combined A+B.
- `focused_sharing.cpp`: Iterated Local Search (ILS), normal and repair/sharing modes.
- `experiment_support.hpp`: process execution, independent route checks and summaries.
- `experiments.cpp`: the two comparisons and correctness checks.
- `setup.sh`, `build.sh`: download the pinned dependency and compile the programs.

On Linux with GCC, Open Multi-Processing (OpenMP), curl and tar:

```bash
git clone https://github.com/aashish982k/parallel-tsp-faco-ils.git
cd parallel-tsp-faco-ils
bash setup.sh
bash build.sh
./experiments faco
./experiments check
./experiments ils
```

Run from the project directory. Change `settings.hpp` and rebuild to change settings.
Append `--smoke` to an experiment command for two seeds, at most two threads,
300 FACO iterations and two seconds per ILS search. Fixed-round checks still use
1,000 rounds. These short runs validate integration; they are not research evidence.

## Comparisons

**FACO baseline versus A+B only:** one source contains both versions of
`run_focused_aco`, selected by a C++ template. A uses aligned candidate/pheromone
slots to reduce score-preparation work from O(n k squared) to O(n k). B evaporates,
reinforces and computes the next score in one parallel candidate traversal. Initial
scores are prepared before iteration zero. Every city row has one writer; barriers
protect shared tours and scores. The baseline retains separate passes and lookup.
There is no standalone A experiment or MAX-MIN Ant System executable.

Paired FACO runs have identical seeds, ants, iterations and threads. Their order is
shuffled deterministically. The runner independently validates routes and requires
matching final routes and quality traces. Solver and complete process times are
reported separately. Speedup is baseline time divided by A+B time: above one means
faster. Candidate preprocessing is outside solver time but inside process time.
For optional internal-state debugging, compile FACO with `-Dcheck_state` instead of
`-DNDEBUG`; the runner also compares state hashes. Instrumented timings must not
be treated as release performance results.

**Normal ILS versus ILS + focused repair + selective sharing:** both use eight
parallel trajectories, the same seeds, threads, initialization, perturbation and
time budget. Normal ILS scans all cities in candidate-based 2-opt refinement and
never shares tours. The enhanced method starts repair at changed-edge endpoints
and selectively copies the best tour to one stalled trajectory. Normal ILS here
is a parallel independent-trajectory baseline, not a single sequential trajectory.

The ILS table reports mean/best tour length, solver time and complete process time.
Paired percentage improvements are positive when the enhanced method finds shorter
tours. Equal-time experiments assess solution quality, not time to the same solution.
Both methods can finish an active refinement after the cutoff, but late results are
discarded. Preprocessing is inside the ILS budget. Focused repair can miss improvements
and sharing can reduce diversity; improvement is not guaranteed. FACO and ILS tables
are separate experiments.

## Instances and output

An instance is one set of cities with a specified distance rule. `pr2392` is the
TSPLIB instance containing 2,392 cities. The pinned dependency also bundles
`d15112` (15,112 cities) and `Tnm30001` (30,001 cities). Only symmetric rounded
Euclidean two-dimensional (`EUC_2D`) distances and ordered city IDs are accepted.
FACO needs at least 81 cities with these candidate settings; ILS needs at least 24.
Larger instances require longer budgets and more preprocessing memory/time.

Each invocation creates `results/<suite>_<timestamp>` with settings, raw JSON, logs
and CSV tables. Download results through Colab's Files sidebar; the final notebook
cell optionally archives them. Historical outputs are backed up locally, not shown
as results from the new code. Automatic Drive mounting, graphs, patch templates,
standalone A and the external Lin-Kernighan-Helsgaun (LKH) workflow were removed.

## Attribution and validation

FACO and shared parser/2-opt/3-opt helpers derive from R. Skinderowicz's
[FocusedACO](https://github.com/RSkinderowicz/FocusedACO), pinned at
`05b7c1b8d52b27d121b6138222f986fcbf196863`.
The FACO source retains its original author and paper attribution:
*Improving Ant Colony Optimization efficiency for solving large TSP instances*,
Applied Soft Computing (2022), DOI `10.1016/j.asoc.2022.108653`.
The general ILS framework is described by Lourenco, Martin and Stutzle,
[Iterated Local Search](https://arxiv.org/abs/math/0102188).
Repair/sharing is an experimental modification, not a proven new method.
Third-party source and license notices remain intact under `deps/faco`.

Project source uses `<bits/stdc++.h>`, `using namespace std;`, and lowercase
variable names. Platform macros and upstream APIs retain required names.
Build scripts target Linux/Colab; the C++ process helper also supports Windows
for local validation. See `cpp_conversion_validation.json` for checks actually run.
No GPU implementation or optimality certificate is provided.
