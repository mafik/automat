#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Online-learned render-cost model for frame packing.

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace automat {

struct CostModel {
  // TODO: floor_millis is redundant with always-packed RootWidget - get rid of it
  float floor_millis = 3.0f;  // learned fixed per-frame cost

  // Per-type cost in ms: cost(type, px) = rbias[type] + rweight[type] * px_Mpx
  std::unordered_map<std::string, int> type_index;
  std::vector<std::string> type_names;  // [type] -> Widget::Name(), for inspection
  std::vector<float> rbias, rweight;    // per-type fixed cost (ms) and per-Mpx cost (ms/Mpx)
  std::vector<long> tcount;             // [type] -> # training samples seen (0 = prior only)
  std::vector<float> v_b, v_w;          // RMSProp running mean-square of each per-type gradient
  std::vector<float> pxsum;             // scratch: per-type Σ px_Mpx packed this frame
  std::vector<int> occ;                 // scratch: per-type count packed this frame

  float lr_e2e = 0.01f;  // gradient step (RMSProp-scaled), shared by floor and the per-type weights
  float v_floor = 4.0f;  // RMSProp running mean-square of the intercept (floor) gradient

  // Running mean-absolute prediction error (ms)
  float mae = 1.5f;

  int TypeId(const std::string& n);

  float RenderCostMs(int t, float px2) const { return rbias[t] + rweight[t] * (px2 * 1e-6f); }

  struct Obs {
    int type;
    float px;
    float measured = -1;  // unused (kept just in case it's actually useful)
  };

  // One online RMSProp step of the regression above. Returns pred (ms), for logging.
  float TrainStep(const std::vector<Obs>& render_obs, float frame_ms);

  bool loaded = false;  // set once the persisted weights (if any) have been read

  // Persist the learned weights to a human-readable, hand-editable text file. Numeric fields come
  // first so type names (which may contain spaces) can be the rest of the line.
  void Save(const std::string& path) const;

  void Load(const std::string& path);
};

}  // namespace automat
