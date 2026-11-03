#include "iqk/loader.hpp"

#include <algorithm>

#include "iqk/config.hpp"
#include "iqk/icd/aam.hpp"

namespace iqk {

namespace aam = iqk::icd::aam;

namespace {
constexpr std::uint8_t kWrite = 0x23;
constexpr std::uint8_t kRead = 0x40;
constexpr std::uint8_t kWriteAck = 0x60;
constexpr std::uint8_t kReadValue = 0x43;
constexpr std::uint8_t kAbort = 0x80;
}  // namespace

void PlanLoader::start(const plan::Plan& p, std::int64_t now) noexcept {
  ops_.clear();
  nodes_.clear();
  const auto regs = plan::registers(p);
  for (const auto& r : regs) {
    if (std::find(nodes_.begin(), nodes_.end(), r.node) == nodes_.end()) {
      nodes_.push_back(r.node);
    }
    ops_.push_back({true, r, 0});
  }
  for (const int n : nodes_) {
    ops_.push_back({true, {n, aam::kRegCommitIndex, aam::kRegCommitSub, 1U}, 0});
  }
  for (const auto& r : regs) {
    ops_.push_back({false, r, r.value});
  }
  for (const int n : nodes_) {
    ops_.push_back({false, {n, aam::kRegConfigCrcIndex, aam::kRegConfigCrcSub, 0U}, plan::expected_module_crc(p, n)});
  }
  pos_ = 0;
  attempts_ = 0;
  waiting_ = false;
  sent_at_ = now;
  failure_ = Failure::none;
  status_ = ops_.empty() ? Status::done : Status::busy;
}

void PlanLoader::fail(Failure f, std::uint32_t detail) noexcept {
  status_ = Status::failed;
  failure_ = f;
  failed_node_ = ops_[pos_].r.node;
  detail_ = detail;
  waiting_ = false;
}

std::optional<CanFrame> PlanLoader::poll(std::int64_t now) noexcept {
  if (status_ != Status::busy) {
    return std::nullopt;
  }
  if (waiting_) {
    if (now - sent_at_ < cfg::plan::reg_timeout_ms * 1'000'000) {
      return std::nullopt;
    }
    if (attempts_ > cfg::plan::reg_retries) {  // first try plus reg_retries retries
      fail(Failure::timeout, static_cast<std::uint32_t>(pos_));
      return std::nullopt;
    }
  }
  const Op& op = ops_[pos_];
  aam::AamRegRequest req{};
  req.cmd = op.write ? kWrite : kRead;
  req.index = op.r.index;
  req.sub = op.r.sub;
  req.value = op.write ? op.r.value : 0U;
  CanFrame f{};
  f.id = aam::AamRegRequest::can_id(static_cast<std::uint8_t>(op.r.node));
  f.dlc = static_cast<std::uint8_t>(aam::encode(req, f.data));
  ++attempts_;
  waiting_ = true;
  sent_at_ = now;
  return f;
}

void PlanLoader::on_frame(const CanFrame& f, std::int64_t now) noexcept {
  if (status_ != Status::busy || !waiting_) {
    return;
  }
  const Op& op = ops_[pos_];
  if (f.id != aam::AamRegResponse::can_id(static_cast<std::uint8_t>(op.r.node))) {
    return;
  }
  aam::AamRegResponse r{};
  if (!aam::decode(f.payload(), r) || r.index != op.r.index || r.sub != op.r.sub) {
    return;  // a late answer to an earlier request; keep waiting
  }
  if (r.cmd == kAbort) {
    fail(Failure::abort, r.value);
    return;
  }
  if (op.write ? r.cmd != kWriteAck : r.cmd != kReadValue) {
    return;
  }
  if (!op.write && r.value != op.expect) {
    fail(op.r.index == aam::kRegConfigCrcIndex ? Failure::crc_mismatch : Failure::readback_mismatch, r.value);
    return;
  }
  ++pos_;
  attempts_ = 0;
  waiting_ = false;
  sent_at_ = now;
  if (pos_ == ops_.size()) {
    status_ = Status::done;
  }
}

}  // namespace iqk
