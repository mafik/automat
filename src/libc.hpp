#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "status.hpp"

// Provides access to host's libc (even when statically linked).

namespace automat::libc {

void Init(Status&);

void* OpenLibrary(const char* name, int mode);
void* FindSymbol(void* library, const char* name);
int CloseLibrary(void* library);
const char* LibraryError();

}  // namespace automat::libc
