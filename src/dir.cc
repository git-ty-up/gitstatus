// Copyright 2019 Roman Perepelitsa.
//
// This file is part of GitStatus.
//
// GitStatus is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// GitStatus is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with GitStatus. If not, see <https://www.gnu.org/licenses/>.

#include "dir.h"

#include <algorithm>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <endian.h>
#include <sys/syscall.h>
#endif

#include "check.h"
#include "scope_guard.h"
#include "string_cmp.h"

namespace gitstatus {

namespace {

bool Dots(const char* name) {
  if (name[0] == '.') {
    if (name[1] == 0) return true;
    if (name[1] == '.' && name[2] == 0) return true;
  }
  return false;
}

}  // namespace

// The linux-specific implementation is about 20% faster than the generic (posix) implementation.
#ifdef __linux__

uint64_t Read64(const void* p) {
  uint64_t res;
  std::memcpy(&res, p, 8);
  return res;
}

void Write64(uint64_t x, void* p) { std::memcpy(p, &x, 8); }

void SwapBytes(char** begin, char** end) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  for (; begin != end; ++begin) Write64(__builtin_bswap64(Read64(*begin)), *begin);
#elif __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "sorry, not implemented"
#endif
}

template <bool kCaseSensitive>
void SortEntries(char** begin, char** end) {
  static_assert(kCaseSensitive, "");
  SwapBytes(begin, end);
  std::sort(begin, end, [](const char* a, const char* b) {
    uint64_t x = Read64(a);
    uint64_t y = Read64(b);
    // Add 5 for good luck.
    return x < y || (x == y && std::memcmp(a + 5, b + 5, 256) < 0);
  });
  SwapBytes(begin, end);
}

template <>
void SortEntries<false>(char** begin, char** end) {
  std::sort(begin, end, StrLt<false>());
}

bool ListDir(int dir_fd, Arena& arena, std::vector<char*>& entries, bool case_sensitive) {
  struct linux_dirent64 {
    ino64_t d_ino;
    off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
  };

  constexpr size_t kBufSize = 8 << 10;
  entries.clear();

  while (true) {
    char* buf = static_cast<char*>(arena.Allocate(kBufSize, alignof(linux_dirent64)));
    // Save 256 bytes for the rainy day.
    int n = syscall(SYS_getdents64, dir_fd, buf, kBufSize - 256);
    if (n < 0) {
      entries.clear();
      return false;
    }
    if (n == 0) break;
    for (int pos = 0; pos < n;) {
      auto* ent = reinterpret_cast<linux_dirent64*>(buf + pos);
      if (!Dots(ent->d_name)) entries.push_back(ent->d_name);
      pos += ent->d_reclen;
      // It's tempting to bail here if n + sizeof(linux_dirent64) + 512 <= n. After all, there
      // was enough space for another entry but SYS_getdents64 didn't write it, so this must be
      // the end of the directory listing, right? Unfortuatenly, no. SYS_getdents64 is finicky.
      // It sometimes writes a partial list of entries even if the full list would fit.
    }
  }

  if (case_sensitive) {
    SortEntries<true>(entries.data(), entries.data() + entries.size());
  } else {
    SortEntries<false>(entries.data(), entries.data() + entries.size());
  }

  return true;
}

#else

bool ListDir(int dir_fd, Arena& arena, std::vector<char*>& entries, bool case_sensitive) {
  VERIFY((dir_fd = dup(dir_fd)) >= 0);
  DIR* dir = fdopendir(dir_fd);
  if (!dir) {
    CHECK(!close(dir_fd)) << Errno();
    return -1;
  }
  ON_SCOPE_EXIT(&) { CHECK(!closedir(dir)) << Errno(); };
  entries.clear();
  while (struct dirent* ent = (errno = 0, readdir(dir))) {
    if (Dots(ent->d_name)) continue;
    size_t len = std::strlen(ent->d_name);
    char* p = arena.Allocate<char>(len + 2);
    *p++ = ent->d_type;
    std::memcpy(p, ent->d_name, len + 1);
    entries.push_back(p);
  }
  if (errno) {
    entries.clear();
    return false;
  }
  StrSort(entries.data(), entries.data() + entries.size(), case_sensitive);
  return true;
}

#endif

}  // namespace gitstatus
