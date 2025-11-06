# --- Compiler and Linker ---
CXX = g++

# --- Compiler Flags ---
# -std=c++17: Use C++17 standard
# -g:         Include debug symbols
# -Wall:      Enable all warnings
# -I../cpp-httplib: Add the cpp-httplib directory to the include path
#                   (ASSUMES httplib is in a folder *next to* your project)
# $(shell ...): Runs pkg-config to get the compiler flags for libpqxx
CXXFLAGS = -std=c++17 -g -Wall -I../cpp-httplib $(shell pkg-config --cflags libpqxx)

# --- Linker Flags ---
# $(shell ...): Runs pkg-config to get the linker flags for libpqxx
# -lpthread:    Link the pthreads library (required by httplib and std::thread)
# -lssl:        Link OpenSSL
# -lcrypto:     Link OpenSSL Crypto
LDFLAGS = $(shell pkg-config --libs libpqxx) -lpthread -lssl -lcrypto

# --- Targets ---
# Define the names of the executables we want to build
TARGETS = kv_server kv_client load_generator

# --- Rules ---

# The default rule, executed when you just type 'make'
# It depends on all the targets in the TARGETS variable
all: $(TARGETS)

# Rule to build the 'kv_server' executable
# It depends on 'server.cpp'
kv_server: server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# Rule to build the 'kv_client' executable
# It depends on 'client.cpp'
kv_client: client.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# Rule to build the 'load_generator' executable
# It depends on 'load_generator.cpp'
load_generator: load_generator.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# Rule to clean up build files
clean:
	rm -f $(TARGETS)

# Tell 'make' that 'all' and 'clean' are not actual files
.PHONY: all clean