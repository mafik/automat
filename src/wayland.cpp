// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "wayland.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "../build/generated/wayland_protocols.hpp"
#include "format.hpp"
#include "log.hpp"
#include "wayland_ext.hpp"

namespace automat::wayland {

Colony<Client> Client::colony;

Common::Common(Kind kind, U32 id, Client& client) : kind(kind), id(id), client(client) {
  client.SetId(id, this);
}

Common::~Common() {
  client.SetId(id, nullptr);
  if (id < 0xff000000)
    if (Display* display = static_cast<Display*>(client.GetId(1))) display->DeleteId(id);
}

void Display::OnSync(Callback& callback) {
  callback.Done(0);
  callback.ColonyDestroy();
}

void Display::OnGetRegistry(Registry& registry) {
  registry.Global(1, "wl_compositor", 6);
  registry.Global(2, "wl_subcompositor", 1);
  registry.Global(3, "wl_shm", 1);
  registry.Global(4, "wl_seat", 7);
  registry.Global(5, "wl_output", 4);
  registry.Global(6, "wl_data_device_manager", 3);
  registry.Global(7, "xdg_wm_base", 6);
  registry.Global(8, "wp_viewporter", 1);
  registry.Global(9, "zxdg_decoration_manager_v1", 2);
  registry.Global(10, "wp_cursor_shape_manager_v1", 1);
  registry.Global(11, "zwp_linux_dmabuf_v1", 4);
}

void Registry::OnBind(U32, StrView id_interface, U32 id_version, U32 id) {
  if (id_interface == "wl_compositor") {
    Compositor::ColonyMake(id, client);
  } else if (id_interface == "wl_subcompositor") {
    Subcompositor::ColonyMake(id, client);
  } else if (id_interface == "wl_shm") {
    Shm& shm = Shm::ColonyMake(id, client);
    shm.Format(Shm::FormatArgb8888);
    shm.Format(Shm::FormatXrgb8888);
  } else if (id_interface == "wl_seat") {
    Seat& seat = Seat::ColonyMake(id, client);
    seat.version = id_version;
    seat.Capabilities(
        static_cast<Seat::Capability>(Seat::CapabilityPointer | Seat::CapabilityKeyboard));
    if (id_version >= 2) seat.Name("seat0");
  } else if (id_interface == "wl_output") {
    Output& output = Output::ColonyMake(id, client);
    output.Geometry(0, 0, 300, 190, static_cast<Output::Subpixel>(0), "Automat", "Dummy",
                    static_cast<Output::Transform>(0));
    output.Mode(static_cast<enum Output::Mode>(Output::ModeCurrent | Output::ModePreferred), 1920,
                1080, 60000);
    if (id_version >= 4) output.Name("AUTOMAT-1");
    if (id_version >= 2) {
      output.Scale(1);
      output.Done();
    }
  } else if (id_interface == "wl_data_device_manager") {
    DataDeviceManager::ColonyMake(id, client);
  } else if (id_interface == "xdg_wm_base") {
    XdgWmBase::ColonyMake(id, client).version = id_version;
  } else if (id_interface == "wp_viewporter") {
    Viewporter::ColonyMake(id, client);
  } else if (id_interface == "zxdg_decoration_manager_v1") {
    ZxdgDecorationManagerV1::ColonyMake(id, client);
  } else if (id_interface == "wp_cursor_shape_manager_v1") {
    CursorShapeManagerV1::ColonyMake(id, client);
  } else if (id_interface == "zwp_linux_dmabuf_v1") {
    AdvertiseDmabufOnBind(LinuxDmabufV1::ColonyMake(id, client), id_version);
  }
}

void Client::ProtocolError(U32 object_id, U32 code, StrView message) {
  if (errored) return;
  errored = true;
  static_cast<Display*>(GetId(1))->Error(object_id, code, message);
}

void Client::Disconnect() {
  Status status;
  server.epoll.Del(this, status);
  server.epoll.Post([this] { Client::colony.erase(Client::colony.get_iterator(this)); });
}

void Client::NotifyRead(Status&) {
  for (;;) {
    char buf[4096];
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 16)];
    iovec iov{buf, sizeof(buf)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    ssize_t n = recvmsg(fd.fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n > 0) {
      in.append(buf, n);
      for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
          int count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
          int fds[16];
          std::memcpy(fds, CMSG_DATA(cm), sizeof(int) * count);
          for (int k = 0; k < count; ++k) recv_fds.push_back(fds[k]);
        }
      continue;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      Disconnect();
      return;
    }
    break;
  }
  size_t offset = 0;
  while (in.size() - offset >= 8) {
    U32 id, word;
    std::memcpy(&id, in.data() + offset, 4);
    std::memcpy(&word, in.data() + offset + 4, 4);
    U32 size = word >> 16, opcode = word & 0xffff;
    if (size < 8 || in.size() - offset < size) break;
    if (Common* o = GetId(id))
      o->GenericDispatch(opcode, in.data() + offset + 8, in.data() + offset + size);
    offset += size;
    if (errored) break;
  }
  in.erase(0, offset);
  server.FlushAll();
  if (errored) Disconnect();
}
StrView Client::Name() const { return "WaylandClient"sv; }

