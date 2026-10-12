#include "graphics/host_gpu/renderer/meshDispatch.h"
#include "graphics/host_gpu/renderer/meshRestart.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <utility>
#include <unordered_set>

namespace {

using Libs::Graphics::MeshDispatchSlice;
using Libs::Graphics::SplitMeshDispatch;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "MeshDispatchTests: failed: %s\n", message);
    std::abort();
  }
}

// Verifies that the slices are within every host limit, do not overlap, and
// cover the requested grid exactly once in Vulkan's flattened rasterization
// order.
void CheckCoverage(const std::vector<MeshDispatchSlice> &slices,
                   uint32_t groups, uint32_t instances, uint32_t max_groups,
                   uint32_t max_instances, uint32_t max_total) {
  std::unordered_set<uint64_t> covered;
  covered.reserve(static_cast<size_t>(groups) * instances);
  uint64_t next_flattened_index = 0;
  for (const auto &slice : slices) {
    Check(slice.group_count != 0u && slice.instance_count != 0u,
          "a slice has an empty dimension");
    Check(slice.group_offset <= groups && slice.instance_offset <= instances &&
              slice.group_count <= groups - slice.group_offset &&
              slice.instance_count <= instances - slice.instance_offset,
          "a slice runs past the requested grid");
    Check(max_groups == 0u || slice.group_count <= max_groups,
          "a slice exceeds the group dimension limit");
    Check(max_instances == 0u || slice.instance_count <= max_instances,
          "a slice exceeds the instance dimension limit");
    Check(max_total == 0u ||
              static_cast<uint64_t>(slice.group_count) * slice.instance_count <=
                  max_total,
          "a slice exceeds the total workgroup limit");
    // A native dispatch rasterizes X first, then Y. Convert each local
    // workgroup back to the original draw's grid before checking its order.
    for (uint32_t instance = slice.instance_offset;
         instance < slice.instance_offset + slice.instance_count; instance++) {
      for (uint32_t group = slice.group_offset;
           group < slice.group_offset + slice.group_count; group++) {
        const auto flattened_index =
            static_cast<uint64_t>(instance) * groups + group;
        Check(flattened_index == next_flattened_index++,
              "slices change flattened workgroup rasterization order");
        const auto cell = (static_cast<uint64_t>(group) << 32) | instance;
        Check(covered.insert(cell).second, "slices overlap or repeat a cell");
      }
    }
  }
  Check(covered.size() == static_cast<size_t>(groups) * instances,
        "slices do not cover the requested grid exactly once");
}

void TestWithinLimits() {
  const auto slices = SplitMeshDispatch(100, 50, 65535, 65535, 65535);
  Check(slices.size() == 1, "a draw within limits was split");
  Check(slices[0].group_offset == 0u && slices[0].group_count == 100u &&
            slices[0].instance_offset == 0u && slices[0].instance_count == 50u,
        "within-limits slice does not match the draw");
  CheckCoverage(slices, 100, 50, 65535, 65535, 65535);
}

void TestInstanceCountAbovePerDimensionLimit() {
  // Astro's Playroom reports "1x68734" on AMD: one group, 68734 instances,
  // beyond the 65535 per-dimension limit reported by the host.
  const auto slices = SplitMeshDispatch(1, 68734, 65535, 65535, 65535);
  Check(slices.size() == 2u, "oversized instance count did not split");
  Check(slices[0].group_count == 1u && slices[0].instance_count == 65535u,
        "first instance slice is wrong");
  Check(slices[1].instance_offset == 65535u &&
            slices[1].instance_count == 3199u,
        "trailing instance slice is wrong");
  CheckCoverage(slices, 1, 68734, 65535, 65535, 65535);
}

void TestGroupCountAbovePerDimensionLimit() {
  const auto slices = SplitMeshDispatch(200000, 7, 65535, 65535, 65535);
  // Each of seven instances must finish all four X chunks (65535, 65535,
  // 65535, 3395) before the next instance starts rasterizing.
  Check(slices.size() == 28u,
        "oversized group count produced the wrong slice count");
  CheckCoverage(slices, 200000, 7, 65535, 65535, 65535);
}

