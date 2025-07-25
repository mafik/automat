// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "persistence.hh"

#include <rapidjson/prettywriter.h>
#include <rapidjson/rapidjson.h>

#include "automat.hh"
#include "root_widget.hh"
#include "status.hh"
#include "virtual_fs.hh"

namespace automat {

Path StatePath() { return Path::ExecutablePath().Parent() / "automat_state.json"; }

void SaveState(gui::RootWidget& root_widget, Status& status) {
  // Write window_state to a temp file
  auto state_path = StatePath();
  rapidjson::StringBuffer sb;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
  writer.SetMaxDecimalPlaces(6);
  writer.StartObject();
  writer.Key("version");
  writer.Uint(2);
  writer.Key("window");
  root_widget.SerializeState(writer);
  root_machine->SerializeState(writer, "root");

  writer.EndObject();
  writer.Flush();
  std::string window_state = sb.GetString();
  fs::real.Write(state_path, window_state, status);
}

void LoadState(gui::RootWidget& root_widget, Status& status) {
  auto state_path = StatePath();
  auto contents = fs::real.Read(state_path, status);
  if (!OK(status)) {
    status.Reset();
    contents = fs::embedded.Read(Path("assets") / "automat_state.json", status);
    if (!OK(status)) {
      return;
    }
  }
  rapidjson::InsituStringStream stream(const_cast<char*>(contents.c_str()));
  Deserializer d(stream);

  for (auto& key : ObjectView(d, status)) {
    if (key == "version") {
      int version;
      d.Get(version, status);
      if (OK(status) && version != 2) {
        AppendErrorMessage(status) += "Unsupported version: " + std::to_string(version);
      }
    } else if (key == "window") {
      root_widget.DeserializeState(d, status);
    } else if (key == "root") {
      root_machine->DeserializeState(*root_location, d);
    }
  }

  bool fully_decoded = d.reader.IterativeParseComplete();
  if (!fully_decoded) {
    AppendErrorMessage(status) += "Extra data at the end of the JSON string, " + d.ErrorContext();
  }
}
}  // namespace automat