void Server::NotifyRead(Status&) {
  for (;;) {
    int client_fd = accept4(fd.fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0) {
      if (errno == EINTR) continue;
      break;
    }
    Client& client = *Client::colony.emplace(*this);
    client.fd = FD(client_fd);
    Display::ColonyMake(1, client);
    Status status;
    epoll.Add(&client, status);
  }
}
StrView Server::Name() const { return "WaylandServer"sv; }

void Server::FlushAll() {
  for (Client& client : Client::colony) {
    if (client.out.empty()) {
      client.out_fds.clear();
      continue;
    }
    msghdr msg{};
    iovec iov{client.out.data(), client.out.size()};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 16)];
    if (!client.out_fds.empty()) {
      int count = std::min<int>(client.out_fds.size(), 16);
      msg.msg_control = control;
      msg.msg_controllen = CMSG_SPACE(sizeof(int) * count);
      cmsghdr* cm = CMSG_FIRSTHDR(&msg);
      cm->cmsg_level = SOL_SOCKET;
      cm->cmsg_type = SCM_RIGHTS;
      cm->cmsg_len = CMSG_LEN(sizeof(int) * count);
      int fds[16];
      for (int k = 0; k < count; ++k) fds[k] = client.out_fds[k].fd;
      std::memcpy(CMSG_DATA(cm), fds, sizeof(int) * count);
    }
    ssize_t n = sendmsg(client.fd.fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) client.out.erase(0, n);
    client.out_fds.clear();
  }
}

std::unique_ptr<Server> MakeServer(mux::Epoll& epoll, Status& status) {
  const char* runtime = getenv("XDG_RUNTIME_DIR");
  if (!runtime) {
    AppendErrorMessage(status) += "XDG_RUNTIME_DIR is not set";
    return nullptr;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    AppendErrorMessage(status) += f("socket(): {}", strerror(errno));
    return nullptr;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  Str name, sock_path;
  FD lock_fd;
  for (int n = 0; n <= 32 && name.empty(); ++n) {
    Str candidate = f("wayland-{}", n);
    Str path = f("{}/{}", runtime, candidate);
    if (path.size() + 1 > sizeof(addr.sun_path)) continue;
    int lf = open(f("{}.lock", path).c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0660);
    if (lf < 0) continue;
    if (flock(lf, LOCK_EX | LOCK_NB) != 0) {
      close(lf);
      continue;
    }
    unlink(path.c_str());
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      name = candidate;
      sock_path = path;
      lock_fd = FD(lf);
    } else {
      close(lf);
    }
  }
  if (name.empty()) {
    close(fd);
    AppendErrorMessage(status) += "no free wayland-N socket in XDG_RUNTIME_DIR";
    return nullptr;
  }
  if (listen(fd, 16) < 0) {
    close(fd);
    AppendErrorMessage(status) += f("listen(): {}", strerror(errno));
    return nullptr;
  }
  setenv("WAYLAND_DISPLAY", name.c_str(), 1);
  LOG << f("Wayland server listening on {} (WAYLAND_DISPLAY={})", addr.sun_path, name);

  auto srv = std::make_unique<Server>(epoll);
  srv->fd = FD(fd);
  srv->socket_path = sock_path;
  srv->lock_fd = std::move(lock_fd);
  epoll.Post([s = srv.get()] {
    Status status;
    s->epoll.Add(s, status);
    if (!OK(status)) ERROR << "wayland: failed to watch the listening socket: " << status.ToStr();
  });
  return srv;
}

std::unique_ptr<Server> server;

Server::~Server() {
  if (socket_path) {
    Status ignore;
    socket_path.Unlink(ignore, true);
    socket_path.WithExt("lock").Unlink(ignore, true);
  }
}

bool Server::Running() { return true; }

}  // namespace automat::wayland
