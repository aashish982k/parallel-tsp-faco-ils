// Non-ACO experiment; reuses the pinned author's TSP parser and 2/3-opt routines.
#include <bits/stdc++.h>
#include <omp.h>
#include "problem_instance.h"
#include "local_search.h"
#include "rand.h"
#include "json.hpp"

using namespace std;

using Tour = vector<uint32_t>;
using json = nlohmann::json;
constexpr int search_trajectory_count = 8;
constexpr uint32_t candidate_neighbor_count = 20;

Tour nearest_tour(const ProblemInstance &problem_instance, uint32_t start_city) {
    Tour tour;
    vector<char> visited_cities(problem_instance.dimension_, false);
    uint32_t current_city = start_city;
    while (tour.size() < problem_instance.dimension_) {
        tour.push_back(current_city);
        visited_cities[current_city] = true;
        if (tour.size() == problem_instance.dimension_)
            break;
        uint32_t next_city = problem_instance.dimension_;
        for (auto candidate_city : problem_instance.get_nearest_neighbors(
                 current_city, candidate_neighbor_count)) {
            if (!visited_cities[candidate_city]) {
                next_city = candidate_city;
                break;
            }
        }
        if (next_city == problem_instance.dimension_) {
            double nearest_distance = numeric_limits<double>::infinity();
            for (uint32_t candidate_city = 0;
                 candidate_city < problem_instance.dimension_; ++candidate_city) {
                if (!visited_cities[candidate_city] &&
                    problem_instance.get_distance(current_city, candidate_city) <
                        nearest_distance) {
                    next_city = candidate_city;
                    nearest_distance =
                        problem_instance.get_distance(current_city, candidate_city);
                }
            }
        }
        current_city = next_city;
    }
    return tour;
}

void kick(Tour &tour, Random &random_generator, Tour &affected_cities) {
    const uint32_t city_count = tour.size(), segment_span = city_count / 4;
    const uint32_t rotation_offset = random_generator.next_uint32(city_count);
    rotate(tour.begin(), tour.begin() + rotation_offset, tour.end());
    const uint32_t first_cut_position = 1 + random_generator.next_uint32(segment_span);
    const uint32_t second_cut_position =
        first_cut_position + 1 + random_generator.next_uint32(segment_span);
    const uint32_t third_cut_position =
        second_cut_position + 1 + random_generator.next_uint32(segment_span);
    Tour perturbed_tour;
    perturbed_tour.reserve(city_count);
    // A B C D -> A D C B changes four boundary connections, retaining interiors.
    for (auto segment :
         {pair{0u, first_cut_position}, pair{third_cut_position, city_count},
          pair{second_cut_position, third_cut_position},
          pair{first_cut_position, second_cut_position}}) {
        affected_cities.push_back(tour[segment.first]);
        affected_cities.push_back(tour[segment.second - 1]);
        perturbed_tour.insert(perturbed_tour.end(), tour.begin() + segment.first,
                              tour.begin() + segment.second);
    }
    tour.swap(perturbed_tour);
}

struct Walker {
    Random random_generator;
    Tour tour;
    double tour_length = numeric_limits<double>::infinity();
    uint32_t stagnation_attempts = 0;
    uint64_t completed_jobs = 0;
    bool improved_this_round = false;
    double accepted_at_seconds = 0;
};

void self_test() {
    Random random_generator;
    random_generator.init(19);
    vector<Vec2d> city_coordinates;
    for (int city_index = 0; city_index < 96; ++city_index)
        city_coordinates.push_back({double(random_generator.next_uint32(1000)),
                                    double(random_generator.next_uint32(1000))});
    city_coordinates[1] = city_coordinates[0];
    ProblemInstance problem_instance(96, EUC_2D, city_coordinates, {}, true);
    problem_instance.compute_nn_lists(candidate_neighbor_count);
    auto tour = nearest_tour(problem_instance, 0);
    assert(problem_instance.is_route_valid(tour));
    two_opt_nn(problem_instance, tour, true, candidate_neighbor_count);
    for (int iteration_index = 0; iteration_index < 200; ++iteration_index) {
        Tour changed_cities;
        kick(tour, random_generator, changed_cities);
        assert(problem_instance.is_route_valid(tour));
        double length_before_repair = problem_instance.calculate_route_length(tour);
        if (iteration_index % 2)
            two_opt_nn(problem_instance, tour, changed_cities,
                       candidate_neighbor_count);
        else
            two_opt_nn(problem_instance, tour, true, candidate_neighbor_count);
        assert(problem_instance.is_route_valid(tour));
        assert(problem_instance.calculate_route_length(tour) <= length_before_repair);
    }
    cout
        << "Permutation, duplicate-coordinate, perturbation and monotone refinement checks passed\n";
}

