#pragma once

#include <bits/stdc++.h>
#include "json.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

using namespace std;
using json = nlohmann::json;
namespace fs = filesystem;
using Clock = chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return chrono::duration<double>(Clock::now() - start).count();
}

void require(bool condition, const string &message) {
    if (!condition)
        throw runtime_error(message);
}

void write_text(const fs::path &path, const string &text) {
    ofstream output(path);
    output << text;
    output.close();
    require(bool(output), "Cannot write " + path.string());
}

json read_json(const fs::path &path) {
    ifstream input(path);
    require(bool(input), "Cannot open " + path.string());
    json value;
    input >> value;
    return value;
}

fs::path executable(const string &name) {
#ifdef _WIN32
    return fs::absolute(name + ".exe");
#else
    return fs::absolute(name);
#endif
}

#ifdef _WIN32
string quote_argument(const string &argument) {
    string quoted = "\"";
    size_t backslashes = 0;
    for (char character : argument) {
        if (character == '\\') {
            ++backslashes;
            continue;
        }
        quoted.append(backslashes * (character == '"' ? 2 : 1), '\\');
        backslashes = 0;
        if (character == '"')
            quoted += '\\';
        quoted += character;
    }
    quoted.append(backslashes * 2, '\\');
    return quoted + '"';
}
#endif

