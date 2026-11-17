/**
 * This is an implementation of the Focused Ant Colony Optimization (FACO) for
 * solving large TSP instances as described in the paper:
 *
 * R. Skinderowicz,
 * Improving Ant Colony Optimization efficiency for solving large TSP instances,
 * Applied Soft Computing, 2022, 108653, ISSN 1568-4946,
 * https://doi.org/10.1016/j.asoc.2022.108653.
 *
 * @author: Rafał Skinderowicz (rafal.skinderowicz@us.edu.pl)
 */
#include <bits/stdc++.h>
#include <omp.h>

#include "problem_instance.h"
#include "ant.h"
#include "pheromone.h"
#include "local_search.h"
#include "utils.h"
#include "rand.h"
#include "progargs.h"
#include "json.hpp"
#include "logging.h"

using namespace std;

namespace fs = filesystem;

struct HeuristicData {
    const ProblemInstance &problem_;
    double beta_;

    HeuristicData(const ProblemInstance &instance, double beta)
        : problem_(instance), beta_(beta) {}

    [[nodiscard]] double get(uint32_t from, uint32_t to) const {
        auto d = problem_.get_distance(from, to);
        return (d > 0) ? 1. / pow(d, beta_) : 1;
    }

    [[nodiscard]] uint32_t
    find_node_with_max_value(uint32_t from, const vector<uint32_t> &nodes) const {
        assert(beta_ > 0);

        auto result = from;
        auto min_dist = numeric_limits<double>::max();
        for (auto node : nodes) {
            auto dist = problem_.get_distance(from, node);
            if (dist < min_dist) {
                min_dist = dist;
                result = node;
            }
        }
        return result;
    }
};

uint32_t select_max_product_node(uint32_t current_node, Ant &ant,
                                 const CandListPheromone & /*pheromone*/,
                                 const HeuristicData &heuristic) {

    assert(ant.get_unvisited_count() > 0);
    // We are assuming that all nodes on the cand list of the current_node
    // have been visited and thus we do not need to look for pheromone values
    // as all the other edges have the same - default - value
    return heuristic.find_node_with_max_value(current_node, ant.get_unvisited_nodes());
}

static const uint32_t MaxCandListSize = 32;

struct Limits {
    double min_ = 0;
    double max_ = 0;
};

/**
 * This is based on Eq. 11 from the original MAX-MIN paper:
 *
 * Stützle, Thomas, and Holger H. Hoos. "MAX–MIN ant system." Future generation
 * computer systems 16.8 (2000): 889-914.
 */
Limits calc_trail_limits(uint32_t dimension, uint32_t /*cand_list_size*/, double p_best,
                         double rho, double solution_cost) {
    const auto tau_max = 1 / (solution_cost * (1. - rho));
    const auto cand_count = dimension;
    const auto avg = cand_count / 2.;
    const auto p = pow(p_best, 1. / cand_count);
    const auto tau_min = min(tau_max, tau_max * (1 - p) / ((avg - 1) * p));
    return {tau_min, tau_max};
}

/**
 * This is a modified version of the original trail initialization method
 * used in the FACO
 */
Limits calc_trail_limits_cl(uint32_t /*dimension*/, uint32_t cand_list_size,
                            double p_best, double rho, double solution_cost) {
    const auto tau_max = 1 / (solution_cost * (1. - rho));
    const auto avg = cand_list_size; // This is far smaller than dimension/2
    const auto p = pow(p_best, 1. / avg);
    const auto tau_min = min(tau_max, tau_max * (1 - p) / ((avg - 1) * p));
    return {tau_min, tau_max};
}

typedef Limits (*calc_trail_limits_fn_t)(uint32_t dimension,
                                         uint32_t /*cand_list_size*/, double p_best,
                                         double rho, double solution_cost);

