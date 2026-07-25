/**
 * @file pblink.h
 * @author Hüseyin Karakaya <karakayahuseyin023@gmail.com>
 * @brief Pblink module - Bidirectional serial/UDP communication
 */

#pragma once

#include <px4_platform_common/module.h>
#include <px4_platform_common/module_params.h>
#include <px4_platform_common/posix.h>
#include <uORB/Publication.hpp>
#include <uORB/uORB.h>

#include <termios.h>
#include <cstdint>

#include "pblink_topics.h"

#ifdef PBLINK_UDP_SUPPORTED
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
#endif

extern "C" __EXPORT int pblink_main(int argc, char *argv[]);

enum class TransportMode : uint8_t {
	UART,
	UDP
};

class PBLink : public ModuleBase, public ModuleParams
{
public:
	struct StartConfig {
		TransportMode transport{TransportMode::UDP};
		const char *uart_device{nullptr};
		int uart_baud{115200};
		int udp_port{14556};
		char udp_peer_ip[16]{"127.0.0.1"};
	};

	PBLink(const StartConfig &config);
	virtual ~PBLink();

	static Descriptor desc;

	static int run_trampoline(int argc, char *argv[]);
	static int task_spawn(int argc, char *argv[]);
	static PBLink *instantiate(int argc, char *argv[]);
	static int custom_command(int argc, char *argv[]);
	static int print_usage(const char *reason = nullptr);

	void run() override;
	int print_status() override;

private:
	static bool parse_start_config(int argc, char *argv[], StartConfig &config);
	void parameters_update(bool force = false);

	TransportMode _transport{TransportMode::UART};

	int _uart_init(const char *uart_name, int baud_rate);
	void _uart_deinit();
	int _uart_fd{-1};
	char _uart_device[32]{};
	int _uart_baud{115200};
	bool _uart_initialized{false};

#ifdef PBLINK_UDP_SUPPORTED
	int _udp_init(const char *ip, int port);
	void _udp_deinit();
	int _udp_fd{-1};
	int _udp_port{14556};
	char _udp_peer_ip[16]{"127.0.0.1"};
	struct sockaddr_in _companion_addr{};
	bool _udp_initialized{false};
#endif // PBLINK_UDP_SUPPORTED

	bool _send_proto_frame(ProtoMsgType msg_type, const uint8_t *payload, size_t len);
	void _process_incoming_data();
	void _handle_received_frame(uint8_t msg_type, const uint8_t *payload, size_t len);
	bool _transport_has_pending_data();
	bool _uplink_ready() const;
	void _set_uplink_interval(size_t idx, uint32_t interval_us);

	static constexpr size_t RX_BUFFER_SIZE = 2048;
	uint8_t _rx_buffer[RX_BUFFER_SIZE];
	size_t _rx_buffer_len{0};

	struct TransportStats {
		uint64_t tx_total_bytes{0};
		uint64_t rx_total_bytes{0};
		uint32_t tx_message_count{0};
		uint32_t rx_message_count{0};
		uint32_t tx_error_count{0};
		uint32_t rx_error_count{0};
		uint32_t crc_error_count{0};
		int last_tx_errno{0};
		float tx_rate_avg{0.f};
		float rx_rate_avg{0.f};
		hrt_abstime last_update{0};
		hrt_abstime start_time{0};
	};

	TransportStats _stats;
	void _update_stats();

	class RateLimiter {
	public:
		RateLimiter() = default;

		void configure(uint32_t interval_us) {
			_interval_us = interval_us;
		}

		bool should_send() {
			if (_interval_us == 0) {
				return false;
			}

			hrt_abstime now = hrt_absolute_time();
			if (now - _last_sent >= _interval_us) {
				_last_sent = now;
				return true;
			}
			return false;
		}

		uint32_t get_interval() const { return _interval_us; }

		void reset() {
			_last_sent = 0;
		}

	private:
		uint32_t _interval_us{0};
		hrt_abstime _last_sent{0};
	};

	RateLimiter _uplink_rate_limiters[UPLINK_TOPICS_COUNT];

	struct TopicStats {
		uint32_t send_count{0};
		hrt_abstime first_send{0};
		hrt_abstime last_send{0};
	};
	TopicStats _topic_stats[UPLINK_TOPICS_COUNT];

	void _print_topic_stats();
	void _send_topic_list();

	px4_pollfd_struct_t _uplink_fds[UPLINK_POLL_TOPICS_COUNT] {};
	PBLINK_DOWNLINK_PUBLICATIONS

	uint8_t _tx_seq{0};
	uint8_t _rx_seq{0};
	size_t _topic_info_next_index{0};
	hrt_abstime _last_topic_info_cycle{0};
	hrt_abstime _last_topic_info_send{0};
};
