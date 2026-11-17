#include <bits/stdc++.h>
#include <omp.h>
#include "experiment_support.hpp"
#include "settings.hpp"

using namespace std;

json run_faco(const ExperimentSettings &settings, const Coordinates &coordinates,
              const fs::path &instance, const fs::path &directory,
              const string &variant, int seed) {
    const auto prefix = directory / (variant + "_seed_" + to_string(seed));
    const string output_path = prefix.string() + ".json";
    const double process_seconds =
        run_process({executable("faco").string(), variant, instance.string(),
                     to_string(settings.thread_count), to_string(settings.ant_count),
                     to_string(settings.faco_iterations), to_string(seed), output_path},
                    prefix.string() + ".log");
    auto result = read_json(output_path);
    const auto &execution = result.at("executions").at(0);
    validate_route(coordinates, execution.at("final route"),
                   execution.at("final cost"));
    result["process_seconds"] = process_seconds;
    write_text(output_path, result.dump(2));
    return result;
}

void compare_faco(const ExperimentSettings &settings, const Coordinates &coordinates,
                  const fs::path &instance, const fs::path &directory) {
    require(coordinates.size() >= 81, "FACO needs at least 81 cities");
    vector<double> baseline_solver_times, combined_solver_times;
    vector<double> baseline_process_times, combined_process_times;
    vector<double> solver_speedups, process_speedups, costs;
    ostringstream rows;
    rows << setprecision(12)
         << "variant,seed,cost,solver_seconds,process_seconds,preprocessing_seconds\n";
    mt19937 order_generator(20261031);
    for (const int seed : settings.seeds) {
        vector<string> order = {"baseline", "combined"};
        shuffle(order.begin(), order.end(), order_generator);
        map<string, json> paired_results;
        for (const auto &variant : order) {
            auto result =
                run_faco(settings, coordinates, instance, directory, variant, seed);
            const auto &execution = result.at("executions").at(0);
            const double cost = execution.at("final cost");
            const double solver_seconds = execution.at("execution time");
            const double process_seconds = result.at("process_seconds");
            rows << variant << ',' << seed << ',' << cost << ',' << solver_seconds
                 << ',' << process_seconds << ','
                 << result.at("nn and backup lists calc time").get<double>() << '\n';
            write_text(directory / "runs.csv", rows.str());
            cout << variant << ", seed " << seed << ": cost " << cost << ", solver "
                 << solver_seconds << " s, process " << process_seconds << " s" << endl;
            paired_results.emplace(variant, move(result));
        }
        const auto &baseline = paired_results.at("baseline").at("executions").at(0);
        const auto &combined = paired_results.at("combined").at("executions").at(0);
        for (const string field : {"final route", "final cost", "best sol cost values",
                                   "sol cost mean values", "sol cost stdev values"})
            require(baseline.at(field) == combined.at(field),
                    "FACO paired results differ: " + field);
        if (baseline.count("state checks") || combined.count("state checks"))
            require(baseline.at("state checks") == combined.at("state checks"),
                    "FACO internal states differ");
        const double baseline_solver = baseline.at("execution time");
        const double combined_solver = combined.at("execution time");
        const double baseline_process =
            paired_results.at("baseline").at("process_seconds");
        const double combined_process =
            paired_results.at("combined").at("process_seconds");
        require(combined_solver > 0 && combined_process > 0, "Invalid measured time");
        baseline_solver_times.push_back(baseline_solver);
        combined_solver_times.push_back(combined_solver);
        baseline_process_times.push_back(baseline_process);
        combined_process_times.push_back(combined_process);
        solver_speedups.push_back(baseline_solver / combined_solver);
        process_speedups.push_back(baseline_process / combined_process);
        costs.push_back(baseline.at("final cost"));
    }
    ostringstream summary;
    summary << setprecision(12)
            << "method,median_solver_seconds,median_process_seconds,mean_cost\n";
    const double mean_cost = accumulate(costs.begin(), costs.end(), 0.0) / costs.size();
    summary << "FACO baseline," << median(baseline_solver_times) << ','
            << median(baseline_process_times) << ',' << mean_cost << '\n';
    summary << "FACO A+B," << median(combined_solver_times) << ','
            << median(combined_process_times) << ',' << mean_cost << '\n';
    write_text(directory / "summary.csv", summary.str());
    ostringstream speedups;
    speedups
        << setprecision(12)
        << "comparison,median_paired_solver_speedup,median_paired_process_speedup\n"
        << "Baseline vs A+B," << median(solver_speedups) << ','
        << median(process_speedups) << '\n';
    write_text(directory / "paired_speedups.csv", speedups.str());
    cout << '\n'
         << summary.str() << speedups.str()
         << "All paired FACO routes and quality traces match.\n";
    write_text(directory / "validation.json",
               json({{"paired_routes_and_quality_traces_match", true},
                     {"paired_seeds", settings.seeds}})
                   .dump(2));
}

