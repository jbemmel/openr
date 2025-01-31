/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/PrependLabelAllocator.h>

namespace openr {

PrependLabelAllocator::PrependLabelAllocator(
    std::shared_ptr<const Config> config) {
  if (config->isSegmentRoutingEnabled() &&
      config->isSegmentRoutingConfigured()) {
    const auto& srConfig = config->getSegmentRoutingConfig();
    CHECK(srConfig.prepend_label_ranges_ref().has_value());
    labelRangeV4_.first =
        *srConfig.prepend_label_ranges_ref()->v4_ref()->start_label_ref();
    labelRangeV4_.second =
        *srConfig.prepend_label_ranges_ref()->v4_ref()->end_label_ref();
    labelRangeV6_.first =
        *srConfig.prepend_label_ranges_ref()->v6_ref()->start_label_ref();
    labelRangeV6_.second =
        *srConfig.prepend_label_ranges_ref()->v6_ref()->end_label_ref();
  }
}

int32_t
PrependLabelAllocator::getNewMplsLabel(bool isV4) {
  // Return label from free range if any available
  if (isV4) {
    if (freedMplsLabelsV4_.size()) {
      int32_t ret = freedMplsLabelsV4_.back();
      freedMplsLabelsV4_.pop_back();
      return ret;
    }
    // Return next label from the v4 range
    CHECK_LE(nextMplsLabelV4_, labelRangeV4_.second)
        << "V4: Exhausted static MPLS range";
    return nextMplsLabelV4_++;
  } else {
    if (freedMplsLabelsV6_.size()) {
      int32_t ret = freedMplsLabelsV6_.back();
      freedMplsLabelsV6_.pop_back();
      return ret;
    }
    // Return next label from the v6 range
    CHECK_LE(nextMplsLabelV6_, labelRangeV6_.second)
        << "V6: Exhausted static MPLS range";
    return nextMplsLabelV6_++;
  }
}

void
PrependLabelAllocator::freeMplsLabel(
    bool isV4, int32_t label, const std::string& nh_str) {
  auto labelRange = getPrependLabelRange(isV4);
  const std::string ip_family = fmt::format("IPv{}", isV4 ? 4 : 6);
  CHECK(label >= labelRange.first and label <= labelRange.second)
      << " label " << label << " assignment to " << ip_family
      << " address is incorrect for " << nh_str;
  if (isV4) {
    freedMplsLabelsV4_.emplace_back(label);
  } else {
    freedMplsLabelsV6_.emplace_back(label);
  }
}

const std::pair<int32_t, int32_t>
PrependLabelAllocator::getPrependLabelRange(bool isV4) {
  if (isV4) {
    return labelRangeV4_;
  } else {
    return labelRangeV6_;
  }
}

int32_t
PrependLabelAllocator::decrementRefCount(
    const std::set<folly::IPAddress>& nextHopSet) {
  int32_t oldLabel = 0;
  if (nextHopSet.size()) {
    auto& [refCount, label] = getNextHopSetToLabel().at(nextHopSet);
    refCount--;
    return label;
  }
  return oldLabel;
}

void
PrependLabelAllocator::incrementRefCount(
    const std::set<folly::IPAddress>& nextHopSet) {
  if (nextHopSet.size()) {
    auto& [refCount, label] = getNextHopSetToLabel().at(nextHopSet);
    refCount++; // Increase ref-count
  }
}
} // namespace openr
