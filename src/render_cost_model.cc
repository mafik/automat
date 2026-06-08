// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "render_cost_model.hh"

#include <fstream>
#include <sstream>

namespace automat {

int CostModel::TypeId(const std::string& n) {
  auto it = type_index.find(n);
  if (it != type_index.end()) return it->second;
  int id = (int)rbias.size();
  type_index.emplace(n, id);
  type_names.push_back(n);
  rbias.push_back(0.6f);    // prior fixed cost (~one cheap widget, ms) ...
  rweight.push_back(0.4f);  // ... plus a mild O(pixels) slope; nonzero so they stay learnable
  tcount.push_back(0);
  v_b.push_back(4.0f);
  v_w.push_back(4.0f);
  pxsum.push_back(0);
  occ.push_back(0);
  return id;
}

static float Clip(float v, float lim) { return v > lim ? lim : (v < -lim ? -lim : v); }

float CostModel::TrainStep(const std::vector<Obs>& render_obs, float frame_ms) {
  for (size_t t = 0; t < rbias.size(); ++t) {
    occ[t] = 0;
    pxsum[t] = 0;
  }
  float R = 0;
  for (const auto& o : render_obs) {
    float px = o.px * 1e-6f;
    R += rbias[o.type] + rweight[o.type] * px;
    ++occ[o.type];
    pxsum[o.type] += px;
  }
  float pred = floor_millis + R;
  float err = Clip(pred - frame_ms, 50.0f);
  float ae = err < 0 ? -err : err;
  if (ae > 5.0f) ae = 5.0f;  // clip the rare big spike so the steady margin tracks typical error
  mae = 0.97f * mae + 0.03f * ae;
  // RMSProp per parameter. The gradient of the squared error w.r.t. each parameter is err * its
  // feature (d pred / d param): 1 for floor, occ[t] for rbias[t], pxsum[t] for rweight[t].
  // Dividing by the gradient's running RMS gives ~unit-scale steps regardless of how often a
  // type is packed.
  float g_floor = err;
  v_floor = 0.9f * v_floor + 0.1f * g_floor * g_floor;
  floor_millis -= lr_e2e * g_floor / (sqrtf(v_floor) + 1e-3f);
  if (floor_millis < 0) floor_millis = 0;
  for (size_t t = 0; t < rbias.size(); ++t) {
    if (occ[t] == 0) continue;
    float g_b = err * occ[t];
    float g_w = err * pxsum[t];
    v_b[t] = 0.9f * v_b[t] + 0.1f * g_b * g_b;
    v_w[t] = 0.9f * v_w[t] + 0.1f * g_w * g_w;
    rbias[t] -= lr_e2e * g_b / (sqrtf(v_b[t]) + 1e-3f);
    rweight[t] -= lr_e2e * g_w / (sqrtf(v_w[t]) + 1e-3f);
    if (rbias[t] < 0) rbias[t] = 0;  // costs are non-negative (keeps the budget well-defined)
    if (rweight[t] < 0) rweight[t] = 0;
    tcount[t] += occ[t];
  }
  return pred;
}

void CostModel::Save(const std::string& path) const {
  std::ofstream f(path);
  if (!f) return;
  f << "# automat render cost model\n";
  f << "floor " << floor_millis << "\n";
  f << "mae " << mae << "\n";
  f << "# type  rbias(ms)  rweight(ms/Mpx)  trained_on  name\n";
  for (size_t t = 0; t < rbias.size(); ++t)
    f << "type " << rbias[t] << " " << rweight[t] << " " << tcount[t] << " " << type_names[t]
      << "\n";
}

void CostModel::Load(const std::string& path) {
  std::ifstream f(path);
  if (!f) return;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string key;
    ss >> key;
    if (key == "floor")
      ss >> floor_millis;
    else if (key == "mae")
      ss >> mae;  // legacy "render_scale"/"glitch_scale" lines are ignored
    else if (key == "type") {
      float rb = 0, rw = 0;
      long tc = 0;
      ss >> rb >> rw >> tc;
      std::string name;
      std::getline(ss, name);
      size_t p = name.find_first_not_of(" \t");
      if (p == std::string::npos) continue;
      name = name.substr(p);
      int id = TypeId(name);  // creates the slot with priors, then overwrite from the file
      rbias[id] = rb < 0 ? 0 : rb;
      rweight[id] = rw < 0 ? 0 : rw;
      tcount[id] = tc;
    }
  }
}

}  // namespace automat