json run_ils(const Coordinates &coordinates, const fs::path &instance,
             const fs::path &directory, const string &mode, int threads, int seed,
             double seconds, int rounds, const string &label) {
    const auto result_path = directory / (label + ".json");
    const double process_seconds =
        run_process({executable("focused_sharing").string(), instance.string(), mode,
                     to_string(threads), to_string(seed), to_string(seconds),
                     to_string(rounds), result_path.string()},
                    directory / (label + ".log"), max(300.0, seconds * 3));
    auto result = read_json(result_path);
    validate_route(coordinates, result.at("route"), result.at("cost"));
    validate_trace(result, seconds);
    require(result.at("mode") == mode && result.at("threads") == threads &&
                result.at("seed") == seed,
            "Unexpected ILS run metadata");
    result["process_seconds"] = process_seconds;
    write_text(result_path, result.dump(2));
    return result;
}

void compare_ils(const ExperimentSettings &settings, const Coordinates &coordinates,
                 const fs::path &instance, const fs::path &directory) {
    map<string, vector<double>> costs, process_times, search_times;
    vector<double> cost_improvements, elapsed_ratios;
    ostringstream rows, pairs;
    rows
        << setprecision(12)
        << "method,seed,cost,budget_seconds,solver_seconds,process_seconds,jobs,migrations\n";
    pairs << setprecision(12)
          << "seed,cost_improvement_percent,normal_over_enhanced_process_time\n";
    mt19937 order_generator(20261031);
    for (const int seed : settings.seeds) {
        vector<string> order = {"ils", "cooperate"};
        shuffle(order.begin(), order.end(), order_generator);
        map<string, json> results;
        for (const auto &mode : order) {
            auto result = run_ils(coordinates, instance, directory, mode,
                                  settings.thread_count, seed, settings.ils_seconds, 0,
                                  mode + "_seed_" + to_string(seed));
            const double cost = result.at("cost");
            const double process_seconds = result.at("process_seconds");
            const double solver_seconds = result.at("elapsed_seconds");
            costs[mode].push_back(cost);
            process_times[mode].push_back(process_seconds);
            search_times[mode].push_back(solver_seconds);
            rows << mode << ',' << seed << ',' << cost << ',' << settings.ils_seconds
                 << ',' << solver_seconds << ',' << process_seconds << ','
                 << result.at("jobs") << ',' << result.at("migrations") << '\n';
            write_text(directory / "runs.csv", rows.str());
            cout << mode << ", seed " << seed << ": cost " << cost << ", process "
                 << process_seconds << " s, jobs " << result.at("jobs")
                 << ", transfers " << result.at("migrations") << endl;
            results.emplace(mode, move(result));
        }
        const double normal_cost = results.at("ils").at("cost");
        const double enhanced_cost = results.at("cooperate").at("cost");
        // Zero-length instances have no percentage improvement to calculate.
        const double improvement =
            normal_cost > 0 ? 100 * (normal_cost - enhanced_cost) / normal_cost : 0;
        const double elapsed_ratio =
            results.at("ils").at("process_seconds").get<double>() /
            results.at("cooperate").at("process_seconds").get<double>();
        cost_improvements.push_back(improvement);
        elapsed_ratios.push_back(elapsed_ratio);
        pairs << seed << ',' << improvement << ',' << elapsed_ratio << '\n';
        write_text(directory / "paired_comparison.csv", pairs.str());
    }
    ostringstream summary;
    summary
        << setprecision(12)
        << "method,mean_cost,best_cost,median_solver_seconds,median_process_seconds\n";
    for (const string mode : {"ils", "cooperate"}) {
        const string label =
            mode == "ils" ? "Normal ILS" : "ILS + focused repair + selective sharing";
        summary << label << ','
                << accumulate(costs[mode].begin(), costs[mode].end(), 0.0) /
                       costs[mode].size()
                << ',' << *min_element(costs[mode].begin(), costs[mode].end()) << ','
                << median(search_times[mode]) << ',' << median(process_times[mode])
                << '\n';
    }
    write_text(directory / "summary.csv", summary.str());
    cout
        << '\n'
        << summary.str()
        << "Median paired cost improvement (%): " << median(cost_improvements)
        << "\nPositive means focused repair + sharing found shorter tours.\n"
        << "Median normal/enhanced process-time ratio: " << median(elapsed_ratios)
        << "\nEqual time budgets compare solution quality, not time to the same solution.\n";
}

