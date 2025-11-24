#!/bin/bash


WORKLOAD=$1
NUM_THREADS=$2
TEST_DURATION=$3

DISK_DEVICE="sda" 
OUTPUT_FILE="results_${WORKLOAD}.csv"
LOAD_GENERATOR="./build/load_generator"

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <workload_type> <num_threads> <duration_seconds>"
    echo "Example: $0 get_popular 128 30"
    exit 1
fi


# Check dependencies
if ! command -v pidstat &> /dev/null; then
    echo "Error: 'pidstat' not found. Please install sysstat (sudo apt install sysstat)."
    exit 1
fi
if ! command -v iostat &> /dev/null; then
    echo "Error: 'iostat' not found. Please install sysstat."
    exit 1
fi

# Find Server PID
SERVER_PID=$(pgrep -f "kv_server" | head -n 1)
if [ -z "$SERVER_PID" ]; then
    echo "Error: kv_server is not running! Please start it first."
    exit 1
fi

if [ ! -f "$OUTPUT_FILE" ]; then
    echo "threads,throughput,response_time,cpu_util,disk_util" > $OUTPUT_FILE
    echo "Created new output file: $OUTPUT_FILE"
fi

echo "----------------------------------------------------"
echo "Running Single Load Test"
echo "Workload:      $WORKLOAD"
echo "Threads:       $NUM_THREADS"
echo "Duration:      $TEST_DURATION seconds"
echo "Output File:   $OUTPUT_FILE (Appending)"
echo "----------------------------------------------------"

pidstat -u -p $SERVER_PID 1 $(($TEST_DURATION + 1)) > cpu_stats.tmp &
PID_MON_PID=$!

iostat -d -x 1 $(($TEST_DURATION + 1)) | grep "$DISK_DEVICE" > disk_stats.tmp &
DISK_MON_PID=$!

output=$(taskset -c 1,2,3 $LOAD_GENERATOR $NUM_THREADS $TEST_DURATION $WORKLOAD 127.0.0.1 8080)

wait $PID_MON_PID
wait $DISK_MON_PID

throughput=$(echo "$output" | awk '/Avg Throughput/ {print $4}')
response_time=$(echo "$output" | awk '/Avg Response Time/ {print $5}')

cpu_util=$(grep "Average" cpu_stats.tmp | awk '{print $8}')

disk_util=$(awk '{ sum += $NF; n++ } END { if (n > 0) print sum / n; else print 0 }' disk_stats.tmp)

if [ -z "$throughput" ]; then throughput=0; fi
if [ -z "$response_time" ]; then response_time=0; fi
if [ -z "$cpu_util" ]; then cpu_util=0; fi
if [ -z "$disk_util" ]; then disk_util=0; fi

echo "Result:"
echo "  -> Throughput: $throughput req/s"
echo "  -> Latency:    $response_time ms"
echo "  -> CPU Util:   $cpu_util %"
echo "  -> Disk Util:  $disk_util %"
echo ""

# 5. Save to CSV
echo "$NUM_THREADS,$throughput,$response_time,$cpu_util,$disk_util" >> $OUTPUT_FILE

rm cpu_stats.tmp disk_stats.tmp

echo "Saved to $OUTPUT_FILE"