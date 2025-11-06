
// --- C++ Standard Libraries ---
#include <iostream>
#include <string>
#include <vector>
#include <sstream>

#include "cpp-httplib/httplib.h"

// Server address
std::string SERVER_HOST = "127.0.0.1";
int SERVER_PORT = 8080;

std::vector<std::string> parse_command(const std::string& input) {
    std::istringstream iss(input);
    std::string token;
    std::vector<std::string> tokens;
    
    // Read the command (e.g., "get", "put", "delete")
    if (iss >> token) {
        tokens.push_back(token);
    }

    // Read the key
    if (iss >> token) {
        tokens.push_back(token);
    }

    // If the command is "put", read the rest of the line as the value
    if (tokens.size() > 0 && tokens[0] == "put") {
        std::string value;
        // Gulp the rest of the line
        if (std::getline(iss, value)) {
            // Remove leading whitespace (if any)
            size_t first = value.find_first_not_of(' ');
            if (std::string::npos == first) {
                tokens.push_back(""); // Empty value
            } else {
                tokens.push_back(value.substr(first));
            }
        }
    }
    
    return tokens;
}

void print_usage() {
    std::cout << "Usage:" << std::endl;
    std::cout << "  get <key>" << std::endl;
    std::cout << "  put <key> <value>" << std::endl;
    std::cout << "  delete <key>" << std::endl;
    std::cout << "  quit/exit" << std::endl;
    std::cout << "--------------------" << std::endl;
}

void print_response(const httplib::Result& res) {
    if (res) {
        std::cout << "  Status: " << res->status << std::endl;
        std::cout << "  Body:   " << res->body << std::endl;
    } else {
        std::cout << "  Error: " << httplib::to_string(res.error()) << std::endl;
    }
    std::cout << "--------------------" << std::endl;
}

int main(int argc, char* argv[]) {
    // Create an HTTP client
    
    SERVER_HOST = argc > 1 ? argv[1] : SERVER_HOST;
    SERVER_PORT = argc > 2 ? std::stoi(argv[2]) : SERVER_PORT; 

    httplib::Client cli(SERVER_HOST, SERVER_PORT);
    cli.set_connection_timeout(5, 0); 

    std::cout << "Interactive KV Client" << std::endl;
    std::cout << "Connected to http://" << SERVER_HOST << ":" << SERVER_PORT << std::endl;
    print_usage();

    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) {
            break; // End of input
        }

        auto tokens = parse_command(line);
        if (tokens.empty()) {
            continue; // Empty line
        }

        std::string cmd = tokens[0];

        // --- Handle QUIT ---
        if (cmd == "quit" || cmd == "exit") {
            break;
        }

        else if (cmd == "get") {
            if (tokens.size() != 2) {
                std::cout << "Error: 'get' requires one <key> argument." << std::endl;
                continue;
            }
            std::string path = "/kv/" + tokens[1];
            print_response(cli.Get(path.c_str()));
        }

        else if (cmd == "put") {
            if (tokens.size() != 3) {
                std::cout << "Error: 'put' requires <key> and <value> arguments." << std::endl;
                continue;
            }
            std::string path = "/kv/" + tokens[1];
            std::string value = tokens[2];
            print_response(cli.Put(path.c_str(), value, "text/plain"));
        }

        else if (cmd == "delete") {
            if (tokens.size() != 2) {
                std::cout << "Error: 'delete' requires one <key> argument." << std::endl;
                continue;
            }
            std::string path = "/kv/" + tokens[1];
            print_response(cli.Delete(path.c_str()));
        }

        else {
            std::cout << "Error: Unknown command '" << cmd << "'" << std::endl;
            print_usage();
        }
    }

    std::cout << "Goodbye!" << std::endl;
    return 0;
}
