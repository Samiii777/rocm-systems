/*
 * Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

/**
Test Case Scenarios :
Negative -
1) Pass graphExec as nullptr and verify api returns error code.
2) Pass path as nullptr and verify api returns error code.
Functional -
1) Create a graph with kernel nodes that have pointer args, by-value uint args,
   and a by-value uint16_t[8] array arg. Dump the graph exec and verify:
   - JSON file is created with correct structure
   - Pointer args are marked as "PTR"
   - By-value uint args contain correct hex values
   - By-value array arg contains correct hex bytes
   - Dispatch dimensions match what was set
   - Dependencies are correct
   - Code object ELF files are written
2) Test with multiple kernel nodes and dependencies between them.
*/

#include <hip_test_common.hh>

#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

// Kernel with pointer args + by-value uint32_t scalar
__global__ void kernel_scalar(float* output, const float* input, uint32_t count, uint32_t multiplier) {
  uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < count) {
    output[i] = input[i] * multiplier;
  }
}

// Struct to pass uint16_t[8] as a by-value argument
struct ShortArray8 {
  uint16_t data[8];
};

// Kernel with pointer arg + by-value uint16_t[8] array arg + by-value uint32_t
__global__ void kernel_array_arg(float* output, ShortArray8 coeffs, uint32_t n) {
  uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) {
    // Use coefficients from the by-value array
    uint32_t idx = i % 8;
    output[i] = static_cast<float>(coeffs.data[idx]);
  }
}

// Helper: check if a file exists
static bool fileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