template <typename Pheromone_t>
uint32_t select_next_node(const Pheromone_t &pheromone, const HeuristicData &heuristic,
                          const NodeList &nn_list,
                          const vector<double> &nn_product_cache,
                          const NodeList &backup_nn_list, Ant &ant) {
    assert(!ant.route_.empty());

    const auto current_node = ant.get_current_node();
    assert(nn_list.size() <= ::MaxCandListSize);

    // A list of the nearest unvisited neighbors of current_node, i.e. so
    // called "candidates list", or "cl" in short
    uint32_t cl[::MaxCandListSize];
    uint32_t cl_size = 0;

    // In the MMAS the local pheromone evaporation is absent thus for each ant
    // the product of the pheromone trail and the heuristic will be the same
    // and we can pre-load it into nn_product_cache
    auto nn_product_cache_it =
        nn_product_cache.begin() + static_cast<uint32_t>(current_node * nn_list.size());

    double cl_product_prefix_sums[::MaxCandListSize];
    double cl_products_sum = 0;
    double max_prod = 0;
    uint32_t max_node = current_node;
    for (auto node : nn_list) {
        uint32_t valid = 1 - ant.is_visited(node);
        cl[cl_size] = node;
        auto prod = *nn_product_cache_it * valid;
        cl_products_sum += prod;
        cl_product_prefix_sums[cl_size] = cl_products_sum;
        cl_size += valid;
        ++nn_product_cache_it;
        if (max_prod < prod) {
            max_prod = prod;
            max_node = node;
        }
    }

    uint32_t chosen_node = max_node;

    if (cl_size > 1) { // Select from the closest nodes
        // The following could be done using binary search in O(log(cl_size))
        // time but should not matter for small values of cl_size
        chosen_node = cl[cl_size - 1];
        const auto r = get_rng().next_float() * cl_products_sum;
        for (uint32_t i = 0; i < cl_size; ++i) {
            if (r < cl_product_prefix_sums[i]) {
                chosen_node = cl[i];
                break;
            }
        }
    } else if (cl_size == 0) { // Select from the rest of the unvisited nodes the one
                               // with the maximum product of pheromone and heuristic
        for (auto node : backup_nn_list) {
            if (!ant.is_visited(node)) {
                chosen_node = node;
                break;
            }
        }
        if (chosen_node == max_node) { // Still nothing selected
            chosen_node =
                select_max_product_node(current_node, ant, pheromone, heuristic);
        }
    }
    assert(chosen_node != current_node);
    return chosen_node;
}

/**
 * This wraps problem instance and pheromone memory and provides convenient
 * methods to manipulate it.
 */
template <class Impl> class ACOModel {
  protected:
    const ProblemInstance &problem_;
    double p_best_;
    double rho_;
    uint32_t cand_list_size_;

  public:
    Limits trail_limits_;

    calc_trail_limits_fn_t calc_trail_limits_ = calc_trail_limits;

    ACOModel(const ProblemInstance &problem, const ProgramOptions &options)
        : problem_(problem), p_best_(options.p_best_), rho_(options.rho_),
          cand_list_size_(options.cand_list_size_) {}

    void init(double solution_cost) {
        update_trail_limits(solution_cost);
        static_cast<Impl *>(this)->init_impl();
    }

    void update_trail_limits(double solution_cost) {
        trail_limits_ = calc_trail_limits_(problem_.dimension_, cand_list_size_,
                                           p_best_, rho_, solution_cost);
    }

    void evaporate_pheromone() {
        get_pheromone().evaporate(1 - rho_, trail_limits_.min_);
    }

    decltype(auto) get_pheromone() {
        return static_cast<Impl *>(this)->get_pheromone_impl();
    }

    // Increases amount of pheromone on trails corresponding edges of the
    // given solution (sol). Returns deposited amount.
    double deposit_pheromone(const Ant &sol) {
        const double deposit = 1.0 / sol.cost_;
        auto prev_node = sol.route_.back();
        auto &pheromone = get_pheromone();
        for (auto node : sol.route_) {
            // The global update of the pheromone trails
            pheromone.increase(prev_node, node, deposit, trail_limits_.max_);
            prev_node = node;
        }
        return deposit;
    }
};

class CandListModel : public ACOModel<CandListModel> {
    unique_ptr<CandListPheromone> pheromone_ = nullptr;

  public:
    CandListModel(const ProblemInstance &problem, const ProgramOptions &options)
        : ACOModel(problem, options) {}

    CandListPheromone &get_pheromone_impl() { return *pheromone_; }

    void init_impl() {
        pheromone_ =
            make_unique<CandListPheromone>(problem_.get_nn_lists(cand_list_size_),
                                           trail_limits_.max_, problem_.is_symmetric_);
    }
};

vector<vector<uint32_t>> par_build_initial_routes(const ProblemInstance &problem,
                                                  bool use_local_search,
                                                  uint32_t sol_count = 0) {
    uint32_t nn_count = 16;

    if (sol_count == 0) {
#pragma omp parallel default(none) shared(sol_count)
#pragma omp master
        sol_count = omp_get_num_procs();
    }

    vector<vector<uint32_t>> routes(sol_count);

    for (uint32_t i = 0; i < sol_count; ++i) {
        auto start_node = get_rng().next_uint32(problem.dimension_);
        routes[i] = problem.build_nn_tour(start_node);
    }

    if (use_local_search) {
#pragma omp parallel for default(none) shared(problem, routes, nn_count, sol_count)
        for (uint32_t i = 0; i < sol_count; ++i) {
            two_opt_nn(problem, routes[i], true, nn_count);
            three_opt_nn(problem, routes[i], /*use_dont_look_bits*/ true, nn_count);
        }
    }
    return routes;
}