void TestGroupSlicesPreserveInstanceOrder() {
  // Batching both instances into each X chunk would rasterize global IDs
  // 0,1,4,5,2,3,6,7 instead of 0,1,2,3,4,5,6,7.
  const auto slices = SplitMeshDispatch(4, 2, 2, 2, 4);
  CheckCoverage(slices, 4, 2, 2, 2, 4);
  Check(slices.size() == 4u,
        "split X ranges must finish one instance before batching another");
}

void TestTotalCountCapsInstanceChunks() {
  const auto slices = SplitMeshDispatch(100, 1000, 65535, 65535, 65535);
  // The total limit of 65535 allows at most 655 instances per 100-group slice.
  Check(slices.size() == 2u,
        "total-limit capping produced the wrong slice count");
  Check(slices[0].instance_count == 655u && slices[1].instance_count == 345u,
        "total-limit capping split instances incorrectly");
  CheckCoverage(slices, 100, 1000, 65535, 65535, 65535);
}

void TestDegenerateCounts() {
  Check(SplitMeshDispatch(0, 10, 65535, 65535, 65535).empty(),
        "zero groups produced a non-empty dispatch");
  Check(SplitMeshDispatch(10, 0, 65535, 65535, 65535).empty(),
        "zero instances produced a non-empty dispatch");
  const auto single = SplitMeshDispatch(1, 1, 65535, 65535, 65535);
  Check(single.size() == 1u && single[0].group_count == 1u &&
            single[0].instance_count == 1u,
        "single-workgroup draw was not preserved");
}

void TestTotalLimitTighterThanGroupLimit() {
  // A host can report maxMeshWorkGroupCount[0] above maxMeshWorkGroupTotalCount
  // (the total constraint is independent per the Vulkan spec). The group chunk
  // must be clamped to the total limit, or a single slice could exceed it.
  const auto tighter_groups = SplitMeshDispatch(4, 1, 4, 1, 3);
  Check(tighter_groups.size() == 2u,
        "group stride above the total limit was not clamped");
  CheckCoverage(tighter_groups, 4, 1, 4, 1, 3);
  const auto mixed = SplitMeshDispatch(6, 2, 4, 2, 3);
  Check(mixed.size() == 4u, "total-limited slicing is wrong");
  CheckCoverage(mixed, 6, 2, 4, 2, 3);
}

void TestZeroAndTinyLimitsClampToOne() {
  // Hosts must report limits of at least one, but the splitting accepts zero
  // and treats it as one instead of dividing by zero.
  const auto one_group = SplitMeshDispatch(4, 1, 0, 0, 0);
  Check(one_group.size() == 4u, "zero limits did not clamp to one");
  CheckCoverage(one_group, 4, 1, 0, 0, 0);
  const auto tiny_limits = SplitMeshDispatch(3, 300, 2, 1, 2);
  Check(tiny_limits.size() == 600u,
        "tiny limits produced the wrong slice count");
  CheckCoverage(tiny_limits, 3, 300, 2, 1, 2);
}

