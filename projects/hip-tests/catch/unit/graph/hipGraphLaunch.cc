/*
 * Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include <hip_test_common.hh>
#include <hip_test_checkers.hh>

#include <vector>

/**
 * @addtogroup hipGraphLaunch hipGraphLaunch
 * @{
 * @ingroup GraphTest
 * `hipGraphLaunch(hipGraphExec_t graphExec, hipStream_t stream)` -
 * Launches an executable graph in a stream
 */

static void HostFunctionSetToZero(void* arg) {
  int* test_number = (int*)arg;
  (*test_number) = 0;
}

static void HostFunctionAddOne(void* arg) {
  int* test_number = (int*)arg;
  (*test_number) += 1;
}

/* create an executable graph that will set an integer pointed to by 'number' to one*/
static void CreateTestExecutableGraph(hipGraphExec_t* graph_exec, int* number) {
  hipGraph_t graph;
  hipGraphNode_t node_error;

  hipGraphNode_t node_set_zero;
  hipHostNodeParams params_set_to_zero = {HostFunctionSetToZero, number};

  hipGraphNode_t node_add_one;
  hipHostNodeParams params_set_add_one = {HostFunctionAddOne, number};

  HIP_CHECK(hipGraphCreate(&graph, 0));

  HIP_CHECK(hipGraphAddHostNode(&node_set_zero, graph, nullptr, 0, &params_set_to_zero));
  HIP_CHECK(hipGraphAddHostNode(&node_add_one, graph, &node_set_zero, 1, &params_set_add_one));

  HIP_CHECK(hipGraphInstantiate(graph_exec, graph, &node_error, nullptr, 0));
  HIP_CHECK(hipGraphDestroy(graph));
}

static void HipGraphLaunch_Positive_Simple(hipStream_t stream) {
  int number = 5;

  hipGraphExec_t graph_exec;
  CreateTestExecutableGraph(&graph_exec, &number);

  HIP_CHECK(hipGraphLaunch(graph_exec, stream));
  HIP_CHECK(hipStreamSynchronize(stream));
  REQUIRE(number == 1);

  HIP_CHECK(hipGraphExecDestroy(graph_exec));
}


/**
 * Test Description
 * ------------------------
 *    - Basic positive test for hipGraphLaunch
 *        -# stream as a created stream
 *        -# with stream as hipStreamPerThread
 * Test source
 * ------------------------
 *    - unit/graph/hipGraphLaunch.cc
 * Test requirements
 * ------------------------
 *    - HIP_VERSION >= 5.2
 */
HIP_TEST_CASE(Unit_hipGraphLaunch_Positive) {
  SECTION("stream as a created stream") {
    hipStream_t stream;
    HIP_CHECK(hipStreamCreate(&stream));
    HipGraphLaunch_Positive_Simple(stream);
    HIP_CHECK(hipStreamDestroy(stream));
  }

  SECTION("with stream as hipStreamPerThread") {
    HipGraphLaunch_Positive_Simple(hipStreamPerThread);
  }
}

/**
 * Test Description
 * ------------------------
 *    - Negative parameter test for hipGraphLaunch
 *        -# graphExec is nullptr and stream is a created stream
 *        -# graphExec is nullptr and stream is hipStreamPerThread
 *        -# graphExec is an empty object
 *        -# graphExec is destroyed before calling hipGraphLaunch
 * Test source
 * ------------------------
 *    - unit/graph/hipGraphLaunch.cc
 * Test requirements
 * ------------------------
 *    - HIP_VERSION >= 5.2
 */
HIP_TEST_CASE(Unit_hipGraphLaunch_Negative_Parameters) {
  SECTION("graphExec is nullptr and stream is a created stream") {
    hipStream_t stream;
    hipError_t ret;
    HIP_CHECK(hipStreamCreate(&stream));
    ret = hipGraphLaunch(nullptr, stream);
    HIP_CHECK(hipStreamDestroy(stream));
    REQUIRE(ret == hipErrorInvalidValue);
  }

  SECTION("graphExec is nullptr and stream is hipStreamPerThread") {
    HIP_CHECK_ERROR(hipGraphLaunch(nullptr, hipStreamPerThread), hipErrorInvalidValue);
  }

  SECTION("graphExec is an empty object") {
    hipGraphExec_t graph_exec{};
    HIP_CHECK_ERROR(hipGraphLaunch(graph_exec, hipStreamPerThread), hipErrorInvalidValue);
  }

  SECTION("graphExec is destroyed") {
    int number = 5;
    hipGraphExec_t graph_exec;
    CreateTestExecutableGraph(&graph_exec, &number);
    HIP_CHECK(hipGraphLaunch(graph_exec, hipStreamPerThread));
    HIP_CHECK(hipStreamSynchronize(hipStreamPerThread));
    REQUIRE(number == 1);
    HIP_CHECK(hipGraphExecDestroy(graph_exec));
    HIP_CHECK_ERROR(hipGraphLaunch(graph_exec, hipStreamPerThread), hipErrorInvalidValue);
  }
}

