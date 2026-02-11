// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "persistence.hh"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/rapidjson.h>

#include "argument.hh"
#include "automat.hh"
#include "root_widget.hh"
#include "status.hh"
#include "virtual_fs.hh"

namespace automat {

Path StatePath() { return Path::ExecutablePath().Parent() / "automat_state.json"; }

void SaveState(ui::RootWidget& root_widget, Status& status) {
  // Write window_state to a temp file
  auto state_path = StatePath();
  rapidjson::StringBuffer sb;
  ObjectSerializer writer(sb);
  writer.SetMaxDecimalPlaces(6);
  writer.StartObject();
  writer.Key("version");
  writer.Uint(2);
  writer.Key("window");
  root_widget.SerializeState(writer);

  writer.assigned_names.emplace("version");
  writer.assigned_names.emplace("window");
  writer.Serialize(*root_machine);

  writer.EndObject();
  writer.Flush();
  std::string window_state = sb.GetString();
  fs::real.Write(state_path, window_state, status);
}

void LoadState(ui::RootWidget& root_widget, Status& status) {
  auto state_path = StatePath();
  auto contents = fs::real.Read(state_path, status);
  if (!OK(status)) {
    status.Reset();
    contents = fs::embedded.Read(Path("assets") / "automat_state.json", status);
    if (!OK(status)) {
      return;
    }
  }

  rapidjson::StringStream stream(contents.data());
  ObjectDeserializer d(stream);

  // First pass: Create objects and register them by name
  // We use a lookahead deserializer to parse ahead without consuming the main stream
  {
    rapidjson::StringStream lookahead_stream(contents.data());
    Deserializer lookahead(lookahead_stream);

    for (auto& key : ObjectView(lookahead, status)) {
      if (key == "version") {
        int version;
        lookahead.Get(version, status);
        if (OK(status) && version != 2) {
          AppendErrorMessage(status) += "Unsupported version: " + std::to_string(version);
        }
      } else if (key == "window") {
        root_widget.DeserializeState(lookahead, status);
      } else {
        // This is an object definition - find the type and create it
        for (auto& field : ObjectView(lookahead, status)) {
          if (field == "type") {
            Str type;
            lookahead.Get(type, status);
            if (OK(status)) {
              if (type == "Machine") {
                d.RegisterObject(key, *root_machine);
              } else {
                auto proto = prototypes->Find(type);
                if (proto == nullptr) {
                  root_machine->ReportError(f("Unknown object type: {}", type));
                } else {
                  auto object = proto->Clone();
                  d.RegisterObject(key, *object);
                }
              }
            }
          } else {
            lookahead.Skip();  // skip other fields in first pass
          }
        }
      }
    }
  }

  // Second pass: Deserialize object states and connections
  for (auto& key : ObjectView(d, status)) {
    if (key == "version") {
      d.Skip();  // already handled
    } else if (key == "window") {
      d.Skip();  // already handled
    } else {
      // This is an object - deserialize its state
      auto* object = d.LookupObject(key);
      if (object) {
        for (auto& field : ObjectView(d, status)) {
          if (field == "type") {
            d.Skip();  // Already handled during object creation in first pass
          } else if (field == "links") {
            // Deserialize argument connections
            for (auto& arg_name : ObjectView(d, status)) {
              Argument* from_arg = dynamic_cast<Argument*>(object->AtomFromName(arg_name));
              if (from_arg) {
                Str to_name;
                d.Get(to_name, status);
                auto to_atom = d.LookupAtom(to_name);
                if (to_atom) {
                  from_arg->Connect(*object, to_atom);
                }
              } else {
                d.Skip();
              }
            }
          } else if (!object->DeserializeKey(d, field)) {
            d.Skip();  // Unknown field, skip it
          }
        }
      } else {
        d.Skip();
      }
    }
  }

  // Objects may have been rendered in their incomplete state - re-render them all.
  for (auto& loc : root_machine->locations) {
    loc->WakeToys();
  }

  bool fully_decoded = d.reader.IterativeParseComplete();
  if (!fully_decoded) {
    AppendErrorMessage(status) += "Extra data at the end of the JSON string, " + d.ErrorContext();
  }
}
}  // namespace automat
