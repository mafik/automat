#pragma once

#include "virtual_fs.hh"

namespace automat::audio {

void Init(int* argc, char*** argv);

void Play(maf::fs::VFile&);

}  // namespace automat::audio