// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "random.hh"

#include "int.hh"
#include "log.hh"
#include "time.hh"

#ifdef __linux__
#include <sys/random.h>
#else
// clang-format off
#include <windows.h>
#include <wincrypt.h> /* CryptAcquireContext, CryptGenRandom */
// clang-format on
#endif

std::random_device rand_dev;
std::mt19937 generator(rand_dev());

namespace automat {

void RandomBytesSecure(Span<> out) {
#ifdef __linux__
  SSize n = getrandom(out.data(), out.size(), 0);
#else
  HCRYPTPROV p;
  ULONG i;

  if (CryptAcquireContext(&p, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) == FALSE) {
    FATAL << "RandomBytesSecure(): CryptAcquireContext failed.";
  }

  if (CryptGenRandom(p, out.size_bytes(), (BYTE*)out.data()) == FALSE) {
    FATAL << "RandBytes(): CryptGenRandom failed.";
  }

  CryptReleaseContext(p, 0);
#endif
}

XorShift32 XorShift32::MakeFromCurrentTime() {
  auto now = automat::time::SecondsSinceEpoch();
  return XorShift32{reinterpret_cast<U32*>(&now)[0] ^ reinterpret_cast<U32*>(&now)[1] ^ 0xdeadbeef};
}
}  // namespace automat