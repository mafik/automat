#include "path.hh"

#include <pwd.h>
#include <unistd.h>

#include "int.hh"
#include "status.hh"

namespace maf {

Path Path::ExpandUser() const {
  StrView p = str;
  if (p.starts_with("~")) {
    p.remove_prefix(1);
    if (p.empty() or p.starts_with("/")) {
      return Path(getenv("HOME") + Str(p));
    } else {
      size_t slash_pos = p.find("/");
      Str username;
      if (slash_pos == StrView::npos) {
        username = p;
      } else {
        username = p.substr(0, slash_pos);
      }
      struct passwd *pw = getpwnam(username.c_str());
      return Str(pw->pw_dir) + Str(p.substr(slash_pos));
    }
  } else {
    return *this;
  }
}

Path Path::ReadLink(Status &status) const {
  char link[PATH_MAX];
  SSize link_size = readlink(str.c_str(), link, sizeof(link));
  if (link_size < 0) {
    AppendErrorMessage(status) = "readlink(" + str + ") failed";
    return Path();
  }
  return Path(StrView(link, link_size));
}

void Path::Unlink(Status &status, bool missing_ok) const {
  int ret = unlink(str.c_str());
  if (ret < 0) {
    if (errno == ENOENT and missing_ok) {
      errno = 0;
      return;
    }
    AppendErrorMessage(status) = "unlink(" + str + ") failed";
  }
}

void Path::Rename(const Path &to, Status &status) const {
  int ret = rename(str.c_str(), to.str.c_str());
  if (ret < 0) {
    AppendErrorMessage(status) = "rename(" + str + ", " + to.str + ") failed";
  }
}

Str Path::Name() const {
  auto slash_pos = str.rfind("/");
  if (slash_pos == StrView::npos) {
    return str;
  } else {
    return str.substr(slash_pos + 1);
  }
}

Str Path::Stem() const {
  auto name = Name();
  auto dot_pos = name.rfind(".");
  if (dot_pos == StrView::npos) {
    return name;
  } else {
    return name.substr(0, dot_pos);
  }
}

Path Path::WithStem(StrView stem) const {
  auto slash_pos = str.rfind("/");
  Size stem_begin;
  if (slash_pos == StrView::npos) {
    stem_begin = 0;
  } else {
    stem_begin = slash_pos + 1;
  }
  auto dot_pos = str.rfind(".");
  Size stem_end;
  if (dot_pos == StrView::npos) {
    stem_end = str.size();
  } else {
    stem_end = dot_pos;
  }
  return Path(str.substr(0, stem_begin) + Str(stem) + str.substr(stem_end));
}

} // namespace maf