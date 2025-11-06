
// --- C++ Standard Libraries ---
#include <iostream>
#include <string>
#include <map>           
#include <list>          
#include <mutex>         
#include <stdexcept>     
#include <queue>         
#include <memory>        
#include <condition_variable> 
#include<pthread.h>


// Define this *before* including httplib.h to set the thread pool size
#define CPPHTTPLIB_THREAD_POOL_COUNT 128
#include "cpp-httplib/httplib.h"

#include <pqxx/pqxx>

// --- Configuration ---
const std::string DB_CONNECTION_STRING = "dbname=postgres user=postgres password=mysecretpassword hostaddr=127.0.0.1 port=5432";
const int CACHE_CAPACITY = 1024; // Max number of items in the LRU cache
const int DB_POOL_SIZE = 64;    // Number of connections in the DB pool


class LRUCache {
public:
    LRUCache(size_t capacity) : capacity_(capacity) {}

    bool get(const std::string& key, std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_); // Thread-safe access

        auto it = cache_items_map_.find(key);
        if (it == cache_items_map_.end()) {
            return false; // Not found
        }

        cache_items_list_.splice(cache_items_list_.begin(), cache_items_list_, it->second);
        
        value = it->second->second;
        return true;
    }

    void put(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_); // Thread-safe access

        auto it = cache_items_map_.find(key);
        if (it != cache_items_map_.end()) {
            it->second->second = value;
            cache_items_list_.splice(cache_items_list_.begin(), cache_items_list_, it->second);
            return;
        }

        if (cache_items_map_.size() == capacity_) {
            auto last = cache_items_list_.back();
            cache_items_map_.erase(last.first);
            cache_items_list_.pop_back();
        }

        cache_items_list_.emplace_front(key, value);
        cache_items_map_[key] = cache_items_list_.begin();
    }

    void remove(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_); // Thread-safe access

        auto it = cache_items_map_.find(key);
        if (it != cache_items_map_.end()) {
            cache_items_list_.erase(it->second);
            cache_items_map_.erase(it);
        }
    }

private:
    size_t capacity_;
    std::list<std::pair<std::string, std::string>> cache_items_list_;
    std::map<std::string, std::list<std::pair<std::string, std::string>>::iterator> cache_items_map_;
    std::mutex mutex_; // Mutex to protect all cache operations
};


class ConnectionPool {
public:
    ConnectionPool(size_t pool_size, const std::string& conn_string) {
        for (size_t i = 0; i < pool_size; ++i) {
            try {
                // Create a new connection and add it to the pool
                auto conn = std::make_unique<pqxx::connection>(conn_string);
                if (!conn->is_open()) {
                    throw std::runtime_error("Failed to open DB connection for pool");
                }
                pool_.push(std::move(conn));
            } catch (const std::exception& e) {
                std::cerr << "Error creating connection for pool: " << e.what() << std::endl;
            }
        }
    }

    std::unique_ptr<pqxx::connection> get() {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Wait until a connection is available
        condition_.wait(lock, [this] { return !pool_.empty(); });

        // Get the connection from the front of the queue
        std::unique_ptr<pqxx::connection> conn = std::move(pool_.front());
        pool_.pop();
        
        return conn;
    }

    void release(std::unique_ptr<pqxx::connection> conn) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        pool_.push(std::move(conn));
        
        // Notify one waiting thread that a connection is available
        condition_.notify_one();
    }

private:
    std::queue<std::unique_ptr<pqxx::connection>> pool_;
    std::mutex mutex_;
    std::condition_variable condition_;
};


class PooledConnection {
public:
    PooledConnection(ConnectionPool& pool)
        : pool_(pool), conn_(pool.get()) {}

    ~PooledConnection() {
        if (conn_) {
            pool_.release(std::move(conn_));
        }
    }

    // Get a reference to the underlying pqxx::connection
    pqxx::connection& get() {
        return *conn_;
    }

    // Prevent copying
    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;

private:
    ConnectionPool& pool_;
    std::unique_ptr<pqxx::connection> conn_;
};