template <bool use_combined_optimizations, typename ComputationsLog_t>
unique_ptr<Solution> run_focused_aco(const ProblemInstance &problem,
                                     const ProgramOptions &opt,
                                     ComputationsLog_t &comp_log) {

    const auto dimension = problem.dimension_;
    const auto cl_size = opt.cand_list_size_;
    const auto bl_size = opt.backup_list_size_;
    const auto ants_count = opt.ants_count_;
    const auto iterations = opt.iterations_;
    const auto use_ls = opt.local_search_ != 0;

    Timer start_sol_timer;
    const auto start_routes = par_build_initial_routes(problem, use_ls);
    auto start_sol_count = start_routes.size();
    vector<double> start_costs(start_sol_count);

#pragma omp parallel default(none)                                                     \
    shared(start_sol_count, problem, start_costs, start_routes)
#pragma omp for
    for (size_t i = 0; i < start_sol_count; ++i) {
        start_costs[i] = problem.calculate_route_length(start_routes[i]);
    }
    comp_log("initial solutions build time", start_sol_timer.get_elapsed_seconds());

    auto smallest_pos =
        distance(begin(start_costs), min_element(begin(start_costs), end(start_costs)));
    auto initial_cost = start_costs[smallest_pos];
    const auto &start_route = start_routes[smallest_pos];
    comp_log("initial sol cost", initial_cost);

    HeuristicData heuristic(problem, opt.beta_);
    vector<double> cl_heuristic_cache;

    cl_heuristic_cache.resize(cl_size * dimension);
    for (uint32_t node = 0; node < dimension; ++node) {
        auto cache_it = cl_heuristic_cache.begin() + node * cl_size;

        for (auto &nn : problem.get_nearest_neighbors(node, cl_size)) {
            *cache_it++ = heuristic.get(node, nn);
        }
    }

    // Probabilistic model based on pheromone trails:
    CandListModel model(problem, opt);
    // If the LS is on, the differences between pheromone trails should be
    // smaller -- we use calc_trail_limits_cl instead of calc_trail_limits
    model.calc_trail_limits_ = !use_ls ? calc_trail_limits : calc_trail_limits_cl;
    model.init(initial_cost);
    auto &pheromone = model.get_pheromone();
    pheromone.set_all_trails(model.trail_limits_.max_);

    vector<double> nn_product_cache(dimension * cl_size);

    auto best_ant = make_unique<Ant>(start_route, initial_cost);

    vector<Ant> ants(ants_count);
    Ant *iteration_best = nullptr;

    auto source_solution = make_unique<Solution>(start_route, best_ant->cost_);

    // The following are mainly for reporting purposes
    int64_t select_next_node_calls = 0;
    Trace<ComputationsLog_t, SolutionCost> best_cost_trace(comp_log, "best sol cost",
                                                           iterations, 1, true, 1.);
    Trace<ComputationsLog_t, double> select_next_node_calls_trace(
        comp_log, "mean percent of select next node calls", iterations, 20);
    Trace<ComputationsLog_t, double> mean_cost_trace(comp_log, "sol cost mean",
                                                     iterations, 20);
    Trace<ComputationsLog_t, double> stdev_cost_trace(comp_log, "sol cost stdev",
                                                      iterations, 20);
    Timer main_timer;

    vector<double> sol_costs(ants_count);

#ifdef check_state
    vector<uint64_t> state_checks;
#endif

#pragma omp parallel default(shared)
    {
        // Endpoints of new edges (not present in source_route) are inserted
        // into ls_checklist and later used to guide local search
        vector<uint32_t> ls_checklist;
        ls_checklist.reserve(dimension);

        for (int32_t iteration = 0; iteration < iterations; ++iteration) {
#pragma omp barrier
#ifdef check_state
#pragma omp master
            {
                uint64_t hash = 1469598103934665603ULL;
                auto mix = [&](uint64_t x) {
                    hash ^= x;
                    hash *= 1099511628211ULL;
                };
                for (double x : pheromone.trails_) {
                    uint64_t bits;
                    memcpy(&bits, &x, sizeof bits);
                    mix(bits);
                }
                for (const auto &ant : ants)
                    for (auto x : ant.route_)
                        mix(x);
                for (auto x : source_solution->route_)
                    mix(x);
                state_checks.push_back(hash);
            }
#pragma omp barrier
#endif

            if constexpr (use_combined_optimizations) {
                if (iteration == 0) {
// Candidate slots share the same order in all three arrays.
#pragma omp for schedule(static)
                    for (size_t candidate_slot = 0;
                         candidate_slot < nn_product_cache.size(); ++candidate_slot) {
                        nn_product_cache[candidate_slot] =
                            cl_heuristic_cache[candidate_slot] *
                            pheromone.trails_[candidate_slot];
                    }
                }

            } else {
// Load pheromone * heuristic for each edge connecting nearest
// neighbors (up to cl_size)
#pragma omp for schedule(static)
                for (uint32_t node = 0; node < dimension; ++node) {
                    auto cache_it = nn_product_cache.begin() + node * cl_size;
                    auto heuristic_it = cl_heuristic_cache.begin() + node * cl_size;
                    for (auto &nn : problem.get_nearest_neighbors(node, cl_size)) {
                        *cache_it++ = *heuristic_it++ * pheromone.get(node, nn);
                    }
                }
            }

#pragma omp master
            select_next_node_calls = 0;

// Changing schedule from "static" to "dynamic" can speed up
// computations a bit, however it introduces non-determinism due to
// threads scheduling. With "static" the computations always follow
// the same path -- i.e. if we run the program with the same PRNG
// seed (--seed X) then we get exactly the same results.
#pragma omp for schedule(static, 1) reduction(+ : select_next_node_calls)
            for (uint32_t ant_idx = 0; ant_idx < ants.size(); ++ant_idx) {
                uint32_t target_new_edges = opt.min_new_edges_;

                auto &ant = ants[ant_idx];
                ant.initialize(dimension);

                auto start_node = get_rng().next_uint32(dimension);
                ant.visit(start_node);

                ls_checklist.clear();
                ls_checklist.push_back(start_node);

                // We are counting edges (undirected) that are not present in
                // the source_route. The factual # of new edges can be +1 as we
                // skip the check for the closing edge (minor optimization).
                uint32_t new_edges = 0;

                while (ant.visited_count_ < dimension) {
                    auto curr = ant.get_current_node();
                    auto next = select_next_node(
                        pheromone, heuristic,
                        problem.get_nearest_neighbors(curr, cl_size), nn_product_cache,
                        problem.get_backup_neighbors(curr, cl_size, bl_size), ant);
                    ant.visit(next);

                    ++select_next_node_calls;

                    if (!source_solution->contains_edge(curr, next)) {
                        ++new_edges;
                        // The endpoint (tail) of the new edge should be
                        // checked by the local search
                        ls_checklist.push_back(next);
                    }

                    // If we have enough new edges, we try to copy "old" edges
                    // from the source_route.
                    if (new_edges >= target_new_edges) {
                        // Forward direction, start at { next, succ(next) }
                        auto it = source_solution->get_iterator(next);
                        while (ant.try_visit(it.goto_succ())) {
                        }
                        // Backward direction
                        it.goto_pred(); // Reverse .goto_succ() from above
                        while (ant.try_visit(it.goto_pred())) {
                        }
                    }
                }
                if (use_ls) {
                    two_opt_nn(problem, ant.route_, ls_checklist,
                               opt.ls_cand_list_size_);
                }

                ant.cost_ = problem.calculate_route_length(ant.route_);
                sol_costs[ant_idx] = ant.cost_;
            }

#pragma omp master
            {
                iteration_best = &ants.front();
                for (auto &ant : ants) {
                    if (ant.cost_ < iteration_best->cost_) {
                        iteration_best = &ant;
                    }
                }
                if (iteration_best->cost_ < best_ant->cost_) {
                    best_ant->update(iteration_best->route_, iteration_best->cost_);

                    auto error = problem.calc_relative_error(best_ant->cost_);
                    best_cost_trace.add({best_ant->cost_, error}, iteration,
                                        main_timer());

                    model.update_trail_limits(best_ant->cost_);
                }

                auto total_edges = (dimension - 1) * ants_count;
                select_next_node_calls_trace.add(
                    round(100.0 * static_cast<double>(select_next_node_calls) /
                              total_edges,
                          2),
                    iteration, main_timer());

                mean_cost_trace.add(round(sample_mean(sol_costs), 1), iteration);
                stdev_cost_trace.add(round(sample_stdev(sol_costs), 1), iteration);
            }

// Synchronize threads before pheromone update
#pragma omp barrier

            if constexpr (use_combined_optimizations) {
#pragma omp master
                {
                    bool use_best_ant =
                        (get_rng().next_float() < opt.gbest_as_source_prob_);
                    auto &update_ant = use_best_ant ? *best_ant : *iteration_best;
                    source_solution->update(update_ant.route_, update_ant.cost_);
                    const double retention_fraction = 1 - (1 - opt.rho_);
                    pheromone.default_pheromone_value_ =
                        max(pheromone.default_pheromone_value_ * retention_fraction,
                            model.trail_limits_.min_);
                }
#pragma omp barrier
// One writer per row: evaporate, reinforce, then prepare scores.
#pragma omp for schedule(static)
                for (uint32_t city = 0; city < dimension; ++city) {
                    const auto successor_city = source_solution->get_succ(city);
                    const auto predecessor_city = source_solution->get_pred(city);
                    const double pheromone_deposit = 1.0 / source_solution->cost_;
                    const double retention_fraction = 1 - (1 - opt.rho_);
                    for (size_t candidate_slot = size_t(city) * cl_size;
                         candidate_slot < size_t(city + 1) * cl_size;
                         ++candidate_slot) {
                        auto updated_pheromone =
                            max(model.trail_limits_.min_,
                                pheromone.trails_[candidate_slot] * retention_fraction);
                        const auto neighbor = pheromone.nodes_[candidate_slot];
                        if (neighbor == successor_city ||
                            neighbor == predecessor_city) {
                            updated_pheromone =
                                min(model.trail_limits_.max_,
                                    pheromone_deposit + updated_pheromone);
                        }
                        pheromone.trails_[candidate_slot] = updated_pheromone;
                        nn_product_cache[candidate_slot] =
                            cl_heuristic_cache[candidate_slot] * updated_pheromone;
                    }
                }
            } else {
                model.evaporate_pheromone();

#pragma omp master
                {
                    bool use_best_ant =
                        (get_rng().next_float() < opt.gbest_as_source_prob_);
                    auto &update_ant = use_best_ant ? *best_ant : *iteration_best;

                    model.deposit_pheromone(update_ant);

                    // Increase pheromone values on the edges of the new
                    // source_solution
                    source_solution->update(update_ant.route_, update_ant.cost_);
                }
            }
        }
    }
#ifdef check_state
    comp_log("state checks", state_checks);
#endif

    return unique_ptr<Solution>(dynamic_cast<Solution *>(best_ant.release()));
}