int main(int argument_count, char **arguments) {
    try {
        if (argument_count == 2 && string(arguments[1]) == "--self-test") {
            self_test();
            return 0;
        }
        if (argument_count != 8)
            throw runtime_error(
                "Usage: solver instance mode threads seed seconds rounds output.json");
        string search_mode = arguments[2];
        int thread_count = stoi(arguments[3]), round_limit = stoi(arguments[6]);
        uint64_t random_seed = stoull(arguments[4]);
        double time_budget_seconds = stod(arguments[5]);
        if ((search_mode != "ils" && search_mode != "cooperate") || thread_count < 1 ||
            thread_count > 64 || !isfinite(time_budget_seconds) ||
            time_budget_seconds < 0 || round_limit < 0 ||
            (time_budget_seconds == 0 && round_limit == 0))
            throw runtime_error("Invalid experiment parameters");
        omp_set_dynamic(0);
        omp_set_num_threads(thread_count);
        const double search_start_time = omp_get_wtime();
        auto problem_instance = load_tsplib_instance(arguments[1]);
        if (!problem_instance.is_symmetric_ ||
            problem_instance.edge_weight_type_ != EUC_2D ||
            problem_instance.dimension_ < 24)
            throw runtime_error(
                "Experiment requires EUC_2D symmetric TSP, at least 24 cities");
        problem_instance.compute_nn_lists(candidate_neighbor_count);
        const double preprocessing_seconds = omp_get_wtime() - search_start_time;
        vector<Walker> walkers(search_trajectory_count);
        for (int worker_id = 0; worker_id < search_trajectory_count; ++worker_id)
            walkers[worker_id].random_generator.init(
                random_seed + UINT64_C(0x9e3779b97f4a7c15) * uint64_t(worker_id + 1));
        Tour best_tour;
        double best_tour_length = numeric_limits<double>::infinity();
        json improvement_history = json::array();
        int completed_rounds = 0, elite_transfer_count = 0;
        const double deadline_time = time_budget_seconds > 0
                                         ? search_start_time + time_budget_seconds
                                         : numeric_limits<double>::infinity();
        while ((round_limit == 0 || completed_rounds < round_limit) &&
               omp_get_wtime() < deadline_time) {
#pragma omp parallel for schedule(dynamic, 1)
            for (int worker_id = 0; worker_id < search_trajectory_count; ++worker_id) {
                auto &walker = walkers[worker_id];
                walker.improved_this_round = false;
                if (omp_get_wtime() >= deadline_time)
                    continue;
                Tour candidate_tour, changed_cities;
                bool requires_initial_tour = walker.tour.empty();
                if (requires_initial_tour) {
                    candidate_tour = nearest_tour(problem_instance,
                                                  walker.random_generator.next_uint32(
                                                      problem_instance.dimension_));
                    two_opt_nn(problem_instance, candidate_tour, true,
                               candidate_neighbor_count);
                    three_opt_nn(problem_instance, candidate_tour, true,
                                 candidate_neighbor_count);
                } else {
                    candidate_tour = walker.tour;
                    kick(candidate_tour, walker.random_generator, changed_cities);
                    if (walker.stagnation_attempts >= 100)
                        kick(candidate_tour, walker.random_generator, changed_cities);
                    if (search_mode == "ils")
                        two_opt_nn(problem_instance, candidate_tour, true,
                                   candidate_neighbor_count);
                    else {
                        // shortcut: candidate checklist may miss distant gains;
                        // benchmark against full scans.
                        two_opt_nn(problem_instance, candidate_tour, changed_cities,
                                   candidate_neighbor_count);
                    }
                }
                double tour_length =
                    problem_instance.calculate_route_length(candidate_tour);
                double completion_time = omp_get_wtime();
                ++walker.completed_jobs;
                // Ignore late results; unfinished refinement is allowed to finish
                // safely.
                if (completion_time > deadline_time)
                    continue;
                if (tour_length <= walker.tour_length) {
                    walker.improved_this_round = tour_length < walker.tour_length;
                    walker.stagnation_attempts =
                        walker.improved_this_round ? 0 : walker.stagnation_attempts + 1;
                    walker.tour.swap(candidate_tour);
                    walker.tour_length = tour_length;
                    walker.accepted_at_seconds = completion_time - search_start_time;
                } else
                    ++walker.stagnation_attempts;
            }
            vector<int> completion_order(search_trajectory_count);
            iota(completion_order.begin(), completion_order.end(), 0);
            sort(completion_order.begin(), completion_order.end(),
                 [&](int left_worker, int right_worker) {
                     return walkers[left_worker].accepted_at_seconds <
                                walkers[right_worker].accepted_at_seconds ||
                            (walkers[left_worker].accepted_at_seconds ==
                                 walkers[right_worker].accepted_at_seconds &&
                             left_worker < right_worker);
                 });
            for (int worker_id : completion_order) {
                const auto &walker = walkers[worker_id];
                if (walker.improved_this_round &&
                    walker.tour_length < best_tour_length) {
                    best_tour_length = walker.tour_length;
                    best_tour = walker.tour;
                    improvement_history.push_back(
                        {{"seconds", walker.accepted_at_seconds},
                         {"cost", best_tour_length}});
                }
            }
            ++completed_rounds;
            if (search_mode == "cooperate" && completed_rounds % 32 == 0 &&
                !best_tour.empty()) {
                auto worst_walker = max_element(
                    walkers.begin(), walkers.end(),
                    [](const auto &left_worker, const auto &right_worker) {
                        return left_worker.tour_length < right_worker.tour_length;
                    });
                if (worst_walker->stagnation_attempts >= 32 &&
                    worst_walker->tour_length > best_tour_length) {
                    // Replace only one stalled trajectory, leaving the other seven
                    // independent.
                    worst_walker->tour = best_tour;
                    worst_walker->tour_length = best_tour_length;
                    worst_walker->stagnation_attempts = 0;
                    ++elite_transfer_count;
                }
            }
        }
        const double elapsed_seconds = omp_get_wtime() - search_start_time;
        if (best_tour.empty())
            throw runtime_error("Budget expired before any tour completed");
        if (!problem_instance.is_route_valid(best_tour) ||
            problem_instance.calculate_route_length(best_tour) != best_tour_length)
            throw runtime_error("Invalid final tour");
        json walker_states = json::array();
        uint64_t completed_jobs = 0;
        for (const auto &walker : walkers) {
            walker_states.push_back({{"cost", walker.tour_length},
                                     {"route", walker.tour},
                                     {"jobs", walker.completed_jobs}});
            completed_jobs += walker.completed_jobs;
        }
        json result_data = {{"mode", search_mode},
                            {"threads", thread_count},
                            {"seed", random_seed},
                            {"budget_seconds", time_budget_seconds},
                            {"preprocessing_seconds", preprocessing_seconds},
                            {"elapsed_seconds", elapsed_seconds},
                            {"rounds", completed_rounds},
                            {"jobs", completed_jobs},
                            {"migrations", elite_transfer_count},
                            {"cost", best_tour_length},
                            {"route", best_tour},
                            {"trace", improvement_history},
                            {"walkers", walker_states}};
        ofstream result_file(arguments[7]);
        if (!result_file)
            throw runtime_error("Cannot open result path");
        result_file << result_data.dump();
        result_file.close();
        if (!result_file)
            throw runtime_error("Cannot write result");
        cout << "Final cost " << best_tour_length << ", jobs " << completed_jobs
             << ", seconds " << elapsed_seconds << "\n";
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << "\n";
        return 1;
    }
}
