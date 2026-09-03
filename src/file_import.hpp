#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <atomic>

#include "path.hpp"
#include "status.hpp"
#include "str.hpp"

namespace automat {

Path AutomatDir();

Path UniqueNameIn(const Path& dir, StrView basename);

bool SameFilesystem(const Path& a, const Path& b);

uint64_t FileSize(const Path&);

void CopyInto(const Path& src, const Path& dst, Status&, std::atomic<uint64_t>* progress = nullptr);

void WriteInto(const Path& dst, StrView bytes, Status&);

Path PathFromFileURI(StrView uri, Status&);

Str FileURIFromPath(StrView path);

}  // namespace automat