void TestNonDividingStridesConsumeExactRemainders() {
  // Strides are host limits that need not divide the guest counts (e.g. a
  // stride of 2 against odd group/instance counts). Each iteration must
  // consume exactly the count it covers: with groups or instances == UINT32_MAX
  // (odd) and a host-reported stride of 2, advancing by the raw stride would
  // wrap the uint32 counter and loop forever. The full UINT32_MAX input cannot
  // be enumerated in a unit test, but termination depends on the exact
  // remainders verified here.
  const auto slices = SplitMeshDispatch(7, 5, 2, 2, 10);
  CheckCoverage(slices, 7, 5, 2, 2, 10);
  const auto &tail = slices.back();
  Check(tail.group_offset == 6u && tail.group_count == 1u,
        "trailing group remainder is wrong");
  bool saw_last_remainder = false;
  for (const auto &slice : slices) {
    if (slice.group_offset == 6u && slice.instance_offset == 4u) {
      Check(slice.instance_count == 1u, "trailing instance remainder is wrong");
      saw_last_remainder = true;
    }
  }
  Check(saw_last_remainder, "trailing instance remainder is missing");

  // Full-X dispatches still batch instances and must consume their remainder.
  const auto instance_slices = SplitMeshDispatch(2, 5, 2, 2, 10);
  CheckCoverage(instance_slices, 2, 5, 2, 2, 10);
  Check(instance_slices.back().instance_offset == 4u &&
            instance_slices.back().instance_count == 1u,
        "batched instances did not consume the exact trailing remainder");
}


// ---- Primitive restart -------------------------------------------------------------------------

using Libs::Graphics::MeshRestartSegment;
using Libs::Graphics::SplitAtPrimitiveRestart;

using Triangle = std::array<uint32_t, 3>;

// What hardware draws for an indexed triangle strip with restart: a marker ends the strip, the
// next index begins a new one with fresh winding.
std::vector<Triangle> ReferenceStrip(const std::vector<uint32_t> &indices,
                                     uint32_t marker) {
  std::vector<Triangle> out;
  uint32_t run = 0;
  for (size_t i = 0; i < indices.size(); i++) {
    if (indices[i] == marker) {
      run = 0;
      continue;
    }
    run++;
    if (run >= 3) {
      const auto a = indices[i - 2], b = indices[i - 1], c = indices[i];
      out.push_back((run & 1u) ? Triangle{a, b, c} : Triangle{b, a, c});
    }
  }
  return out;
}

std::vector<Triangle> ReferenceFan(const std::vector<uint32_t> &indices,
                                   uint32_t marker) {
  std::vector<Triangle> out;
  uint32_t run = 0, centre = 0;
  for (size_t i = 0; i < indices.size(); i++) {
    if (indices[i] == marker) {
      run = 0;
      continue;
    }
    run++;
    if (run == 1) {
      centre = indices[i];
    } else if (run >= 3) {
      out.push_back({centre, indices[i - 1], indices[i]});
    }
  }
  return out;
}

// The mesh shader assembles a strip or fan from the first index of whatever draw it is given,
// with winding and centre taken from that start. Model that, then draw each run on its own.
std::vector<Triangle> MeshStrip(const std::vector<uint32_t> &indices) {
  std::vector<Triangle> out;
  for (size_t p = 0; p + 2 < indices.size(); p++) {
    const auto a = indices[p], b = indices[p + 1], c = indices[p + 2];
    out.push_back((p & 1u) ? Triangle{b, a, c} : Triangle{a, b, c});
  }
  return out;
}

std::vector<Triangle> MeshFan(const std::vector<uint32_t> &indices) {
  std::vector<Triangle> out;
  for (size_t p = 0; p + 2 < indices.size(); p++) {
    out.push_back({indices[0], indices[p + 1], indices[p + 2]});
  }
  return out;
}

template <typename Index>
std::vector<Triangle> DrawSegmented(const std::vector<uint32_t> &wide, bool fan) {
  std::vector<Index> narrow(wide.begin(), wide.end());
  const uint32_t marker = static_cast<Index>(~Index{0});
  const auto segments = SplitAtPrimitiveRestart(narrow.data(), sizeof(Index),
                                                static_cast<uint32_t>(narrow.size()), marker);
  std::vector<Triangle> out;
  for (const auto &run : segments) {
    const std::vector<uint32_t> part(wide.begin() + run.first,
                                     wide.begin() + run.first + run.count);
    const auto tris = fan ? MeshFan(part) : MeshStrip(part);
    out.insert(out.end(), tris.begin(), tris.end());
  }
  return out;
}