class ServerApp {
public:
    // Constructor: Connect to DB, set up cache and HTTP routes
    ServerApp() 
      : cache_(CACHE_CAPACITY), 
        db_pool_(DB_POOL_SIZE, DB_CONNECTION_STRING) // Initialize the connection pool
        {
        // Create the table if it doesn't exist (using one of the connections)
        initialize_database();
        std::cout << "Database connection pool created with " << DB_POOL_SIZE << " connections." << std::endl;
                
        setup_routes();
    }

    void start(int port) {
        std::cout << "Server listening on port : " << port << std::endl;
        svr_.listen("0.0.0.0", port);
    }

private:
    httplib::Server svr_;
    LRUCache cache_;
    ConnectionPool db_pool_;

    void setup_routes() {

        svr_.Put("/kv/(.+)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string key = req.matches[1];
            std::string value = req.body;

            try {
                // Get a connection from the pool. It will be auto-returned.
                auto conn = PooledConnection(this->db_pool_);

                // 1. Write to the database
                pqxx::work txn(conn.get()); // Use the pooled connection
                
                std::string sql = "INSERT INTO key_value (key_text, value_text) VALUES (" + 
                                  txn.quote(key) + ", " + txn.quote(value) + ") " +
                                  "ON CONFLICT (key_text) DO UPDATE SET value_text = " + txn.quote(value);
                txn.exec(sql);
                txn.commit();

                this->cache_.put(key, value);

                res.set_content("OK", "text/plain");

            } catch (const std::exception &e) {
                res.status = 500;
                res.set_content("Database error: " + std::string(e.what()), "text/plain");
            }
        });

        svr_.Get("/kv/(.+)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string key = req.matches[1];
            std::string value;

            if (this->cache_.get(key, value)) {
                // Cache Hit
                res.set_content(value, "text/plain");
                return;
            }

            try {
                // Get a connection from the pool
                auto conn = PooledConnection(this->db_pool_);
                
                pqxx::work txn(conn.get()); 
                std::string sql = "SELECT value_text FROM key_value WHERE key_text = " + txn.quote(key);
                pqxx::result r = txn.exec(sql);
                txn.commit(); 

                if (r.empty()) {
                    res.status = 404;
                    res.set_content("Not Found", "text/plain");
                } else {
                    // Found in DB
                    value = r[0][0].as<std::string>();
                    res.set_content(value, "text/plain");
                    
                    this->cache_.put(key, value);
                }
            } catch (const std::exception &e) {
                res.status = 500;
                res.set_content("Database error: " + std::string(e.what()), "text/plain");
            }
        });

        svr_.Delete("/kv/(.+)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string key = req.matches[1];

            try {
                // Get a connection from the pool
                auto conn = PooledConnection(this->db_pool_);

                // 1. Delete from database
                pqxx::work txn(conn.get()); // Use the pooled connection
                std::string sql = "DELETE FROM key_value WHERE key_text = " + txn.quote(key);
                txn.exec(sql);
                txn.commit();

                // 2. Delete from cache
                this->cache_.remove(key);

                res.set_content("OK", "text/plain");

            } catch (const std::exception &e) {
                res.status = 500;
                res.set_content("Database error: " + std::string(e.what()), "text/plain");
            }
        });
    }

    // Create the 'key_value' table if it doesn't already exist
    void initialize_database() {
        try {
            auto conn = PooledConnection(this->db_pool_);
            pqxx::work txn(conn.get());
            txn.exec(R"(
                CREATE TABLE IF NOT EXISTS key_value (
                    id SERIAL PRIMARY KEY,
                    key_text TEXT UNIQUE NOT NULL,
                    value_text TEXT
                );
            )");
            txn.commit();
            std::cout << "Database table 'key_value' initialized." << std::endl;
        } catch (const std::exception &e) {
            std::cerr << "Database initialization error: " << e.what() << std::endl;
            throw; // Re-throw to stop the server from starting
        }
    }
};


int main() {
    try {
        ServerApp server;
        server.start(8080); // Start listening on port 8080

    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