// Arguments go directly to the process API, never through a shell.
double run_process(const vector<string> &arguments, const fs::path &log_path,
                   double timeout_seconds = 600) {
    require(!arguments.empty() && timeout_seconds > 0, "Invalid process request");
    const auto start = Clock::now();
#ifdef _WIN32
    SECURITY_ATTRIBUTES security = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE log_file =
        CreateFileW(log_path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                    &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(log_file != INVALID_HANDLE_VALUE, "Cannot open process log");
    string command;
    for (const auto &argument : arguments)
        command += quote_argument(argument) + ' ';
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log_file;
    startup.hStdError = log_file;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process = {};
    const bool created =
        CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(log_file);
    require(created, "Cannot start " + arguments.front());
    const auto wait_result = WaitForSingleObject(
        process.hProcess, static_cast<DWORD>(timeout_seconds * 1000));
    if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    require(wait_result == WAIT_OBJECT_0,
            "Process timed out; see " + log_path.string());
    require(exit_code == 0, "Process failed; see " + log_path.string());
#else
    vector<char *> argv;
    for (const auto &argument : arguments)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    require(posix_spawn_file_actions_init(&actions) == 0, "Cannot prepare process");
    const int open_status = posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const int redirect_status =
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t process_id = -1;
    const int spawn_status = (open_status || redirect_status)
                                 ? EINVAL
                                 : posix_spawn(&process_id, argv[0], &actions, nullptr,
                                               argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    require(spawn_status == 0, "Cannot start " + arguments.front() + ": " +
                                   string(strerror(spawn_status)));
    int status = 0;
    while (true) {
        const auto wait_result = waitpid(process_id, &status, WNOHANG);
        if (wait_result == process_id)
            break;
        if (wait_result < 0 && errno == EINTR)
            continue;
        if (wait_result < 0 || seconds_since(start) > timeout_seconds) {
            kill(process_id, SIGKILL);
            while (waitpid(process_id, &status, 0) < 0 && errno == EINTR) {
            }
            throw runtime_error("Process failed or timed out; see " +
                                log_path.string());
        }
        this_thread::sleep_for(chrono::milliseconds(2));
    }
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "Process failed; see " + log_path.string());
#endif
    return seconds_since(start);
}

string trim(string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == string::npos)
        return "";
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

using Coordinates = vector<pair<double, double>>;

Coordinates read_coordinates(const fs::path &path) {
    ifstream input(path);
    require(bool(input), "Cannot open instance: " + path.string());
    map<string, string> header;
    Coordinates coordinates;
    string line;
    bool reading_coordinates = false;
    while (getline(input, line)) {
        line = trim(line);
        if (line.empty())
            continue;
        if (line == "NODE_COORD_SECTION") {
            reading_coordinates = true;
            continue;
        }
        if (line == "EOF")
            break;
        if (!reading_coordinates) {
            const auto separator = line.find(':');
            if (separator != string::npos)
                header[trim(line.substr(0, separator))] =
                    trim(line.substr(separator + 1));
            continue;
        }
        istringstream row(line);
        int city_id;
        double x_coordinate, y_coordinate;
        string extra;
        require(bool(row >> city_id >> x_coordinate >> y_coordinate) && !(row >> extra),
                "Invalid coordinate row");
        require(city_id == static_cast<int>(coordinates.size()) + 1 &&
                    isfinite(x_coordinate) && isfinite(y_coordinate),
                "Expected ordered city IDs and finite coordinates");
        coordinates.emplace_back(x_coordinate, y_coordinate);
    }
    require(header["TYPE"] == "TSP" && header["EDGE_WEIGHT_TYPE"] == "EUC_2D",
            "Only symmetric EUC_2D TSP instances are supported");
    require(header.count("DIMENSION") && !coordinates.empty() &&
                coordinates.size() == stoull(header.at("DIMENSION")),
            "Coordinate count does not match DIMENSION");
    return coordinates;
}

long long validate_route(const Coordinates &coordinates, const json &route,
                         double expected_cost) {
    require(route.is_array() && route.size() == coordinates.size(),
            "Wrong number of cities in route");
    vector<bool> visited(coordinates.size(), false);
    vector<size_t> cities;
    for (const auto &value : route) {
        require(value.is_number_integer(), "Route city must be an integer");
        const auto city = value.get<long long>();
        require(city >= 0 && static_cast<size_t>(city) < coordinates.size() &&
                    !visited[city],
                "Duplicate or out-of-range city");
        visited[city] = true;
        cities.push_back(static_cast<size_t>(city));
    }
    long long cost = 0;
    for (size_t position = 0; position < cities.size(); ++position) {
        const auto [from_x, from_y] = coordinates[cities[position]];
        const auto [to_x, to_y] = coordinates[cities[(position + 1) % cities.size()]];
        const double distance = floor(hypot(from_x - to_x, from_y - to_y) + 0.5);
        require(isfinite(distance) && distance >= 0 &&
                    distance <
                        static_cast<double>(numeric_limits<long long>::max() - cost),
                "Route cost exceeds supported range");
        cost += static_cast<long long>(distance);
    }
    require(isfinite(expected_cost) && static_cast<double>(cost) == expected_cost,
            "Reported tour cost does not match independent calculation");
    return cost;
}

double median(vector<double> values) {
    require(!values.empty(), "Cannot summarize empty results");
    sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle]
                             : (values[middle - 1] + values[middle]) / 2;
}

fs::path new_result_directory(const string &suite) {
    fs::create_directories("results");
    const auto timestamp = chrono::duration_cast<chrono::microseconds>(
                               chrono::system_clock::now().time_since_epoch())
                               .count();
    const fs::path directory =
        fs::path("results") / (suite + "_" + to_string(timestamp));
    require(fs::create_directory(directory), "Result directory already exists");
    return directory;
}

void validate_trace(const json &result, double budget_seconds) {
    double previous_cost = numeric_limits<double>::infinity();
    double previous_seconds = 0;
    for (const auto &event : result.at("trace")) {
        const double cost = event.at("cost");
        const double seconds = event.at("seconds");
        require(isfinite(cost) && isfinite(seconds) && cost < previous_cost &&
                    seconds >= previous_seconds &&
                    (budget_seconds == 0 || seconds <= budget_seconds),
                "Invalid ILS improvement trace");
        previous_cost = cost;
        previous_seconds = seconds;
    }
    require(previous_cost == result.at("cost").get<double>(),
            "Final cost does not match improvement trace");
}
