#pragma once

#include <deque>
#include <memory>
#include <string>

#include <asio/ts/buffer.hpp>
#include <asio/ts/internet.hpp>
#include <asio/ts/io_context.hpp>
#include <asio/ts/net.hpp>
#include <asio_handle_exception.hpp>

#include "dvlnet/base.h"
#include "dvlnet/frame_queue.h"
#include "dvlnet/packet.h"
#include "dvlnet/tcp_server.h"

namespace devilution::net {

class tcp_client : public base {
public:
	int create(std::string_view addrstr) override;
	int join(std::string_view addrstr) override;

	std::expected<void, PacketError> poll() override;
	std::expected<void, PacketError> send(packet &pkt) override;
	void DisconnectNet(plr_t plr) override;

	bool SNetLeaveGame(net::leaveinfo_t type) override;
	bool connection_lost() override;

	~tcp_client() override;

	std::string make_default_gamename() override;

protected:
	bool IsGameHost() override;
	/** Client-server: only the host drops other players (its drop also closes their connection). */
	bool MayDropOthers() override;
	/** Obeys a drop from the server, from the host (player 0: the creator's own connection is the server's first) or
	 *  from the player itself (leaving), never another client's: one stalled client must not drop everybody. */
	bool AcceptsDisconnect(plr_t source, plr_t target) override;

private:
	frame_queue recv_queue;
	buffer_t recv_buffer = buffer_t(frame_queue::max_frame_size);

	asio::io_context ioc;
	asio::ip::tcp::resolver resolver = asio::ip::tcp::resolver(ioc);
	asio::ip::tcp::socket sock = asio::ip::tcp::socket(ioc);
	std::unique_ptr<tcp_server> local_server; // must be declared *after* ioc

	std::optional<PacketError> ioHandlerResult;
	/** The connection broke (LoseConnection): the receive loop has stopped, nothing more arrives. */
	bool connectionLost_ = false;
	/** SNetLeaveGame closed the socket on purpose: the aborted receive is not a lost connection. */
	bool leaving_ = false;

	/**
	 * Frames waiting to be written, the front one being written. asio allows one async_write at a time per socket:
	 * a second one started while the first is only partly written interleaves their bytes.
	 */
	std::deque<std::unique_ptr<buffer_t>> sendQueue;

	void HandleReceive(const asio::error_code &error, size_t bytesRead);
	void StartReceive();
	void StartWrite();
	void HandleSend(const asio::error_code &error, size_t bytesSent);
	void HandleTcpErrorCode();

	void RaiseIoHandlerError(const PacketError &error);
	/** A read or write failed, the server closed the connection or sent what can't be read: logged once, and connection_lost(). */
	void LoseConnection(const PacketError &error);
};

} // namespace devilution::net
