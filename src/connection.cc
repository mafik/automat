#include "connection.hh"

#include "location.hh"

namespace automat {

Connection::~Connection() {
  // Maybe call some "OnConnectionDeleted" method on "from" and "to"?
  for (auto it = from.outgoing.begin(); it != from.outgoing.end(); ++it) {
    if (it->second == this) {
      from.outgoing.erase(it);
      break;
    }
  }
  for (auto it = to.incoming.begin(); it != to.incoming.end(); ++it) {
    if (it->second == this) {
      to.incoming.erase(it);
      break;
    }
  }
}

}  // namespace automat