/*
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <amd_smi_test/test_base.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <variant>
#include <vector>

#include "../test_common.h"
#include "rocm_smi/rocm_smi_dyn_gpu_metrics.h"
#include "rocm_smi/rocm_smi_gpu_metrics.h"

namespace amd::smi {

// Forward declarations of internal helpers we exercise in this unit-test.
AMDGpuMetricVersionFlags_t translate_header_to_flag_version(
    const AMDGpuMetricsHeader_v1_t& metrics_header, bool is_partition_metrics,
    const std::string& file_path);

GpuMetricsBasePtr amdgpu_metrics_factory(AMDGpuMetricVersionFlags_t gpu_metric_version,
                                         bool is_partition_metrics, const std::string& file_path);

}  // namespace amd::smi

namespace {
// Version helper checker
auto GetExpectedMetricVersionFlag(uint16_t major, uint16_t minor, bool is_partition_metrics)
    -> amd::smi::AMDGpuMetricVersionFlags_t {
  using Flag = amd::smi::AMDGpuMetricVersionFlags_t;
  if (is_partition_metrics) {
    if (major == 1) {
      if (minor == 0) {
        return Flag::kGpuXcpMetricV10;
      } else if (minor >= 1) {
        return Flag::kGpuXcpMetricDynV11Plus;
      } else {
        return Flag::kGpuMetricNone;
      }
    }
  } else {  // GPU metrics
    if (major == 1) {
      switch (minor) {
        case 0:
          return Flag::kGpuMetricV10;
        case 1:
          return Flag::kGpuMetricV11;
        case 2:
          return Flag::kGpuMetricV12;
        case 3:
          return Flag::kGpuMetricV13;
        case 4:
          return Flag::kGpuMetricV14;
        case 5:
          return Flag::kGpuMetricV15;
        case 6:
          return Flag::kGpuMetricV16;
        case 7:
          return Flag::kGpuMetricV17;
        case 8:
          return Flag::kGpuMetricV18;
        default:
          return Flag::kGpuMetricDynV19Plus;
      }
    }
  }
  return Flag::kGpuMetricNone;
}

// pass a header we want to test against
auto BuildFakeMetricsBlob(amd::smi::AMDGpuMetricsHeader_v1_t new_header) -> std::vector<uint8_t> {
  if (new_header.m_structure_size < sizeof(new_header)) {
    throw std::runtime_error("Header size too small");
  }
  amd::smi::AMDGpuMetricsHeader_v1_t header{};
  header.m_structure_size = static_cast<uint16_t>(sizeof(header));
  header.m_format_revision = new_header.m_format_revision;
  header.m_content_revision = new_header.m_content_revision;

  const uint8_t* begin = reinterpret_cast<const uint8_t*>(&header);
  return std::vector<uint8_t>(begin, begin + sizeof(header));
}

auto WriteBlobToTempFile(const std::vector<uint8_t>& blob,
                         const std::string& filename = "amdsmi_fake_metrics.bin")
    -> std::filesystem::path {
  auto temp_dir = std::filesystem::temp_directory_path();
  auto file_path = temp_dir / filename;

  std::ofstream stream(file_path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(blob.data()),
               static_cast<std::streamsize>(blob.size()));
  stream.close();

  return file_path;
}

}  // namespace

TEST(AmdSmiDynamicMetricTest, GPUMetricDynamicVersionSupported) {
  PRINT_VERBOSITY();
  const bool is_partition_metrics = false;
  for (auto ver : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18}) {
    std::string test_detail = "[GPUMetric";
    if (ver >= 9) {
      test_detail += "Dynamic] ";
    } else {
      test_detail += "Static] ";
    }
    std::cout << test_detail << "Checking version 1." << ver << std::endl;
    SCOPED_TRACE(testing::Message() << "Subtest for minor version: 1." << ver);
    const auto blob = BuildFakeMetricsBlob(amd::smi::AMDGpuMetricsHeader_v1_t{
        .m_structure_size = sizeof(amd::smi::AMDGpuMetricsHeader_v1_t),
        .m_format_revision = 1,
        .m_content_revision = static_cast<uint8_t>(ver),  // Known minor versions
    });
    const auto fake_path =
        WriteBlobToTempFile(blob, "amdsmi_fake_gpu_metrics_v1" + std::to_string(ver) + ".bin");

    ASSERT_FALSE(blob.empty());
    ASSERT_TRUE(std::filesystem::exists(fake_path));

    const auto* header = reinterpret_cast<const amd::smi::AMDGpuMetricsHeader_v1_t*>(blob.data());
    const auto flag = amd::smi::translate_header_to_flag_version(*header, is_partition_metrics,
                                                                 fake_path.string());
    EXPECT_EQ(flag,
              GetExpectedMetricVersionFlag(1, static_cast<uint16_t>(ver), is_partition_metrics))
        << "Version 1." << ver << " should be treated as supported";

    auto gpu_metrics_ptr =
        amd::smi::amdgpu_metrics_factory(flag, is_partition_metrics, fake_path.string());
    EXPECT_NE(gpu_metrics_ptr, nullptr)
        << "Factory must create metrics object for supported version";

    if (gpu_metrics_ptr) {
      std::cout << test_detail << "Created valid object for version 1." << ver << std::endl;
    } else {
      std::cout << test_detail << "Unsupported Metric Version"
                << " | Failed to create valid object for version 1." << ver << std::endl;
    }

    std::filesystem::remove(fake_path);
  }
}

TEST(AmdSmiDynamicMetricTest, XCPMetricDynamicVersionSupported) {
  PRINT_VERBOSITY();
  const bool is_partition_metrics = true;
  for (auto ver : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18}) {
    std::string test_detail = "[XCPMetric";
    if (ver >= 1) {
      test_detail += "Dynamic] ";
    } else {
      test_detail += "Static] ";
    }
    std::cout << test_detail << "Checking version 1." << ver << std::endl;
    SCOPED_TRACE(testing::Message() << "Subtest for minor version: 1." << ver);
    const auto blob = BuildFakeMetricsBlob(amd::smi::AMDGpuMetricsHeader_v1_t{
        .m_structure_size = sizeof(amd::smi::AMDGpuMetricsHeader_v1_t),
        .m_format_revision = 1,
        .m_content_revision = static_cast<uint8_t>(ver),  // Known minor versions
    });
    const auto fake_path =
        WriteBlobToTempFile(blob, "amdsmi_fake_xcp_metrics_v1" + std::to_string(ver) + ".bin");

    ASSERT_FALSE(blob.empty());
    ASSERT_TRUE(std::filesystem::exists(fake_path));

    const auto* header = reinterpret_cast<const amd::smi::AMDGpuMetricsHeader_v1_t*>(blob.data());
    const auto flag = amd::smi::translate_header_to_flag_version(*header, is_partition_metrics,
                                                                 fake_path.string());
    EXPECT_EQ(flag,
              GetExpectedMetricVersionFlag(1, static_cast<uint16_t>(ver), is_partition_metrics))
        << "Version 1." << ver << " should be treated as supported";

    auto xcp_metrics_ptr =
        amd::smi::amdgpu_metrics_factory(flag, is_partition_metrics, fake_path.string());
    EXPECT_NE(xcp_metrics_ptr, nullptr)
        << "Factory must create metrics object for supported version";

    if (xcp_metrics_ptr) {
      std::cout << test_detail << "Created valid object for version 1." << ver << std::endl;
    } else {
      std::cout << test_detail << "Failed to create valid object for version 1." << ver
                << std::endl;
    }

    std::filesystem::remove(fake_path);
  }
}

// ---------------------------------------------------------------------------
// Helper: append the raw bytes of a trivially-copyable value to a byte vector
// ---------------------------------------------------------------------------
namespace {
template <typename T>
void append_bytes(std::vector<std::byte>& buf, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  const auto* src = reinterpret_cast<const std::byte*>(&value);
  buf.insert(buf.end(), src, src + sizeof(T));
}

// Build a minimal dynamic-metrics blob containing a single attribute.
//   attr_type_val  — raw integer value of AMDGpuMetricAttributeType_t
//   attr_id_val    — raw integer value of AMDGpuMetricAttributeId_t
//   payload        — the value to embed
//   payload_bytes  — sizeof the payload type (4 or 8)
auto BuildSingleAttrBlob(uint64_t attr_type_val, uint64_t attr_id_val, uint64_t payload,
                         std::size_t payload_bytes) -> std::vector<std::byte> {
  std::vector<std::byte> blob;

  // Header: format 1, content 19 (dynamic path)
  amd::smi::details::AMDGpuDynamicMetricsHeader_v1_t hdr{};
  hdr.m_structure_size = static_cast<uint16_t>(sizeof(hdr));
  hdr.m_format_revision = 1;
  hdr.m_content_revision = 19;
  append_bytes(blob, hdr);

  // attr_count = 1
  append_bytes(blob, uint32_t{1});

  // Encoded attribute descriptor: unit=0, type, id, instances=1
  const uint64_t enc =
      amd::smi::details::amdgpu_metrics_encode_attr(0, attr_type_val, attr_id_val, 1);
  append_bytes(blob, enc);

  // Payload (little-endian, only payload_bytes bytes)
  for (std::size_t i = 0; i < payload_bytes; ++i) {
    blob.push_back(static_cast<std::byte>((payload >> (8 * i)) & 0xFF));
  }

  return blob;
}
}  // namespace

// ---------------------------------------------------------------------------
// ROCM-21475: ACCUMULATION_COUNTER emitted as TYPE_UINT32 by older drivers
// must be accepted, parsed, and widened to uint64_t before storage.
// ---------------------------------------------------------------------------
TEST(AmdSmiDynamicMetricTest, AccumulationCounterUint32WidenedToUint64) {
  PRINT_VERBOSITY();
  using namespace amd::smi::details;

  constexpr uint32_t kTestValue = 0xDEADBEEFu;

  const auto blob =
      BuildSingleAttrBlob(static_cast<uint64_t>(AMDGpuMetricAttributeType_t::TYPE_UINT32),
                          static_cast<uint64_t>(AMDGpuMetricAttributeId_t::ACCUMULATION_COUNTER),
                          kTestValue, sizeof(uint32_t));

  amd::smi::AMDGpuDynamicMetrics_t metrics;
  const auto status =
      metrics.parse_from_buffer(reinterpret_cast<const std::byte*>(blob.data()), blob.size());

  ASSERT_EQ(status, RSMI_STATUS_SUCCESS) << "parse_from_buffer must succeed for TYPE_UINT32 "
                                            "ACCUMULATION_COUNTER (older driver compat)";

  const auto& rows = metrics.get_metric_rows();
  ASSERT_EQ(rows.size(), 1u) << "Exactly one attribute row expected";

  const auto& row = rows[0];
  EXPECT_EQ(row.m_instance.m_attribute_type, AMDGpuMetricAttributeType_t::TYPE_UINT64)
      << "Canonical type must be TYPE_UINT64 after widening";
  ASSERT_TRUE(std::holds_alternative<uint64_t>(row.m_value))
      << "Stored variant must hold uint64_t after widening from uint32_t";
  EXPECT_EQ(std::get<uint64_t>(row.m_value), static_cast<uint64_t>(kTestValue))
      << "Widened value must equal the original uint32_t value";
}

// ---------------------------------------------------------------------------
// ROCM-21475: ACCUMULATION_COUNTER emitted as TYPE_UINT64 by newer drivers
// must parse without widening and preserve values exceeding UINT32_MAX.
// ---------------------------------------------------------------------------
TEST(AmdSmiDynamicMetricTest, AccumulationCounterUint64ParsesWithoutWidening) {
  PRINT_VERBOSITY();
  using namespace amd::smi::details;

  // Value intentionally exceeds UINT32_MAX to prove no truncation occurred
  constexpr uint64_t kTestValue = 0x1'0000'0001ULL;

  const auto blob =
      BuildSingleAttrBlob(static_cast<uint64_t>(AMDGpuMetricAttributeType_t::TYPE_UINT64),
                          static_cast<uint64_t>(AMDGpuMetricAttributeId_t::ACCUMULATION_COUNTER),
                          kTestValue, sizeof(uint64_t));

  amd::smi::AMDGpuDynamicMetrics_t metrics;
  const auto status =
      metrics.parse_from_buffer(reinterpret_cast<const std::byte*>(blob.data()), blob.size());

  ASSERT_EQ(status, RSMI_STATUS_SUCCESS) << "parse_from_buffer must succeed for TYPE_UINT64 "
                                            "ACCUMULATION_COUNTER (current driver)";

  const auto& rows = metrics.get_metric_rows();
  ASSERT_EQ(rows.size(), 1u) << "Exactly one attribute row expected";

  const auto& row = rows[0];
  EXPECT_EQ(row.m_instance.m_attribute_type, AMDGpuMetricAttributeType_t::TYPE_UINT64)
      << "Canonical type must be TYPE_UINT64";
  ASSERT_TRUE(std::holds_alternative<uint64_t>(row.m_value)) << "Stored variant must hold uint64_t";
  EXPECT_EQ(std::get<uint64_t>(row.m_value), kTestValue)
      << "Value must be preserved exactly (no truncation to uint32_t)";
}

// ---------------------------------------------------------------------------
// When an attribute's emitted type is not in the schema's accepted-type
// list (NOT_SUPPORTED), the parser must skip that attribute's payload and
// continue — subsequent valid attributes must still appear in the output.
// ---------------------------------------------------------------------------
TEST(AmdSmiDynamicMetricTest, TypeMismatchIsSkippedAndParseResumes) {
  PRINT_VERBOSITY();
  using namespace amd::smi::details;

  // Build a two-attribute blob:
  //   Attr 1: ACCUMULATION_COUNTER with TYPE_INT32 — not in {TYPE_UINT32, TYPE_UINT64}
  //           → schema lookup returns NOT_SUPPORTED → must be skipped
  //   Attr 2: CURR_SOCKET_POWER with TYPE_UINT16 — correct type
  //           → must appear in output
  constexpr uint16_t kSocketPowerValue = 500u;  // 500 W

  std::vector<std::byte> blob;

  // Header
  AMDGpuDynamicMetricsHeader_v1_t hdr{};
  hdr.m_structure_size = static_cast<uint16_t>(sizeof(hdr));
  hdr.m_format_revision = 1;
  hdr.m_content_revision = 19;
  append_bytes(blob, hdr);

  // attr_count = 2
  append_bytes(blob, uint32_t{2});

  // Attr 1: ACCUMULATION_COUNTER / TYPE_INT32 (wrong type — neither UINT32 nor UINT64)
  append_bytes(blob,
               amdgpu_metrics_encode_attr(
                   0, static_cast<uint64_t>(AMDGpuMetricAttributeType_t::TYPE_INT32),
                   static_cast<uint64_t>(AMDGpuMetricAttributeId_t::ACCUMULATION_COUNTER), 1));
  // INT32 payload (4 bytes) — value does not matter, must be skipped
  append_bytes(blob, uint32_t{0xCAFEBABEu});

  // Attr 2: CURR_SOCKET_POWER / TYPE_UINT16 (correct type)
  append_bytes(blob, amdgpu_metrics_encode_attr(
                         0, static_cast<uint64_t>(AMDGpuMetricAttributeType_t::TYPE_UINT16),
                         static_cast<uint64_t>(AMDGpuMetricAttributeId_t::CURR_SOCKET_POWER), 1));
  append_bytes(blob, kSocketPowerValue);

  amd::smi::AMDGpuDynamicMetrics_t metrics;
  const auto status =
      metrics.parse_from_buffer(reinterpret_cast<const std::byte*>(blob.data()), blob.size());

  ASSERT_EQ(status, RSMI_STATUS_SUCCESS)
      << "parse_from_buffer must succeed even when one attribute is skipped";

  const auto& rows = metrics.get_metric_rows();
  ASSERT_EQ(rows.size(), 1u) << "Only the valid attribute should produce a row; "
                                "the type-mismatched attribute must be skipped";

  const auto& row = rows[0];
  EXPECT_EQ(row.m_instance.m_attribute_id, AMDGpuMetricAttributeId_t::CURR_SOCKET_POWER)
      << "Surviving row must be CURR_SOCKET_POWER";
  EXPECT_EQ(row.m_instance.m_attribute_type, AMDGpuMetricAttributeType_t::TYPE_UINT16)
      << "Canonical type must be TYPE_UINT16";
  ASSERT_TRUE(std::holds_alternative<uint16_t>(row.m_value)) << "Stored variant must hold uint16_t";
  EXPECT_EQ(std::get<uint16_t>(row.m_value), kSocketPowerValue)
      << "CURR_SOCKET_POWER value must be preserved exactly";
}
