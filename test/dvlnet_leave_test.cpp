/**
 * The network thread (nthread) goes on sending and receiving turns after the game has been left (SNetLeaveGame on a fatal error or at the
 * end of the game, on another thread) and after the network is destroyed (SNetDestroy): neither may touch a player's turn queue.
 */
#include <cstdint>
#include <memory>
#include <string_view>

#include <gtest/gtest.h>

#include "dvlnet/base.h"
#include "multi.h"
#include "player.h"
#include "storm/storm_net.hpp"

using namespace devilution;

namespace {

/** A dvlnet game in memory: nothing goes out, nothing comes in. */
class QuietNet final : public net::base {
public:
	QuietNet()
	{
		clear_password(); // (makes the packet factory)
	}
	int create(std::string_view /*addrstr*/) override
	{
		plr_self = 0;
		return plr_self;
	}
	int join(std::string_view /*addrstr*/) override
	{
		return -1;
	}
	std::string make_default_gamename() override
	{
		return "quiet";
	}
	std::expected<void, net::PacketError> poll() override
	{
		return {};
	}
	std::expected<void, net::PacketError> send(net::packet & /*pkt*/) override
	{
		return {};
	}
	net::plr_t Self() const
	{
		return plr_self;
	}

protected:
	bool IsGameHost() override
	{
		return true;
	}
};

int32_t Turn(uint32_t value)
{
	return static_cast<int32_t>(value);
}

TEST(DvlnetLeave, NoOwnTurnsAfterLeavingTheGame)
{
	Players.resize(MAX_PLRS);
	auto net = std::make_unique<QuietNet>();
	ASSERT_EQ(net->create("quiet"), 0);
	int32_t turn = Turn(1);
	ASSERT_TRUE(net->SNetSendTurn(reinterpret_cast<char *>(&turn), sizeof(turn)));
	uint32_t inTransit = 0;
	ASSERT_TRUE(net->SNetGetTurnsInTransit(&inTransit));
	EXPECT_EQ(inTransit, 1U);

	ASSERT_TRUE(net->SNetLeaveGame(net::leaveinfo_t::LEAVE_EXIT));
	ASSERT_EQ(net->Self(), net::PLR_BROADCAST);
	// What the network thread does next (nthread_send_and_recv_turn): it used to index playerStateTable_[PLR_BROADCAST], past the table.
	ASSERT_TRUE(net->SNetGetTurnsInTransit(&inTransit));
	EXPECT_EQ(inTransit, 0U);
	for (int i = 0; i < 4; ++i)
		EXPECT_TRUE(net->SNetSendTurn(reinterpret_cast<char *>(&turn), sizeof(turn)));
	ASSERT_TRUE(net->SNetGetTurnsInTransit(&inTransit));
	EXPECT_EQ(inTransit, 0U);
	Players.clear();
}

TEST(DvlnetLeave, NoTurnsAfterTheNetworkIsDestroyed)
{
	// app_fatal destroys the network (SNetDestroy) while the network thread may still run: its calls find nothing, rather than a null net.
	ASSERT_TRUE(SNetDestroy());
	uint32_t inTransit = 1;
	EXPECT_TRUE(SNetGetTurnsInTransit(&inTransit));
	EXPECT_EQ(inTransit, 0U);
	int32_t turn = Turn(1);
	EXPECT_TRUE(SNetSendTurn(reinterpret_cast<char *>(&turn), sizeof(turn)));
	char *data[MAX_PLRS] = {};
	size_t sizes[MAX_PLRS] = {};
	uint32_t status[MAX_PLRS] = {};
	EXPECT_FALSE(SNetReceiveTurns(MAX_PLRS, data, sizes, status));
	uint8_t sender = 0;
	void *message = nullptr;
	size_t messageSize = 0;
	EXPECT_FALSE(SNetReceiveMessage(&sender, &message, &messageSize));
}

} // namespace