static __global__ void IndependentNodeWrite(int* out, int idx, int iters, const int* value) {
  float v = idx;
  for (int k = 0; k < iters; ++k) v = v * 1.0001f + 0.5f;
  if (threadIdx.x == 0) out[idx] = (v != -1.0f) ? *value : -1;
}

/**
 * Test Description
 * ------------------------
 *  - Launches a graph of edge-free kernel nodes that the runtime collapses onto a single
 *    stream, so the independent root segments may overlap on one HW queue.
 *  - Verifies every node observes stream work enqueued before the launch and that work
 *    enqueued after the launch observes every node, both on the same stream and through an
 *    event on another stream.
 * Test source
 * ------------------------
 *  - unit/graph/hipGraphLaunch.cc
 * Test requirements
 * ------------------------
 *  - HIP_VERSION >= 7.2
 */
HIP_TEST_CASE(Unit_hipGraphLaunch_IndependentNodes_StreamOrdering) {
  constexpr int kNodes = 20;
  int* out = nullptr;
  int* value = nullptr;
  HIP_CHECK(hipMalloc(&out, kNodes * sizeof(int)));
  HIP_CHECK(hipMalloc(&value, sizeof(int)));

  hipGraph_t graph;
  HIP_CHECK(hipGraphCreate(&graph, 0));
  std::vector<int> idx(kNodes), iters(kNodes);
  for (int i = 0; i < kNodes; ++i) {
    idx[i] = i;
    // The last node is short so it would finish first if it ignored ordering.
    iters[i] = (i == kNodes - 1) ? 1 : 200000;
    void* args[] = {&out, &idx[i], &iters[i], &value};
    hipKernelNodeParams params{};
    params.func = reinterpret_cast<void*>(IndependentNodeWrite);
    params.gridDim = dim3(1);
    params.blockDim = dim3(64);
    params.kernelParams = args;
    hipGraphNode_t node;
    HIP_CHECK(hipGraphAddKernelNode(&node, graph, nullptr, 0, &params));
  }
  hipGraphExec_t graph_exec;
  HIP_CHECK(hipGraphInstantiate(&graph_exec, graph, nullptr, nullptr, 0));

  hipStream_t stream, other;
  HIP_CHECK(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking));
  HIP_CHECK(hipStreamCreateWithFlags(&other, hipStreamNonBlocking));
  hipEvent_t event;
  HIP_CHECK(hipEventCreateWithFlags(&event, hipEventDisableTiming));

  std::vector<int> host(kNodes);
  for (int it = 1; it <= 10; ++it) {
    HIP_CHECK(hipMemcpyAsync(value, &it, sizeof(int), hipMemcpyHostToDevice, stream));
    HIP_CHECK(hipMemsetAsync(out, 0, kNodes * sizeof(int), stream));
    HIP_CHECK(hipGraphLaunch(graph_exec, stream));
    if (it % 2) {
      HIP_CHECK(
          hipMemcpyAsync(host.data(), out, kNodes * sizeof(int), hipMemcpyDeviceToHost, stream));
      HIP_CHECK(hipStreamSynchronize(stream));
    } else {
      HIP_CHECK(hipEventRecord(event, stream));
      HIP_CHECK(hipStreamWaitEvent(other, event, 0));
      HIP_CHECK(
          hipMemcpyAsync(host.data(), out, kNodes * sizeof(int), hipMemcpyDeviceToHost, other));
      HIP_CHECK(hipStreamSynchronize(other));
    }
    for (int i = 0; i < kNodes; ++i) {
      INFO("iteration " << it << " node " << i);
      REQUIRE(host[i] == it);
    }
  }

  HIP_CHECK(hipEventDestroy(event));
  HIP_CHECK(hipStreamDestroy(other));
  HIP_CHECK(hipStreamDestroy(stream));
  HIP_CHECK(hipGraphExecDestroy(graph_exec));
  HIP_CHECK(hipGraphDestroy(graph));
  HIP_CHECK(hipFree(value));
  HIP_CHECK(hipFree(out));
}

/**
 * End doxygen group GraphTest.
 * @}
 */
