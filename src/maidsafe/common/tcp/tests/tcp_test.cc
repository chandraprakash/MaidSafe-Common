/*  Copyright 2014 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "asio/buffer.hpp"
#include "asio/error.hpp"
#include "asio/io_service.hpp"
#include "asio/ip/tcp.hpp"
#include "asio/write.hpp"

#include "maidsafe/common/asio_service.h"
#include "maidsafe/common/make_unique.h"
#include "maidsafe/common/on_scope_exit.h"
#include "maidsafe/common/test.h"
#include "maidsafe/common/utils.h"
#include "maidsafe/common/config.h"
#include "maidsafe/common/tcp/connection.h"
#include "maidsafe/common/tcp/listener.h"


namespace maidsafe {

namespace tcp {

namespace test {

class Messages {
 public:
  enum class Status { kSuccess, kMismatch, kTimedOut };
  explicit Messages(std::vector<Message> expected_messages)
      : kExpectedMessages_([&]() -> std::vector<Message> {
          std::sort(std::begin(expected_messages), std::end(expected_messages));
          return expected_messages;
        }()),
        received_messages_(),
        mutex_() {}

  Status MessagesMatch() {
    if (!WaitForEnoughMessages()) {
      LOG(kError) << "Timed out waiting for messages.";
      return Status::kTimedOut;
    }
    std::lock_guard<std::mutex> lock{mutex_};
    std::sort(std::begin(received_messages_), std::end(received_messages_));
    return received_messages_ == kExpectedMessages_ ? Status::kSuccess : Status::kMismatch;
  }

  void AddMessage(Message message) {
    std::lock_guard<std::mutex> lock{mutex_};
    received_messages_.emplace_back(std::move(message));
  }

 private:
  size_t ReceivedCount() const {
    std::lock_guard<std::mutex> lock{mutex_};
    return received_messages_.size();
  }

  bool WaitForEnoughMessages() const {
    using std::chrono::steady_clock;
    size_t total_messages_size{0};
    for (auto itr(std::begin(kExpectedMessages_)); itr != std::end(kExpectedMessages_); ++itr)
      total_messages_size += itr->size();

    steady_clock::time_point timeout{steady_clock::now() +
                                     std::chrono::microseconds{total_messages_size + 1000000U}};
    while (steady_clock::now() < timeout && ReceivedCount() < kExpectedMessages_.size())
      Sleep(std::chrono::milliseconds{1});
    // Allow a little extra time to ensure we also check for receiving extra messages.
    Sleep(std::chrono::milliseconds{5});
    return ReceivedCount() == kExpectedMessages_.size();
  }

  const std::vector<Message> kExpectedMessages_;
  std::vector<Message> received_messages_;
  mutable std::mutex mutex_;
};

class TcpTest : public testing::Test {
 protected:
  TcpTest()
      : to_client_messages_(),
        to_server_messages_(),
        messages_received_by_client_(),
        messages_received_by_server_(),
        asio_service_(10),
        client_strand_(asio_service_.service()),
        server_strand_(asio_service_.service()) {}
  ~TcpTest() { asio_service_.Stop(); }

  using ConnectionAndCloser = std::pair<ConnectionPtr, std::unique_ptr<on_scope_exit>>;
  using ListenerAndCloser = std::pair<ListenerPtr, std::unique_ptr<on_scope_exit>>;

  void InitialiseMessagesToClient() {
    messages_received_by_client_ = maidsafe::make_unique<Messages>(to_client_messages_);
  }
  void InitialiseMessagesToServer() {
    messages_received_by_server_ = maidsafe::make_unique<Messages>(to_server_messages_);
  }

  ConnectionAndCloser GenerateClientConnection(Port port,
                                               MessageReceivedFunctor on_message_received,
                                               ConnectionClosedFunctor on_connection_closed) {
    ConnectionPtr connection{Connection::MakeShared(client_strand_, port)};
    connection->Start(on_message_received, on_connection_closed);
    return std::make_pair(
        connection, maidsafe::make_unique<on_scope_exit>([connection] { connection->Close(); }));
  }

  ListenerAndCloser GenerateListener(asio::io_service::strand& strand,
                                     NewConnectionFunctor on_new_connection, Port port) {
    ListenerPtr listener{Listener::MakeShared(strand, on_new_connection, port)};
    return std::make_pair(
        listener, maidsafe::make_unique<on_scope_exit>([listener] { listener->StopListening(); }));
  }

  void AddRandomMessage(std::vector<Message>& messages, std::size_t size) {
    auto data(RandomString(size));
    messages.emplace_back(data.begin(), data.end());
  }

  std::vector<Message> to_client_messages_, to_server_messages_;
  std::unique_ptr<Messages> messages_received_by_client_, messages_received_by_server_;
  AsioService asio_service_;
  asio::io_service::strand client_strand_, server_strand_;
};

TEST_F(TcpTest, BEH_Basic) {
  const size_t kMessageCount(10);
  AddRandomMessage(to_client_messages_, 1);
  AddRandomMessage(to_server_messages_, 1);
  for (size_t i(2); i < kMessageCount; ++i) {
    AddRandomMessage(to_client_messages_, i * 100000);
    AddRandomMessage(to_server_messages_, i * 100000);
  }
  AddRandomMessage(to_client_messages_, Connection::MaxMessageSize());
  AddRandomMessage(to_server_messages_, Connection::MaxMessageSize());
  InitialiseMessagesToClient();
  InitialiseMessagesToServer();

  std::promise<ConnectionPtr> server_promise;
  ListenerAndCloser listener_and_closer{GenerateListener(
      server_strand_,
      [&](ConnectionPtr connection) { server_promise.set_value(std::move(connection)); },
      Port{7777})};
  ConnectionAndCloser client_connection_and_closer{GenerateClientConnection(
      listener_and_closer.first->ListeningPort(),
      [&](Message message) { messages_received_by_client_->AddMessage(std::move(message)); },
      [&] { LOG(kVerbose) << "Client connection closed."; })};

  ConnectionPtr server_connection{server_promise.get_future().get()};
  server_connection->Start(
      [&](Message message) { messages_received_by_server_->AddMessage(std::move(message)); },
      [&] { LOG(kVerbose) << "Server connection closed."; });

  std::random_shuffle(std::begin(to_client_messages_), std::end(to_client_messages_));
  std::random_shuffle(std::begin(to_server_messages_), std::end(to_server_messages_));
  for (size_t i(0); i < kMessageCount; ++i) {
    server_connection->Send(to_client_messages_[i]);
    client_connection_and_closer.first->Send(to_server_messages_[i]);
  }
  EXPECT_EQ(messages_received_by_client_->MessagesMatch(), Messages::Status::kSuccess);
  EXPECT_EQ(messages_received_by_server_->MessagesMatch(), Messages::Status::kSuccess);
}

TEST_F(TcpTest, BEH_UnavailablePort) {
  AddRandomMessage(to_client_messages_, 1000);
  AddRandomMessage(to_server_messages_, 1000);
  InitialiseMessagesToClient();
  InitialiseMessagesToServer();

  asio::io_service::strand strand(asio_service_.service());
  {
    std::promise<ConnectionPtr> server_promise;
    ListenerAndCloser listener_and_closer0{
        GenerateListener(strand, [](ConnectionPtr /*connection*/) {}, Port{7777})};
    ListenerAndCloser listener_and_closer1{GenerateListener(
        server_strand_,
        [&](ConnectionPtr connection) { server_promise.set_value(std::move(connection)); },
        listener_and_closer0.first->ListeningPort())};
    ConnectionAndCloser client_connection_and_closer{GenerateClientConnection(
        listener_and_closer1.first->ListeningPort(),
        [&](Message message) { messages_received_by_client_->AddMessage(std::move(message)); },
        [&] { LOG(kVerbose) << "Client connection closed."; })};

    ConnectionPtr server_connection{server_promise.get_future().get()};
    server_connection->Start(
        [&](Message message) { messages_received_by_server_->AddMessage(std::move(message)); },
        [&] { LOG(kVerbose) << "Server connection closed."; });

    server_connection->Send(to_client_messages_.front());
    client_connection_and_closer.first->Send(to_server_messages_.front());
    EXPECT_EQ(messages_received_by_client_->MessagesMatch(), Messages::Status::kSuccess);
    EXPECT_EQ(messages_received_by_server_->MessagesMatch(), Messages::Status::kSuccess);
  }
  asio_service_.Stop();
}