void TestRestartSegments() {
  const std::vector<uint16_t> idx = {0, 1, 2, 3, 0xffff, 4, 5, 6, 0xffff, 0xffff, 7, 8, 9, 0xffff};
  const auto seg = SplitAtPrimitiveRestart(idx.data(), 2, static_cast<uint32_t>(idx.size()), 0xffffu);
  Check(seg.size() == 3u, "wrong number of runs");
  Check(seg[0].first == 0u && seg[0].count == 4u, "first run is wrong");
  Check(seg[1].first == 5u && seg[1].count == 3u, "second run is wrong");
  Check(seg[2].first == 10u && seg[2].count == 3u, "third run is wrong");
  Check(SplitAtPrimitiveRestart(idx.data(), 2, 0, 0xffffu).empty(), "empty draw has runs");
  const std::vector<uint16_t> only = {0xffff, 0xffff};
  Check(SplitAtPrimitiveRestart(only.data(), 2, 2, 0xffffu).empty(),
        "markers only produced a run");
  const std::vector<uint16_t> none = {1, 2, 3};
  const auto whole = SplitAtPrimitiveRestart(none.data(), 2, 3, 0xffffu);
  Check(whole.size() == 1u && whole[0].first == 0u && whole[0].count == 3u,
        "a draw without markers must stay whole");
  const std::vector<uint8_t> bytes = {1, 2, 0xff, 3, 4, 5};
  const auto b = SplitAtPrimitiveRestart(bytes.data(), 1, 6, 0xffu);
  Check(b.size() == 2u && b[1].first == 3u && b[1].count == 3u, "8-bit markers are wrong");
  // A 32-bit index that merely contains 0xffff is not a marker.
  const std::vector<uint32_t> wide = {0xffffu, 0xffffffffu, 7u};
  const auto w = SplitAtPrimitiveRestart(wide.data(), 4, 3, 0xffffffffu);
  Check(w.size() == 2u && w[0].count == 1u && w[1].first == 2u, "32-bit markers are wrong");
  Check(SplitAtPrimitiveRestart(nullptr, 2, 4, 0xffffu).empty(), "null indices produced runs");
  Check(SplitAtPrimitiveRestart(idx.data(), 3, 4, 0xffffu).empty(), "bad element size produced runs");
}

void TestRestartMatchesHardwareAssembly() {
  // Runs of odd and even length: strip winding must restart per run (an odd run shifts parity
  // for everything after it if the restart is ignored).
  const std::vector<uint32_t> idx = {0, 1, 2, 3, 4, 0xffff, 10, 11, 12, 13, 0xffff,
                                     20, 21, 22, 0xffff, 30, 31};
  const auto strip_ref = ReferenceStrip(idx, 0xffffu);
  const auto fan_ref = ReferenceFan(idx, 0xffffu);
  Check(DrawSegmented<uint16_t>(idx, false) == strip_ref,
        "segmented strip differs from hardware strip assembly");
  Check(DrawSegmented<uint16_t>(idx, true) == fan_ref,
        "segmented fan differs from hardware fan assembly");
  // The unsplit draw reads markers as vertices and uses one centre and one parity.
  Check(MeshStrip(idx) != strip_ref, "the unsplit strip unexpectedly matches (test is vacuous)");
  Check(MeshFan(idx) != fan_ref, "the unsplit fan unexpectedly matches (test is vacuous)");
}

} // namespace

int main() {
  TestWithinLimits();
  TestInstanceCountAbovePerDimensionLimit();
  TestGroupCountAbovePerDimensionLimit();
  TestGroupSlicesPreserveInstanceOrder();
  TestTotalCountCapsInstanceChunks();
  TestDegenerateCounts();
  TestTotalLimitTighterThanGroupLimit();
  TestZeroAndTinyLimitsClampToOne();
  TestNonDividingStridesConsumeExactRemainders();
  TestRestartSegments();
  TestRestartMatchesHardwareAssembly();
  std::puts("MeshDispatchTests: all cases passed");
  return 0;
}
