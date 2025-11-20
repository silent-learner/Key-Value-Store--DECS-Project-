
// --- C++ Standard Libraries ---
#include <iostream>
#include <string>
#include <vector>
#include <thread>         
#include <atomic>         
#include <chrono>         
#include <random>         
#include <sstream>        
#include <stdexcept>      
#include <functional>     

#include "cpp-httplib/httplib.h"

std::string SERVER_HOST = "127.0.0.1";
int SERVER_PORT = 8080;

// These are safe to be read/written by multiple threads.
std::atomic<long long> total_requests_completed(0);
std::atomic<long long> total_requests_attempted(0);
std::atomic<long long> total_latency_for_completed_ms(0);

// This tells all worker threads to stop.
std::atomic<bool> stop_flag(false);

std::string generate_random_string(size_t length) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, sizeof(alphanum) - 2);

    std::string str;
    str.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        str += alphanum[dist(gen)];
    }
    return str;
}


// Generates 100% PUT requests with random keys and large 1MB values.
bool workload_put_all(httplib::Client& cli) {
    static thread_local std::string big_value = generate_random_string(1024*100); // 1 MB
    
    std::string key = "key_put_" + generate_random_string(10);
    
    auto res = cli.Put(("/kv/" + key).c_str(), big_value, "text/plain");

    if (res && res->status == 200) {
        return true;
    }
    return false;
}

// Generates 100% GET requests with unique, random keys.
bool workload_get_all(httplib::Client& cli) {
    std::string key = "key_get_all_" + generate_random_string(10);
    
    auto res = cli.Get(("/kv/" + key).c_str());

    if (res && (res->status == 200 || res->status == 404)) {
        return true;
    }
    return false;
}

// Generates 100% GET requests for a small, popular set of keys.
bool workload_get_popular(httplib::Client& cli) {
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 9);
    
    std::string key = "key_popular_" + std::to_string(dist(gen));
    
    // Send the GET request
    auto res = cli.Get(("/kv/" + key).c_str());

    // Only a 200 (OK) is a successful hit
    if (res && res->status == 200) {
        return true;
    }
    return false;
}


void client_worker_thread(std::function<bool(httplib::Client&)> workload_func) {
    // Each thread gets its own client object.
    httplib::Client cli(SERVER_HOST, SERVER_PORT);
    cli.set_connection_timeout(5, 0); 
    cli.set_read_timeout(5, 0);       
    cli.set_write_timeout(5, 0);      

    
    while (!stop_flag.load(std::memory_order_relaxed)) {
        
        total_requests_attempted.fetch_add(1, std::memory_order_relaxed);
        auto start_time = std::chrono::high_resolution_clock::now();

        bool success = workload_func(cli); 

        auto end_time = std::chrono::high_resolution_clock::now();

        if (success) {
            auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            total_requests_completed.fetch_add(1, std::memory_order_relaxed);
            total_latency_for_completed_ms.fetch_add(latency.count(), std::memory_order_relaxed);
        }
    }

}


int main(int argc, char* argv[]) {
    if (argc != 6) {
        std::cerr << "Usage: ./load_generator <num_threads> <duration_sec> <workload_type> <SERVER IP> <SERVER PORT>" << std::endl;
        std::cerr << "  Workload Types:" << std::endl;
        std::cerr << "    put_all (100% PUTs, 1MB values, disk bandwidth)" << std::endl;
        std::cerr << "    get_all (100% GETs, random keys, cache miss)" << std::endl;
        std::cerr << "    get_popular (100% GETs, 10 popular keys, cache hit)" << std::endl;
        return 1;
    }

    int num_threads = 0;
    int duration_sec = 0;
    std::string workload_type_str;
    std::function<bool(httplib::Client&)> workload_func; 

    try {
        num_threads = std::stoi(argv[1]);
        duration_sec = std::stoi(argv[2]);
        workload_type_str = argv[3];
        SERVER_HOST = argv[4];
        SERVER_PORT = std::stoi(argv[5]);
    } catch (const std::exception& e) {
        std::cerr << "Error: Invalid arguments. " << e.what() << std::endl;
        return 1;
    }

    if (num_threads <= 0 || duration_sec <= 0) {
        std::cerr << "Error: Threads and duration must be positive integers." << std::endl;
        return 1;
    }

    if (workload_type_str == "put_all") {
        workload_func = workload_put_all;
    } else if (workload_type_str == "get_all") {
        workload_func = workload_get_all;
    } else if (workload_type_str == "get_popular") {
        workload_func = workload_get_popular;
    } else {
        std::cerr << "Error: Unknown workload type '" << workload_type_str << "'" << std::endl;
        return 1;
    }

    std::cout << "--- Load Generator Started ---" << std::endl;
    std::cout << "Threads:       " << num_threads << std::endl;
    std::cout << "Duration:      " << duration_sec << " seconds" << std::endl;
    std::cout << "Workload:      " << workload_type_str << std::endl;
    std::cout << "Target:        http://" << SERVER_HOST << ":" << SERVER_PORT << std::endl;
    std::cout << "------------------------------" << std::endl;
    std::cout << "Test running..." << std::endl;

    total_requests_completed = 0;
    total_requests_attempted = 0;
    total_latency_for_completed_ms = 0;
    stop_flag = false;

    auto test_start_time = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(client_worker_thread, workload_func);
    }

    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));

    stop_flag = true; // Signal all threads to stop their loops
    
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    auto test_end_time = std::chrono::high_resolution_clock::now();
    auto actual_duration = std::chrono::duration_cast<std::chrono::milliseconds>(test_end_time - test_start_time);

    long long final_requests_completed = total_requests_completed.load();
    long long final_requests_attempted = total_requests_attempted.load();
    long long final_latency_ms = total_latency_for_completed_ms.load();
    double actual_duration_sec = actual_duration.count() / 1000.0;

    double avg_throughput = 0.0;
    if (actual_duration_sec > 0) {
        avg_throughput = final_requests_completed / actual_duration_sec;
    }

    double avg_response_time = 0.0;
    if (final_requests_completed > 0) {
        avg_response_time = static_cast<double>(final_latency_ms) / final_requests_completed;
    }

    std::cout << "--- Test Finished ---" << std::endl;
    std::cout << "Total Attempts:    " << final_requests_attempted << std::endl;
    std::cout << "Total Completed:   " << final_requests_completed << std::endl;
    std::cout << "Total Duration:    " << actual_duration_sec << " s" << std::endl;
    std::cout << "------------------------------" << std::endl;
    std::cout << "Avg Throughput (Completed): " << avg_throughput << " reqs/sec" << std::endl;
    std::cout << "Avg Response Time (Completed): " << avg_response_time << " ms" << std::endl;

    return 0;
}