TEST_F(TcpTest, BEH_InvalidMessageSizes) {
  to_client_messages_.emplace_back();
  to_server_messages_.emplace_back();
  AddRandomMessage(to_client_messages_, Connection::MaxMessageSize() + 1);
  AddRandomMessage(to_server_messages_, Connection::MaxMessageSize() + 1);
  InitialiseMessagesToClient();
  InitialiseMessagesToServer();

  ListenerAndCloser listener_and_closer{GenerateListener(
      server_strand_,
      [&](ConnectionPtr connection) {
        LOG(kVerbose) << "Server connection opened.";
        connection->Start(
            [&](Message msg) { messages_received_by_server_->AddMessage(std::move(msg)); },
            [&] { LOG(kVerbose) << "Server connection closed."; });
        for (const auto& message : to_client_messages_)
          EXPECT_THROW(connection->Send(message), maidsafe_error);
      },
      Port{7777})};
  ConnectionAndCloser client_connection_and_closer{GenerateClientConnection(
      listener_and_closer.first->ListeningPort(),
      [&](Message message) { messages_received_by_client_->AddMessage(std::move(message)); },
      [&] { LOG(kVerbose) << "Client connection closed."; })};

  EXPECT_THROW(client_connection_and_closer.first->Send(to_server_messages_[0]), maidsafe_error);
  EXPECT_THROW(client_connection_and_closer.first->Send(to_server_messages_[1]), maidsafe_error);
  EXPECT_EQ(messages_received_by_client_->MessagesMatch(), Messages::Status::kTimedOut);
  EXPECT_EQ(messages_received_by_server_->MessagesMatch(), Messages::Status::kTimedOut);

  // Try to make server receive a message which shows its size as too large
  AsioService bad_asio_service{1};
  asio::ip::tcp::socket bad_socket(bad_asio_service.service());
  bool is_v6{client_connection_and_closer.first->Socket().local_endpoint().address().is_v6()};
  if (is_v6) {
    bad_socket.connect(asio::ip::tcp::endpoint{asio::ip::address_v6::loopback(),
                                               listener_and_closer.first->ListeningPort()});
  } else {
    bad_socket.connect(asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(),
                                               listener_and_closer.first->ListeningPort()});
  }
  ASSERT_TRUE(bad_socket.is_open());

  to_server_messages_.erase(std::begin(to_server_messages_));
  ASSERT_EQ(to_server_messages_.size(), 1U);
  ASSERT_GT(to_server_messages_.front().size(), Connection::MaxMessageSize());
  InitialiseMessagesToServer();
  const Message& large_data{to_server_messages_.front()};

  std::array<unsigned char, 4> size_buffer;
  for (int i = 0; i != 4; ++i)
    size_buffer[i] = static_cast<char>(large_data.size() >> (8 * (3 - i)));

  std::array<asio::const_buffer, 2> buffers;
  buffers[0] = asio::buffer(size_buffer);
  buffers[1] = asio::buffer(large_data.data(), large_data.size());
  try {
    // N.B. This may or may not throw depending on how quickly the server closes the connection at
    // its end.  However, we're only interested in the server dropping the message.
    asio::write(bad_socket, buffers);
  } catch (const std::system_error&) {
  }
  EXPECT_EQ(messages_received_by_server_->MessagesMatch(), Messages::Status::kTimedOut);

  // Try to make server receive a message which is too large by lying about its size
  InitialiseMessagesToServer();
  bad_socket = asio::ip::tcp::socket(bad_asio_service.service());
  if (is_v6) {
    bad_socket.connect(asio::ip::tcp::endpoint{asio::ip::address_v6::loopback(),
                                               listener_and_closer.first->ListeningPort()});
  } else {
    bad_socket.connect(asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(),
                                               listener_and_closer.first->ListeningPort()});
  }
  ASSERT_TRUE(bad_socket.is_open());
  --size_buffer[3];
  buffers[0] = asio::buffer(size_buffer);
  EXPECT_EQ(asio::write(bad_socket, buffers), large_data.size() + 4U);
  EXPECT_EQ(messages_received_by_server_->MessagesMatch(), Messages::Status::kMismatch);
}