int main(int argument_count, char **arguments) {
    try {
        if (argument_count != 8)
            throw runtime_error(
                "Usage: faco baseline|combined instance threads ants iterations seed output.json");
        const string variant = arguments[1];
        if (variant != "baseline" && variant != "combined")
            throw runtime_error("Only baseline and A+B combined are supported");
        ProgramOptions options;
        options.threads_ = stoi(arguments[3]);
        options.ants_count_ = stoul(arguments[4]);
        options.iterations_ = stoi(arguments[5]);
        options.seed_ = stoull(arguments[6]);
        if (options.threads_ < 1 || options.threads_ > 64 || options.ants_count_ < 2 ||
            options.iterations_ < 1 || options.seed_ == 0)
            throw runtime_error("Invalid FACO experiment parameters");
        omp_set_dynamic(0);
        omp_set_num_threads(options.threads_);
        init_random_number_generators(options.seed_);
        auto problem = load_tsplib_instance(arguments[2]);
        if (!problem.is_symmetric_ || problem.edge_weight_type_ != EUC_2D ||
            problem.dimension_ < 81)
            throw runtime_error(
                "FACO requires symmetric EUC_2D with at least 81 cities");
        using json = nlohmann::json;
        using Log = ComputationsLog<json>;
        json record, execution;
        Timer preprocessing_timer;
        problem.compute_nn_lists(80);
        record["nn and backup lists calc time"] = preprocessing_timer();
        Log computation_log(execution, cout);
        Timer solver_timer;
        auto result = variant == "baseline"
                          ? run_focused_aco<false>(problem, options, computation_log)
                          : run_focused_aco<true>(problem, options, computation_log);
        const double solver_seconds = solver_timer();
        if (!problem.is_route_valid(result->route_) ||
            problem.calculate_route_length(result->route_) != result->cost_)
            throw runtime_error("FACO returned an invalid tour");
        execution["execution time"] = solver_seconds;
        execution["final route"] = result->route_;
        execution["final cost"] = result->cost_;
        record["variant"] = variant;
        record["executions"] = json::array({execution});
        dump(options, record["args"]);
        ofstream output(arguments[7]);
        output << record.dump(2) << '\n';
        output.close();
        if (!output)
            throw runtime_error("Cannot write FACO result");
        cout << variant << ": cost " << result->cost_ << ", solver seconds "
             << solver_seconds << '\n';
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