// Helper: read entire file to string
static std::string readFile(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Helper: find a JSON string value for a key (simple substring search)
static std::string findJsonValue(const std::string& json, const std::string& key) {
  std::string searchKey = "\"" + key + "\": ";
  auto pos = json.find(searchKey);
  if (pos == std::string::npos) return "";
  pos += searchKey.size();
  if (json[pos] == '"') {
    // String value
    pos++;
    auto end = json.find('"', pos);
    return json.substr(pos, end - pos);
  } else {
    // Numeric or bool
    auto end = json.find_first_of(",}\n", pos);
    return json.substr(pos, end - pos);
  }
}

// Helper: count occurrences of a substring
static int countOccurrences(const std::string& str, const std::string& sub) {
  int count = 0;
  size_t pos = 0;
  while ((pos = str.find(sub, pos)) != std::string::npos) {
    count++;
    pos += sub.size();
  }
  return count;
}

// Helper: remove dump directory recursively
static void cleanupDumpDir(const std::string& path) {
  std::string cmd = "rm -rf " + path;
  system(cmd.c_str());
}

/* Test verifies hipExtGraphExecDump API Negative scenarios. */
HIP_TEST_CASE(Unit_hipExtGraphExecDump_Negative) {
  hipGraph_t graph;
  hipGraphExec_t graphExec;
  HIP_CHECK(hipGraphCreate(&graph, 0));

  // Add an empty node so the graph can be instantiated
  hipGraphNode_t emptyNode;
  HIP_CHECK(hipGraphAddEmptyNode(&emptyNode, graph, nullptr, 0));
  HIP_CHECK(hipGraphInstantiate(&graphExec, graph, nullptr, nullptr, 0));

  SECTION("Pass graphExec as nullptr") {
    HIP_CHECK_ERROR(hipExtGraphExecDump(nullptr, "/tmp/test_dump_neg", 0xF),
                    hipErrorInvalidValue);
  }

  SECTION("Pass path as nullptr") {
    HIP_CHECK_ERROR(hipExtGraphExecDump(graphExec, nullptr, 0xF),
                    hipErrorInvalidValue);
  }

  HIP_CHECK(hipGraphExecDestroy(graphExec));
  HIP_CHECK(hipGraphDestroy(graph));
}

/* Test verifies hipExtGraphExecDump captures by-value scalar args correctly. */
HIP_TEST_CASE(Unit_hipExtGraphExecDump_ScalarArgs) {
  constexpr int N = 1024;
  constexpr int blockSize = 256;
  constexpr int gridSize = (N + blockSize - 1) / blockSize;
  const std::string dumpPath = "/tmp/test_graph_dump_scalar";
  cleanupDumpDir(dumpPath);

  float *d_input, *d_output;
  HIP_CHECK(hipMalloc(&d_input, N * sizeof(float)));
  HIP_CHECK(hipMalloc(&d_output, N * sizeof(float)));

  hipGraph_t graph;
  HIP_CHECK(hipGraphCreate(&graph, 0));

  // Add kernel node with pointer + by-value uint32_t args
  uint32_t count = N;        // 0x00000400
  uint32_t multiplier = 42;  // 0x0000002a
  void* args[] = { &d_output, &d_input, &count, &multiplier };

  hipKernelNodeParams kparams{};
  kparams.func = reinterpret_cast<void*>(kernel_scalar);
  kparams.gridDim = dim3(gridSize);
  kparams.blockDim = dim3(blockSize);
  kparams.kernelParams = reinterpret_cast<void**>(args);
  kparams.sharedMemBytes = 0;

  hipGraphNode_t kNode;
  HIP_CHECK(hipGraphAddKernelNode(&kNode, graph, nullptr, 0, &kparams));

  // Instantiate and dump
  hipGraphExec_t graphExec;
  HIP_CHECK(hipGraphInstantiate(&graphExec, graph, nullptr, nullptr, 0));
  HIP_CHECK(hipExtGraphExecDump(graphExec, dumpPath.c_str(), hipExtGraphExecDumpFlagsAll));

  // Verify output files exist
  REQUIRE(fileExists(dumpPath + "/graph_exec.json"));
  REQUIRE(fileExists(dumpPath + "/codeobjects"));

  // Read and verify JSON
  std::string json = readFile(dumpPath + "/graph_exec.json");
  REQUIRE(!json.empty());

  // Verify node count
  REQUIRE(findJsonValue(json, "node_count") == "1");
  REQUIRE(findJsonValue(json, "kernel_node_count") == "1");

  // Verify kernel name contains "kernel_scalar"
  INFO("JSON content: " << json);
  REQUIRE(json.find("kernel_scalar") != std::string::npos);

  // Verify dispatch dimensions
  REQUIRE(json.find("\"gridDim\": { \"x\": 4, \"y\": 1, \"z\": 1 }") != std::string::npos);
  REQUIRE(json.find("\"blockDim\": { \"x\": 256, \"y\": 1, \"z\": 1 }") != std::string::npos);

  // Verify pointer args are marked as PTR (2 pointer args: output, input)
  REQUIRE(countOccurrences(json, "\"kind\": \"pointer\"") == 2);
  REQUIRE(countOccurrences(json, "\"value\": \"PTR\"") == 2);

  // Verify by-value args are captured
  REQUIRE(countOccurrences(json, "\"kind\": \"value\"") == 2);

  // Verify count = 1024 = 0x00000400
  REQUIRE(json.find("\"0x00000400\"") != std::string::npos);

  // Verify multiplier = 42 = 0x0000002a
  REQUIRE(json.find("\"0x0000002a\"") != std::string::npos);

  // Verify code object file exists
  REQUIRE(json.find("codeobjects/codeobj_") != std::string::npos);

  // Cleanup
  HIP_CHECK(hipGraphExecDestroy(graphExec));
  HIP_CHECK(hipGraphDestroy(graph));
  HIP_CHECK(hipFree(d_input));
  HIP_CHECK(hipFree(d_output));
  cleanupDumpDir(dumpPath);
}

/* Test verifies hipExtGraphExecDump captures a by-value uint16_t[8] array arg. */
HIP_TEST_CASE(Unit_hipExtGraphExecDump_ArrayArg) {
  constexpr int N = 256;
  constexpr int blockSize = 64;
  constexpr int gridSize = (N + blockSize - 1) / blockSize;
  const std::string dumpPath = "/tmp/test_graph_dump_array";
  cleanupDumpDir(dumpPath);

  float* d_output;
  HIP_CHECK(hipMalloc(&d_output, N * sizeof(float)));

  hipGraph_t graph;
  HIP_CHECK(hipGraphCreate(&graph, 0));

  // Set up the uint16_t[8] array with known values
  ShortArray8 coeffs;
  coeffs.data[0] = 0x1111;
  coeffs.data[1] = 0x2222;
  coeffs.data[2] = 0x3333;
  coeffs.data[3] = 0x4444;
  coeffs.data[4] = 0x5555;
  coeffs.data[5] = 0x6666;
  coeffs.data[6] = 0x7777;
  coeffs.data[7] = 0x8888;
  uint32_t n = N;

  void* args[] = { &d_output, &coeffs, &n };

  hipKernelNodeParams kparams{};
  kparams.func = reinterpret_cast<void*>(kernel_array_arg);
  kparams.gridDim = dim3(gridSize);
  kparams.blockDim = dim3(blockSize);
  kparams.kernelParams = reinterpret_cast<void**>(args);
  kparams.sharedMemBytes = 0;

  hipGraphNode_t kNode;
  HIP_CHECK(hipGraphAddKernelNode(&kNode, graph, nullptr, 0, &kparams));

  // Instantiate and dump
  hipGraphExec_t graphExec;
  HIP_CHECK(hipGraphInstantiate(&graphExec, graph, nullptr, nullptr, 0));
  HIP_CHECK(hipExtGraphExecDump(graphExec, dumpPath.c_str(), hipExtGraphExecDumpFlagsAll));

  // Read JSON
  std::string json = readFile(dumpPath + "/graph_exec.json");
  REQUIRE(!json.empty());

  INFO("JSON content: " << json);

  // Verify kernel name
  REQUIRE(json.find("kernel_array_arg") != std::string::npos);

  // Verify pointer arg (d_output)
  REQUIRE(countOccurrences(json, "\"kind\": \"pointer\"") == 1);

  // Verify by-value args: the struct (16 bytes) + uint32_t n
  // The struct ShortArray8 is 16 bytes (8 x uint16_t)
  // The uint16_t[8] array with values {0x1111, 0x2222, ..., 0x8888} should appear
  // as a hex string in little-endian format
  int valueArgCount = countOccurrences(json, "\"kind\": \"value\"");
  REQUIRE(valueArgCount >= 2);  // At least the struct + n

  // Verify n = 256 = 0x00000100
  REQUIRE(json.find("\"0x00000100\"") != std::string::npos);

  // Verify the array bytes are present in hex
  // Little-endian: 0x1111 stored as 11 11, 0x2222 as 22 22, etc.
  // Full 16-byte hex (big-endian display): 0x88887777666655554444333322221111
  REQUIRE(json.find("88887777666655554444333322221111") != std::string::npos);

  // Cleanup
  HIP_CHECK(hipGraphExecDestroy(graphExec));
  HIP_CHECK(hipGraphDestroy(graph));
  HIP_CHECK(hipFree(d_output));
  cleanupDumpDir(dumpPath);
}

/* Test verifies hipExtGraphExecDump with multiple nodes and dependencies. */
HIP_TEST_CASE(Unit_hipExtGraphExecDump_MultiNodeDeps) {
  constexpr int N = 512;
  constexpr int blockSize = 128;
  constexpr int gridSize = (N + blockSize - 1) / blockSize;
  const std::string dumpPath = "/tmp/test_graph_dump_multi";
  cleanupDumpDir(dumpPath);

  float *d_buf1, *d_buf2;
  HIP_CHECK(hipMalloc(&d_buf1, N * sizeof(float)));
  HIP_CHECK(hipMalloc(&d_buf2, N * sizeof(float)));

  hipGraph_t graph;
  HIP_CHECK(hipGraphCreate(&graph, 0));

  // Node 0: kernel_scalar (no deps)
  uint32_t count = N;
  uint32_t mult1 = 7;
  void* args0[] = { &d_buf1, &d_buf2, &count, &mult1 };
  hipKernelNodeParams kp0{};
  kp0.func = reinterpret_cast<void*>(kernel_scalar);
  kp0.gridDim = dim3(gridSize);
  kp0.blockDim = dim3(blockSize);
  kp0.kernelParams = reinterpret_cast<void**>(args0);

  hipGraphNode_t node0;
  HIP_CHECK(hipGraphAddKernelNode(&node0, graph, nullptr, 0, &kp0));

  // Node 1: kernel_scalar (depends on node0)
  uint32_t mult2 = 13;
  void* args1[] = { &d_buf2, &d_buf1, &count, &mult2 };
  hipKernelNodeParams kp1{};
  kp1.func = reinterpret_cast<void*>(kernel_scalar);
  kp1.gridDim = dim3(gridSize);
  kp1.blockDim = dim3(blockSize);
  kp1.kernelParams = reinterpret_cast<void**>(args1);

  hipGraphNode_t node1;
  HIP_CHECK(hipGraphAddKernelNode(&node1, graph, &node0, 1, &kp1));

  // Node 2: kernel_array_arg (depends on node0, parallel with node1)
  ShortArray8 coeffs;
  for (int i = 0; i < 8; i++) coeffs.data[i] = static_cast<uint16_t>(i + 1);
  uint32_t n2 = N;
  void* args2[] = { &d_buf2, &coeffs, &n2 };
  hipKernelNodeParams kp2{};
  kp2.func = reinterpret_cast<void*>(kernel_array_arg);
  kp2.gridDim = dim3(gridSize);
  kp2.blockDim = dim3(blockSize);
  kp2.kernelParams = reinterpret_cast<void**>(args2);

  hipGraphNode_t node2;
  HIP_CHECK(hipGraphAddKernelNode(&node2, graph, &node0, 1, &kp2));

  // Instantiate and dump
  hipGraphExec_t graphExec;
  HIP_CHECK(hipGraphInstantiate(&graphExec, graph, nullptr, nullptr, 0));
  HIP_CHECK(hipExtGraphExecDump(graphExec, dumpPath.c_str(), hipExtGraphExecDumpFlagsAll));

  // Read JSON
  std::string json = readFile(dumpPath + "/graph_exec.json");
  REQUIRE(!json.empty());

  INFO("JSON content: " << json);

  // Verify 3 nodes
  REQUIRE(findJsonValue(json, "node_count") == "3");
  REQUIRE(findJsonValue(json, "kernel_node_count") == "3");

  // Verify dependencies are present
  REQUIRE(json.find("\"dependencies\": []") != std::string::npos);  // node0 has no deps
  // node1 and node2 both depend on node0
  REQUIRE(countOccurrences(json, "\"dependencies\": [0]") == 2);

  // Verify both kernel names appear
  REQUIRE(json.find("kernel_scalar") != std::string::npos);
  REQUIRE(json.find("kernel_array_arg") != std::string::npos);

  // Verify by-value args: mult1=7 (0x00000007), mult2=13 (0x0000000d)
  REQUIRE(json.find("\"0x00000007\"") != std::string::npos);
  REQUIRE(json.find("\"0x0000000d\"") != std::string::npos);

  // Cleanup
  HIP_CHECK(hipGraphExecDestroy(graphExec));
  HIP_CHECK(hipGraphDestroy(graph));
  HIP_CHECK(hipFree(d_buf1));
  HIP_CHECK(hipFree(d_buf2));
  cleanupDumpDir(dumpPath);
}

/* Test verifies flags control what gets dumped. */
HIP_TEST_CASE(Unit_hipExtGraphExecDump_FlagsControl) {
  constexpr int N = 64;
  const std::string dumpPath = "/tmp/test_graph_dump_flags";
  cleanupDumpDir(dumpPath);

  float* d_buf;
  HIP_CHECK(hipMalloc(&d_buf, N * sizeof(float)));

  hipGraph_t graph;
  HIP_CHECK(hipGraphCreate(&graph, 0));

  uint32_t count = N;
  uint32_t mult = 1;
  void* args[] = { &d_buf, &d_buf, &count, &mult };
  hipKernelNodeParams kp{};
  kp.func = reinterpret_cast<void*>(kernel_scalar);
  kp.gridDim = dim3(1);
  kp.blockDim = dim3(N);
  kp.kernelParams = reinterpret_cast<void**>(args);

  hipGraphNode_t node;
  HIP_CHECK(hipGraphAddKernelNode(&node, graph, nullptr, 0, &kp));

  hipGraphExec_t graphExec;
  HIP_CHECK(hipGraphInstantiate(&graphExec, graph, nullptr, nullptr, 0));

  SECTION("Dump with no flags - only basic node info") {
    cleanupDumpDir(dumpPath);
    HIP_CHECK(hipExtGraphExecDump(graphExec, dumpPath.c_str(), hipExtGraphExecDumpFlagsNone));
    std::string json = readFile(dumpPath + "/graph_exec.json");
    REQUIRE(!json.empty());
    // Should not contain arguments, dispatch, or code objects
    REQUIRE(json.find("arguments") == std::string::npos);
    REQUIRE(json.find("gridDim") == std::string::npos);
    REQUIRE(json.find("codeobjects") == std::string::npos);
    REQUIRE(json.find("dependencies") == std::string::npos);
  }

  SECTION("Dump with only dispatch flag") {
    cleanupDumpDir(dumpPath);
    HIP_CHECK(hipExtGraphExecDump(graphExec, dumpPath.c_str(), hipExtGraphExecDumpFlagsDispatch));
    std::string json = readFile(dumpPath + "/graph_exec.json");
    REQUIRE(!json.empty());
    REQUIRE(json.find("gridDim") != std::string::npos);
    REQUIRE(json.find("arguments") == std::string::npos);
    REQUIRE(json.find("codeobjects/") == std::string::npos);
  }

  SECTION("Dump with only args flag") {
    cleanupDumpDir(dumpPath);
    HIP_CHECK(hipExtGraphExecDump(graphExec, dumpPath.c_str(), hipExtGraphExecDumpFlagsKernelArgs));
    std::string json = readFile(dumpPath + "/graph_exec.json");
    REQUIRE(!json.empty());
    REQUIRE(json.find("arguments") != std::string::npos);
    REQUIRE(json.find("gridDim") == std::string::npos);
  }

  HIP_CHECK(hipGraphExecDestroy(graphExec));
  HIP_CHECK(hipGraphDestroy(graph));
  HIP_CHECK(hipFree(d_buf));
  cleanupDumpDir(dumpPath);
}