TEST_F(TcpTest, BEH_ServerConnectionAborts) {
  AddRandomMessage(to_client_messages_, 1000);
  AddRandomMessage(to_server_messages_, 1000);
  InitialiseMessagesToClient();
  InitialiseMessagesToServer();

  std::promise<ConnectionPtr> server_promise;
  ListenerAndCloser listener_and_closer{GenerateListener(
      server_strand_,
      [&](ConnectionPtr connection) { server_promise.set_value(std::move(connection)); },
      Port{8888})};
  ConnectionAndCloser client_connection_and_closer{GenerateClientConnection(
      listener_and_closer.first->ListeningPort(),
      [&](Message message) { messages_received_by_client_->AddMessage(std::move(message)); },
      [&] { LOG(kVerbose) << "Client connection closed."; })};

  ConnectionPtr server_connection{server_promise.get_future().get()};
  server_connection->Start(
      [&](Message message) { messages_received_by_server_->AddMessage(std::move(message)); },
      [&] { LOG(kVerbose) << "Server connection closed."; });

  server_connection->Send(to_client_messages_.front());
  client_connection_and_closer.first->Send(to_server_messages_.front());
  server_connection.reset();
}

TEST_F(TcpTest, BEH_ClientConnectionAborts) {
  AddRandomMessage(to_client_messages_, 1000);
  AddRandomMessage(to_server_messages_, 1000);
  InitialiseMessagesToClient();
  InitialiseMessagesToServer();

  std::promise<ConnectionPtr> server_promise;
  ListenerAndCloser listener_and_closer{GenerateListener(
      server_strand_,
      [&](ConnectionPtr connection) { server_promise.set_value(std::move(connection)); },
      Port{9999})};
  ConnectionAndCloser client_connection_and_closer{GenerateClientConnection(
      listener_and_closer.first->ListeningPort(),
      [&](Message message) { messages_received_by_client_->AddMessage(std::move(message)); },
      [&] { LOG(kVerbose) << "Client connection closed."; })};

  ConnectionPtr server_connection{server_promise.get_future().get()};
  server_connection->Start(
      [&](Message message) { messages_received_by_server_->AddMessage(std::move(message)); },
      [&] { LOG(kVerbose) << "Server connection closed."; });

  server_connection->Send(to_client_messages_.front());
  client_connection_and_closer.first->Send(to_server_messages_.front());
  client_connection_and_closer.first.reset();
}

