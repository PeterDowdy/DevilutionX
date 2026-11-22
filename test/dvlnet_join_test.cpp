/**
 * A joiner asks for the level data with its first turn (the upper bit), and the player that sends it drops turns numbered before its own
 * current turn. The joiner must number its turns from that player's turns, not from those of another player a turn behind.
 */
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "dvlnet/base.h"
#include "multi.h"
#include "player.h"

using namespace devilution;

namespace {

/** A client joining a game in memory: what it sends is recorded, the turns it hears are fed to it. */
class Joiner final : public net::base {
public:
	std::vector<net::turn_t> sentTurns;

	Joiner()
	{
		clear_password(); // (makes the packet factory)
	}
	int create(std::string_view /*addrstr*/) override
	{
		return -1;
	}
	int join(std::string_view /*addrstr*/) override
	{
		return -1;
	}
	std::string make_default_gamename() override
	{
		return "joiner";
	}
	std::expected<void, net::PacketError> poll() override
	{
		return {};
	}
	std::expected<void, net::PacketError> send(net::packet &pkt) override
	{
		if (pkt.Type() == net::PT_TURN) {
			std::expected<net::turn_t, net::PacketError> turn = pkt.Turn();
			if (turn.has_value())
				sentTurns.push_back(*turn);
		}
		return {};
	}
	/** Accepted as player `self` into a game of `players` players (connected as JOIN_ACCEPT and the server's PT_CONNECTs do). */
	void Joined(net::plr_t self, net::plr_t players)
	{
		plr_self = self;
		for (net::plr_t i = 0; i < players; ++i)
			Connect(i);
	}
	void Hear(net::plr_t from, uint8_t sequenceNumber)
	{
		net::turn_t turn;
		turn.SequenceNumber = sequenceNumber;
		turn.Value = 0;
		std::expected<std::unique_ptr<net::packet>, net::PacketError> pkt = pktfty->make_packet<net::PT_TURN>(from, net::PLR_BROADCAST, turn);
		ASSERT_TRUE(pkt.has_value());
		ASSERT_TRUE(RecvLocal(**pkt).has_value());
	}

protected:
	bool IsGameHost() override
	{
		return false;
	}
};

TEST(DvlnetJoin, TurnsAreNumberedFromTheLevelDataSender)
{
	Players.resize(MAX_PLRS);
	Joiner joiner;
	joiner.Joined(5, 6);
	int32_t request = static_cast<int32_t>(0x80000000U); // the first turn, asking for the level data
	ASSERT_TRUE(joiner.SNetSendTurn(reinterpret_cast<char *>(&request), sizeof(request)));
	EXPECT_TRUE(joiner.sentTurns.empty()); // (no numbering yet)

	// Player 3 is a turn behind player 0, which sends the level data (the first connected player but the joiner) and is at turn 21: a
	// request numbered 20 would be dropped there as stale.
	joiner.Hear(3, 20);
	EXPECT_TRUE(joiner.sentTurns.empty());
	joiner.Hear(0, 21);
	ASSERT_EQ(joiner.sentTurns.size(), 1U);
	EXPECT_EQ(joiner.sentTurns[0].SequenceNumber, 21);
	EXPECT_EQ(static_cast<uint32_t>(joiner.sentTurns[0].Value), 0x80000000U);
	Players.clear();
}

TEST(DvlnetJoin, TheLevelDataSenderIsTheFirstPlayerButTheJoiner)
{
	Players.resize(MAX_PLRS);
	Joiner joiner;
	joiner.Joined(0, 3); // (player 0's slot was free again: player 1 sends the level data)
	int32_t request = static_cast<int32_t>(0x80000000U);
	ASSERT_TRUE(joiner.SNetSendTurn(reinterpret_cast<char *>(&request), sizeof(request)));
	joiner.Hear(2, 40);
	EXPECT_TRUE(joiner.sentTurns.empty());
	joiner.Hear(1, 42);
	ASSERT_EQ(joiner.sentTurns.size(), 1U);
	EXPECT_EQ(joiner.sentTurns[0].SequenceNumber, 42);
	Players.clear();
}

} // namespace
