#include <algorithm>
#include <thread>

#include <gtest/gtest.h>

#include "channel.h"

using namespace automat;

TEST(ChannelTest, SendForce) {
  channel c;
  c.send_force(std::make_unique<int>(1));
  EXPECT_EQ(*c.recv<int>(), 1);
}

TEST(ChannelTest, ManySenders) {
  channel c;
  for (int i = 0; i < 100; ++i) {
    std::thread([&c, i] { c.send(std::make_unique<int>(i)); }).detach();
  }
  std::vector<int> received;
  while (received.size() < 100) {
    received.push_back(*c.recv<int>());
  }
  std::sort(received.begin(), received.end());
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(received[i], i);
  }
}

TEST(ChannelTest, RecvBeforeSend) {
  channel c;
  std::thread([&c] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    c.send(std::make_unique<int>(1));
  }).detach();
  EXPECT_EQ(*c.recv<int>(), 1);
}