TEST_F(TcpTest, BEH_MultipleConnectionsToServer) {
  const size_t kMessageCount(10), kClientCount(10);
  std::vector<Message> to_server_messages_from_single_client;
  for (size_t i(0); i < kMessageCount; ++i) {
    AddRandomMessage(to_client_messages_, 10000);
    AddRandomMessage(to_server_messages_from_single_client, 10000);
    for (size_t j(0); j < kClientCount; ++j)
      to_server_messages_.push_back(to_server_messages_from_single_client.back());
  }
  std::vector<std::unique_ptr<Messages>> messages_received_by_client;
  for (size_t i(0); i < kClientCount; ++i)
    messages_received_by_client.emplace_back(maidsafe::make_unique<Messages>(to_client_messages_));
  InitialiseMessagesToServer();

  std::mutex mutex;
  std::condition_variable cond_var;
  std::vector<ConnectionPtr> server_connections;
  ListenerAndCloser listener_and_closer{GenerateListener(
      server_strand_,
      [&](ConnectionPtr connection) {
        connection->Start([&](Message msg) {
                            LOG(kVerbose) << "Server received msg";
                            messages_received_by_server_->AddMessage(std::move(msg));
                          },
                          [&] { LOG(kVerbose) << "Server connection closed."; });
        {
          std::lock_guard<std::mutex> lock{mutex};
          server_connections.push_back(connection);
        }
        cond_var.notify_one();
      },
      Port{9876})};

  std::vector<ConnectionAndCloser> client_connections_and_closers;
  for (size_t i(0); i < kClientCount; ++i) {
    client_connections_and_closers.emplace_back(
        GenerateClientConnection(listener_and_closer.first->ListeningPort(),
                                 [&, i](Message msg) {
                                   LOG(kVerbose) << "Client " << i << " received msg";
                                   messages_received_by_client[i]->AddMessage(std::move(msg));
                                 },
                                 [&] { LOG(kVerbose) << "Client connection closed."; }));
  }

  std::unique_lock<std::mutex> lock{mutex};
  ASSERT_TRUE(cond_var.wait_for(lock, std::chrono::seconds(10),
                                [&] { return server_connections.size() == kClientCount; }));

  std::random_shuffle(std::begin(to_client_messages_), std::end(to_client_messages_));
  std::random_shuffle(std::begin(to_server_messages_from_single_client),
                      std::end(to_server_messages_from_single_client));
  for (size_t i(0); i < kMessageCount; ++i) {
    for (size_t j(0); j < kClientCount; ++j) {
      server_connections[j]->Send(to_client_messages_[i]);
      client_connections_and_closers[j].first->Send(to_server_messages_from_single_client[i]);
    }
  }
  for (size_t i(0); i < kClientCount; ++i)
    EXPECT_EQ(messages_received_by_client[i]->MessagesMatch(), Messages::Status::kSuccess);
  EXPECT_EQ(messages_received_by_server_->MessagesMatch(), Messages::Status::kSuccess);

  for (auto& server_connection : server_connections)
    server_connection->Close();
  server_connections.clear();
}

}  // namespace test

}  // namespace tcp

}  // namespace maidsafe
