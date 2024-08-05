#include "persistence.hh"

#include <rapidjson/prettywriter.h>
#include <rapidjson/rapidjson.h>

#include "root.hh"
#include "status.hh"
#include "virtual_fs.hh"
#include "window.hh"

using namespace maf;

namespace automat {

Path StatePath() { return Path::ExecutablePath().Parent() / "automat_state.json"; }

void SaveState(gui::Window& window, Status& status) {
  // Write window_state to a temp file
  auto state_path = StatePath();
  rapidjson::StringBuffer sb;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
  writer.SetMaxDecimalPlaces(6);
  writer.StartObject();
  writer.Key("version");
  writer.Uint(1);
  writer.Key("window");
  window.SerializeState(writer);
  root_machine->SerializeState(writer, "root");

  writer.EndObject();
  writer.Flush();
  std::string window_state = sb.GetString();
  fs::real.Write(state_path, window_state, status);
}

void LoadState(gui::Window& window, Status& status) {
  auto state_path = StatePath();
  auto contents = fs::real.Read(state_path, status);
  if (!OK(status)) {
    AppendErrorMessage(status) += "Failed to read automat state";
  }
  rapidjson::InsituStringStream stream(const_cast<char*>(contents.c_str()));
  Deserializer d(stream);

  for (auto& key : ObjectView(d, status)) {
    if (key == "version") {
      int version = d.GetInt(status);
      if (!OK(status)) {
        AppendErrorMessage(status) += "Failed to deserialize version";
        return;
      } else if (version != 1) {
        AppendErrorMessage(status) += "Unsupported version: " + std::to_string(version);
        return;
      }
    } else if (key == "window") {
      window.DeserializeState(d, status);
      if (!OK(status)) {
        AppendErrorMessage(status) += "Failed to deserialize window state";
        return;
      }
    } else if (key == "root") {
      root_machine->DeserializeState(root_location, d);
    } else {
      AppendErrorMessage(status) += "Unexpected key: " + key;
      return;
    }
  }
  bool fully_decoded = d.reader.IterativeParseComplete();
  if (!fully_decoded) {
    AppendErrorMessage(status) += "Extra data at the end of the JSON string, " + d.ErrorContext();
    return;
  }

  if (!OK(status)) {
    AppendErrorMessage(status) += "Failed to deserialize saved state";
  }
}
}  // namespace automat