void run_checks(const ExperimentSettings &settings, const Coordinates &coordinates,
                const fs::path &instance, const fs::path &directory) {
    const Coordinates square = {{0, 0}, {3, 0}, {3, 4}, {0, 4}};
    require(validate_route(square, json::array({0, 1, 2, 3}), 14) == 14,
            "Independent distance validation failed");
    for (const auto &bad_route : {json::array({0, 1, 1, 3}), json::array({0, 1, 2, 4}),
                                  json::array({0, 1, 2}), json::array({0, 1, 2, -1})}) {
        bool rejected = false;
        try {
            validate_route(square, bad_route, 14);
        } catch (const exception &) {
            rejected = true;
        }
        require(rejected, "Invalid tour was accepted");
    }
    require(median({4, 1, 2, 3}) == 2.5 && median({3, 1, 2}) == 2,
            "Median calculation failed");
    run_process({executable("focused_sharing").string(), "--self-test"},
                directory / "solver_self_test.log");
    for (const string mode : {"ils", "cooperate"}) {
        const auto sequential = run_ils(coordinates, instance, directory, mode, 1, 101,
                                        0, settings.check_rounds, mode + "_one_thread");
        const auto parallel =
            run_ils(coordinates, instance, directory, mode, settings.thread_count, 101,
                    0, settings.check_rounds, mode + "_parallel");
        for (const string field :
             {"route", "cost", "walkers", "migrations", "jobs", "rounds"})
            require(sequential.at(field) == parallel.at(field),
                    "ILS thread-count check failed: " + mode + " " + field);
        cout << mode << ": fixed-round states match across thread counts; transfers "
             << parallel.at("migrations") << endl;
    }
    write_text(directory / "validation.json",
               json({{"independent_validator_checks", true},
                     {"solver_self_test", true},
                     {"fixed_round_thread_states_match", true},
                     {"rounds", settings.check_rounds}})
                   .dump(2));
}

int main(int argument_count, char **arguments) {
    try {
        require(argument_count >= 2 && argument_count <= 3,
                "Usage: experiments faco|ils|check [--smoke]");
        const string suite = arguments[1];
        require(suite == "faco" || suite == "ils" || suite == "check", "Unknown suite");
        ExperimentSettings settings;
        if (argument_count == 3) {
            require(string(arguments[2]) == "--smoke", "Unknown option");
            settings.thread_count = min(2, omp_get_num_procs());
            settings.seeds = {11, 23};
            settings.faco_iterations = 300;
            settings.ils_seconds = 2;
        }
        require(settings.thread_count >= 1 && settings.thread_count <= 64 &&
                    settings.faco_iterations > 0 && settings.ant_count >= 2 &&
                    isfinite(settings.ils_seconds) && settings.ils_seconds > 0 &&
                    settings.check_rounds > 0 && !settings.seeds.empty(),
                "Invalid experiment settings");
        require(set<int>(settings.seeds.begin(), settings.seeds.end()).size() ==
                        settings.seeds.size() &&
                    *min_element(settings.seeds.begin(), settings.seeds.end()) > 0,
                "Use unique positive seeds");
#ifdef _WIN32
        _putenv_s("OMP_NUM_THREADS", to_string(settings.thread_count).c_str());
        _putenv_s("OMP_DYNAMIC", "FALSE");
        _putenv_s("OMP_WAIT_POLICY", "ACTIVE");
#else
        setenv("OMP_NUM_THREADS", to_string(settings.thread_count).c_str(), 1);
        setenv("OMP_DYNAMIC", "FALSE", 1);
        setenv("OMP_WAIT_POLICY", "ACTIVE", 1);
#endif
        const auto instance = fs::absolute(fs::path("deps/faco/instances") /
                                           (settings.instance_name + ".tsp"));
        const auto coordinates = read_coordinates(instance);
        require(coordinates.size() >= 24, "ILS needs at least 24 cities");
        const auto directory = new_result_directory(suite);
        const json metadata = {
            {"suite", suite},
            {"instance", instance.string()},
            {"cities", coordinates.size()},
            {"threads", settings.thread_count},
            {"seeds", settings.seeds},
            {"ants", settings.ant_count},
            {"faco_iterations", settings.faco_iterations},
            {"ils_seconds", settings.ils_seconds},
            {"check_rounds", settings.check_rounds},
            {"smoke_test", argument_count == 3},
            {"faco_revision", "05b7c1b8d52b27d121b6138222f986fcbf196863"}};
        write_text(directory / "settings.json", metadata.dump(2));
        cout << "Instance " << settings.instance_name << ": " << coordinates.size()
             << " cities, threads " << settings.thread_count << endl;
        if (suite == "faco")
            compare_faco(settings, coordinates, instance, directory);
        else if (suite == "ils")
            compare_ils(settings, coordinates, instance, directory);
        else
            run_checks(settings, coordinates, instance, directory);
        cout << "Saved: " << fs::absolute(directory).string() << endl;
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
