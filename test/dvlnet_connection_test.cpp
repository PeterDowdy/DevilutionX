/**
 * A TCP client whose connection to the game breaks (the host closed it, quit or crashed, or it sent what can't be read) says so
 * (connection_lost), so that a joiner and a player in a game can leave instead of waiting for packets that will never come. Leaving on
 * purpose is not a lost connection. The server here is a stand-in that accepts one join.
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>

#include "dvlnet/base.h"
#include "dvlnet/frame_queue.h"
#include "dvlnet/packet.h"
#include "dvlnet/tcp_client.h"
#include "multi.h"
#include "player.h"

using namespace devilution;
using asio::ip::tcp;

namespace {

/** Accepts one connection, answers its join request as player 1, then does what the test asks. */
class StandInServer {
public:
	StandInServer()
	    : acceptor_(ioc_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0))
	{
		thread_ = std::thread([this]() { Serve(); });
	}
	~StandInServer()
	{
		if (thread_.joinable())
			thread_.join();
	}
	[[nodiscard]] std::string Address() const
	{
		return "127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port());
	}
	/** Waits until the join was answered. */
	bool Accepted()
	{
		for (int i = 0; i < 500 && !accepted_; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return accepted_;
	}
	/** Tells the server thread what to do next: close the connection, or send a frame nobody can read. */
	void Then(int action)
	{
		action_ = action;
	}
	static constexpr int Close = 1;
	static constexpr int SendGarbage = 2;
	static constexpr int Wait = 3; // until the client closes (it leaves)

private:
	void Serve()
	{
		tcp::socket socket(ioc_);
		acceptor_.accept(socket);
		net::frame_queue queue;
		net::packet_factory factory;
		std::unique_ptr<net::packet> request;
		while (request == nullptr) {
			net::buffer_t chunk(4096);
			asio::error_code ec;
			const size_t n = socket.read_some(asio::buffer(chunk), ec);
			if (ec) return;
			chunk.resize(n);
			queue.Write(std::move(chunk));
			std::expected<bool, net::PacketError> ready = queue.PacketReady();
			if (!ready.has_value() || !*ready) continue;
			std::expected<net::buffer_t, net::PacketError> data = queue.ReadPacket();
			if (!data.has_value()) return;
			std::expected<std::unique_ptr<net::packet>, net::PacketError> pkt = factory.make_packet(*data);
			if (!pkt.has_value()) return;
			request = std::move(*pkt);
		}
		net::buffer_t info(sizeof(GameData), 0);
		const net::buffer_t tag = net::ProtocolJoinTag();
		info.insert(info.end(), tag.begin(), tag.end());
		std::expected<std::unique_ptr<net::packet>, net::PacketError> accept
		    = factory.make_packet<net::PT_JOIN_ACCEPT>(net::PLR_MASTER, net::PLR_BROADCAST, *request->Cookie(), static_cast<net::plr_t>(1), info);
		if (!accept.has_value()) return;
		asio::write(socket, asio::buffer(*net::frame_queue::MakeFrame((*accept)->Data())));
		accepted_ = true;
		while (action_ == 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		if (action_ == SendGarbage) {
			net::buffer_t garbage(64, 0xA5);
			asio::write(socket, asio::buffer(*net::frame_queue::MakeFrame(garbage)));
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		} else if (action_ == Wait) {
			net::buffer_t rest(4096);
			asio::error_code ec;
			while (!ec)
				socket.read_some(asio::buffer(rest), ec);
		}
		socket.close();
	}

	asio::io_context ioc_;
	tcp::acceptor acceptor_;
	std::thread thread_;
	std::atomic<bool> accepted_ { false };
	std::atomic<int> action_ { 0 };
};

/** Polls the client (as the game does every tick) until its connection is lost, for at most two seconds. */
bool LosesConnection(net::tcp_client &client)
{
	for (int i = 0; i < 200; ++i) {
		client.process_network_packets();
		if (client.connection_lost()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

class DvlnetConnection : public ::testing::Test {
protected:
	void SetUp() override
	{
		Players.resize(MAX_PLRS);
		client_ = std::make_unique<net::tcp_client>();
		client_->clear_password();
	}
	void TearDown() override
	{
		client_ = nullptr;
		Players.clear();
	}
	std::unique_ptr<net::tcp_client> client_;
};

TEST_F(DvlnetConnection, TheServerClosingTheConnectionIsALostConnection)
{
	StandInServer server;
	ASSERT_EQ(client_->join(server.Address()), 1);
	ASSERT_TRUE(server.Accepted());
	EXPECT_FALSE(client_->connection_lost());
	server.Then(StandInServer::Close); // (a joiner waiting for the level data, or a player in the game: the host quit or crashed)
	EXPECT_TRUE(LosesConnection(*client_));
}

TEST_F(DvlnetConnection, APacketThatCantBeReadIsALostConnection)
{
	StandInServer server;
	ASSERT_EQ(client_->join(server.Address()), 1);
	ASSERT_TRUE(server.Accepted());
	server.Then(StandInServer::SendGarbage); // (the receive loop stops at it: nothing after it would arrive)
	EXPECT_TRUE(LosesConnection(*client_));
}

TEST_F(DvlnetConnection, LeavingTheGameIsNotALostConnection)
{
	StandInServer server;
	ASSERT_EQ(client_->join(server.Address()), 1);
	ASSERT_TRUE(server.Accepted());
	server.Then(StandInServer::Wait);
	EXPECT_TRUE(client_->SNetLeaveGame(net::leaveinfo_t::LEAVE_EXIT));
	EXPECT_FALSE(LosesConnection(*client_));
}

} // namespace
