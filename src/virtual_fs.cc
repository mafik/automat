#include "virtual_fs.hh"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../build/generated/embedded.hh"

namespace maf::fs {

EmbeddedFS embedded;
RealFS real;
OverlayFS real_then_embedded;

__attribute__((constructor)) void InitFS() {
  real_then_embedded.layers.push_back(&real);
  real_then_embedded.layers.push_back(&embedded);
}

void EmbeddedFS::Map(const Path &path, Fn<void(StrView)> callback,
                     Status &status) {
  auto it = gatekeeper::embedded::index.find(path);
  if (it == gatekeeper::embedded::index.end()) {
    status() += "Embedded file not found: " + Str(path);
  } else {
    callback(it->second->content);
  }
}

Str EmbeddedFS::Read(const Path &path, Status &status) {
  auto it = gatekeeper::embedded::index.find(path);
  if (it == gatekeeper::embedded::index.end()) {
    status() += "Embedded file not found: " + Str(path);
    return "";
  } else {
    return it->second->content;
  }
}

void EmbeddedFS::Write(const Path &path, StrView contents, Status &status,
                       mode_t) {
  status() += "Writing to EmbeddedFS is not supported";
}

void RealFS::Map(const Path &path, Fn<void(StrView)> callback, Status &status) {
  int f = open(path, O_RDONLY);
  if (f == -1) {
    status() += "Failed to open " + Str(path);
    return;
  }
  struct stat buffer;
  if (fstat(f, &buffer) != 0) {
    status() += "Failed to fstat " + Str(path);
    close(f);
    return;
  }
  void *ptr = mmap(nullptr, buffer.st_size, PROT_READ, MAP_PRIVATE, f, 0);
  if (ptr == MAP_FAILED) {
    status() += "Failed to mmap " + Str(path);
    close(f);
    return;
  }
  close(f);
  callback(StrView((char *)ptr, buffer.st_size));
  munmap(ptr, buffer.st_size);
}

Str RealFS::Read(const Path &path, Status &status) {
  int f = open(path, O_RDONLY);
  if (f == -1) {
    status() += "Failed to open " + Str(path);
    return "";
  }
  Str ret;
  while (true) {
    char buf[4096];
    ssize_t n = read(f, buf, sizeof(buf));
    if (n == -1) {
      status() += "Failed to read " + Str(path);
      close(f);
      return "";
    }
    if (n == 0) {
      break;
    }
    ret += StrView(buf, n);
  }
  close(f);
  return ret;
}

void RealFS::Write(const Path &path, StrView contents, Status &status,
                   mode_t mode) {
  int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (f == -1) {
    status() += "Failed to open " + Str(path);
    return;
  }
  if (write(f, contents.data(), contents.size()) != (ssize_t)contents.size()) {
    status() += "Failed to write " + Str(path);
    close(f);
    return;
  }
  close(f);
}

void RealFS::Copy(const Path &from, const Path &to, Status &status,
                  mode_t mode) {
  int fd_from = open(from, O_RDONLY);
  if (fd_from == -1) {
    status() += "Failed to open " + Str(from);
    return;
  }
  int fd_to = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd_to == -1) {
    status() += "Failed to open " + std::string(to);
    close(fd_from);
    return;
  }
  struct stat buffer;
  if (fstat(fd_from, &buffer) != 0) {
    status() += "Failed to fstat " + std::string(from);
    close(fd_from);
    close(fd_to);
    return;
  }
  if (sendfile(fd_to, fd_from, nullptr, buffer.st_size) != buffer.st_size) {
    status() += "Failed to sendfile " + std::string(from);
    close(fd_from);
    close(fd_to);
    return;
  }
  close(fd_from);
  close(fd_to);
}

void OverlayFS::Map(const Path &path, Fn<void(StrView)> callback,
                    Status &status) {
  Status all_layers_status;
  for (auto layer : layers) {
    Status layer_status;
    layer->Map(path, callback, layer_status);
    if (OK(layer_status)) {
      return;
    } else {
      all_layers_status() += layer_status.ToStr();
    }
  }
  status() += all_layers_status.ToStr();
}

Str OverlayFS::Read(const Path &path, Status &status) {
  Status all_layers_status;
  for (auto layer : layers) {
    Status layer_status;
    Str ret = layer->Read(path, layer_status);
    if (OK(layer_status)) {
      return ret;
    } else {
      all_layers_status() += layer_status.ToStr();
    }
  }
  status() += all_layers_status.ToStr();
  return "";
}

void OverlayFS::Write(const Path &path, StrView contents, Status &status,
                      mode_t mode) {
  Status all_layers_status;
  for (auto layer : layers) {
    Status layer_status;
    layer->Write(path, contents, layer_status, mode);
    if (OK(layer_status)) {
      return;
    } else {
      all_layers_status() += layer_status.ToStr();
    }
  }
  status() += all_layers_status.ToStr();
}

void Map(VirtualFS &fs, const Path &path, Fn<void(StrView)> callback,
         Status &status) {
  auto expanded = path.ExpandUser();
  fs.Map(expanded, callback, status);
}

Str Read(VirtualFS &fs, const Path &path, Status &status) {
  auto expanded = path.ExpandUser();
  return fs.Read(expanded, status);
}

void Write(VirtualFS &fs, const Path &path, StrView contents, Status &status,
           mode_t mode) {
  auto expanded = path.ExpandUser();
  fs.Write(expanded, contents, status, mode);
}

void Copy(VirtualFS &from_fs, const Path &from, VirtualFS &to_fs,
          const Path &to, Status &status, mode_t mode) {
  if (&from_fs == &to_fs && &from_fs == &real) {
    real.Copy(from, to, status, mode);
    return;
  }
  Map(
      from_fs, from,
      [&](StrView contents) { Write(to_fs, to, contents, status); }, status);
  return;
}

} // namespace maf::fs