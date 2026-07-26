#include "pblink.h"

#include <px4_platform_common/cli.h>
#include <px4_platform_common/getopt.h>
#include <px4_platform_common/log.h>
#include <px4_platform_common/posix.h>
#include <parameters/param.h>

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <errno.h>

extern "C" {
#include <lib/crc/crc.h>
}
#include "uorb_to_proto.h"
#include "pblink_downlink.h"
#include "pblink_uplink.h"

#include <pb_encode.h>
#include <pb_decode.h>

static constexpr uint8_t FRAME_MAGIC_BYTE = 0xAA;
static constexpr int32_t DEFAULT_UDP_PEER_IP_PARAM = 2130706433; // 127.0.0.1
static constexpr uint32_t PARAM_TYPE_FLOAT_PROTO = 0;
static constexpr uint32_t PARAM_TYPE_INT_PROTO = 1;

static void pblink_copy_string(char *dst, size_t dst_size, const char *src, size_t src_size)
{
	if (!dst || dst_size == 0) {
		return;
	}

	if (!src || src_size == 0) {
		dst[0] = '\0';
		return;
	}

	size_t src_len = 0;

	while (src_len < src_size && src[src_len] != '\0') {
		src_len++;
	}

	const size_t copy_len = (src_len < (dst_size - 1)) ? src_len : (dst_size - 1);
	memcpy(dst, src, copy_len);
	dst[copy_len] = '\0';
}

static void pblink_ip_from_param(int32_t ip_param, char *dst, size_t dst_size)
{
	const uint32_t ip = static_cast<uint32_t>(ip_param);
	snprintf(dst, dst_size, "%u.%u.%u.%u",
		 static_cast<unsigned>((ip >> 24) & 0xff),
		 static_cast<unsigned>((ip >> 16) & 0xff),
		 static_cast<unsigned>((ip >> 8) & 0xff),
		 static_cast<unsigned>(ip & 0xff));
}

static bool pblink_parse_ip_arg(const char *arg, char *dst, size_t dst_size)
{
	if (!arg || !dst || dst_size < 16) {
		return false;
	}

	if (strncmp(arg, "p:", 2) == 0) {
		int ip_param = DEFAULT_UDP_PEER_IP_PARAM;

		if (px4_get_parameter_value(arg, ip_param) != 0) {
			return false;
		}

		pblink_ip_from_param(ip_param, dst, dst_size);
		return true;
	}

#ifdef PBLINK_UDP_SUPPORTED
	struct in_addr addr {};
	if (inet_aton(arg, &addr) == 0) {
		return false;
	}
#endif
	snprintf(dst, dst_size, "%s", arg);
	return true;
}

int PBLink::print_status()
{
	uint32_t active_count = 0;
	for (size_t i = 0; i < UPLINK_TOPICS_COUNT; i++) {
		if (_uplink_rate_limiters[i].get_interval() > 0) {
			active_count++;
		}
	}

#ifdef PBLINK_UDP_SUPPORTED
	if (_transport == TransportMode::UDP) {
		PX4_INFO("Transport: UDP (%s:%u) | Status: %s",
			 inet_ntoa(_companion_addr.sin_addr),
			 ntohs(_companion_addr.sin_port),
			 _udp_initialized ? "OK" : "ERROR");
	} else
#endif
	{
		PX4_INFO("Transport: UART (%s @ %d baud) | Status: %s",
		         _uart_device, _uart_baud, _uart_initialized ? "OK" : "ERROR");
	}

	PX4_INFO("TX: %.1f kB/s (%" PRIu32 " msgs) | RX: %.1f kB/s (%" PRIu32 " msgs) | Errors: %" PRIu32,
	         (double)(_stats.tx_rate_avg / 1024.0f), _stats.tx_message_count,
	         (double)(_stats.rx_rate_avg / 1024.0f), _stats.rx_message_count,
	         _stats.tx_error_count + _stats.rx_error_count + _stats.crc_error_count);

	PX4_INFO("Active Topics: %" PRIu32 " / %zu", active_count, UPLINK_TOPICS_COUNT);

	return 0;
}

ModuleBase::Descriptor PBLink::desc{task_spawn, custom_command, print_usage};

int PBLink::custom_command(int argc, char *argv[])
{
	return print_usage("unknown command");
}

int PBLink::run_trampoline(int argc, char *argv[])
{
	return ModuleBase::run_trampoline_impl(desc, [](int ac, char *av[]) -> ModuleBase * {
		return PBLink::instantiate(ac, av);
	}, argc, argv);
}

int PBLink::task_spawn(int argc, char *argv[])
{
	desc.task_id = px4_task_spawn_cmd("pblink",
				      SCHED_DEFAULT,
				      SCHED_PRIORITY_DEFAULT,
				      8192,
				      (px4_main_t)&run_trampoline,
				      (char *const *)argv);

	if (desc.task_id < 0) {
		desc.task_id = -1;
		return -errno;
	}

	return 0;
}

PBLink *PBLink::instantiate(int argc, char *argv[])
{
	StartConfig config;

	if (!parse_start_config(argc, argv, config)) {
		return nullptr;
	}

	return new PBLink(config);
}

bool PBLink::parse_start_config(int argc, char *argv[], StartConfig &config)
{
	bool error_flag = false;
	int myoptind = 1;
	int ch;
	const char *myoptarg = nullptr;

	while ((ch = px4_getopt(argc, argv, "t:d:b:h:u:", &myoptind, &myoptarg)) != EOF) {
		switch (ch) {
		case 't':
			if (strcmp(myoptarg, "serial") == 0) {
				config.transport = TransportMode::UART;

			} else if (strcmp(myoptarg, "udp") == 0) {
				config.transport = TransportMode::UDP;

			} else {
				PX4_ERR("unknown transport: %s", myoptarg);
				error_flag = true;
			}

			break;

		case 'd':
			config.uart_device = myoptarg;
			config.transport = TransportMode::UART;
			break;

		case 'b':
			if (px4_get_parameter_value(myoptarg, config.uart_baud) != 0) {
				PX4_ERR("baudrate parsing failed");
				error_flag = true;
			}

			break;

		case 'h':
			if (!pblink_parse_ip_arg(myoptarg, config.udp_peer_ip, sizeof(config.udp_peer_ip))) {
				PX4_ERR("invalid UDP peer IP '%s'", myoptarg);
				error_flag = true;
			}

			break;

		case 'u':
			if (px4_get_parameter_value(myoptarg, config.udp_port) != 0) {
				PX4_ERR("UDP port parsing failed");
				error_flag = true;
			}

			break;

		case '?':
		default:
			error_flag = true;
			break;
		}
	}

	if (config.transport == TransportMode::UART && config.uart_device == nullptr) {
		PX4_ERR("missing serial device");
		error_flag = true;
	}

	if (config.uart_baud < 9600 || config.uart_baud > 3000000) {
		PX4_ERR("invalid baud rate %d", config.uart_baud);
		error_flag = true;
	}

	if (config.udp_port < 1024 || config.udp_port > 65535) {
		PX4_ERR("invalid UDP port %d", config.udp_port);
		error_flag = true;
	}

	return !error_flag;
}

PBLink::PBLink(const StartConfig &config) : ModuleParams(nullptr)
{
	_transport = config.transport;
	_uart_baud = config.uart_baud;
	_udp_port = config.udp_port;

	if (config.uart_device != nullptr) {
		snprintf(_uart_device, sizeof(_uart_device), "%s", config.uart_device);
	}

	snprintf(_udp_peer_ip, sizeof(_udp_peer_ip), "%s", config.udp_peer_ip);
	memset(_rx_buffer, 0, sizeof(_rx_buffer));

	for (size_t i = 0; i < UPLINK_POLL_TOPICS_COUNT; i++) {
		_uplink_fds[i].fd = -1;
	}
	_stats.start_time = hrt_absolute_time();
	_stats.last_update = _stats.start_time;
}

PBLink::~PBLink()
{
	PBLINK_CLOSE_UPLINK_SUBSCRIPTIONS(_uplink_fds);
	_uart_deinit();
#ifdef PBLINK_UDP_SUPPORTED
	_udp_deinit();
#endif
}

int PBLink::_uart_init(const char *uart_name, int baud_rate)
{
	_uart_fd = open(uart_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (_uart_fd < 0) {
		PX4_ERR("Failed to open UART %s", uart_name);
		return -1;
	}

	speed_t speed;
	switch (baud_rate) {
		case 9600:   speed = B9600;   break;
		case 19200:  speed = B19200;  break;
		case 38400:  speed = B38400;  break;
		case 57600:  speed = B57600;  break;
		case 115200: speed = B115200; break;
		case 230400: speed = B230400; break;
		case 460800: speed = B460800; break;
		case 921600: speed = B921600; break;
		default:     speed = B115200; break;
	}

	termios uart_config{};
	tcgetattr(_uart_fd, &uart_config);
	cfsetspeed(&uart_config, speed);
	uart_config.c_iflag &= ~(IGNBRK | BRKINT | ICRNL | INLCR | PARMRK | INPCK | ISTRIP | IXON);
	uart_config.c_oflag &= ~OPOST;
	uart_config.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
	uart_config.c_cflag &= ~(CSIZE | PARENB);
	uart_config.c_cflag |= CS8;
	tcsetattr(_uart_fd, TCSANOW, &uart_config);

	_uart_initialized = true;
	PX4_INFO("UART %s @ %d baud initialized", uart_name, baud_rate);
	return 0;
}

void PBLink::_uart_deinit()
{
	if (_uart_fd >= 0) {
		close(_uart_fd);
		_uart_fd = -1;
		_uart_initialized = false;
	}
}

#ifdef PBLINK_UDP_SUPPORTED
int PBLink::_udp_init(const char *ip, int port)
{
	_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (_udp_fd < 0) {
		PX4_ERR("Failed to create UDP socket");
		return -1;
	}

	int flags = fcntl(_udp_fd, F_GETFL, 0);
	fcntl(_udp_fd, F_SETFL, flags | O_NONBLOCK);

	struct sockaddr_in bind_addr{};
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = INADDR_ANY;
	bind_addr.sin_port = htons(port);

	if (bind(_udp_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		PX4_ERR("Failed to bind UDP port %d", port);
		close(_udp_fd);
		_udp_fd = -1;
		return -1;
	}

	_companion_addr.sin_family = AF_INET;
	if (inet_pton(AF_INET, ip, &_companion_addr.sin_addr) != 1) {
		PX4_ERR("Invalid UDP peer IP %s", ip);
		close(_udp_fd);
		_udp_fd = -1;
		return -1;
	}

	_companion_addr.sin_port = htons(port + 1);

	_udp_initialized = true;
	PX4_INFO("UDP initialized on port %d, sending to %s:%d", port, ip, port + 1);
	return 0;
}

void PBLink::_udp_deinit()
{
	if (_udp_fd >= 0) {
		close(_udp_fd);
		_udp_fd = -1;
		_udp_initialized = false;
	}
}
#endif

bool PBLink::_send_proto_frame(ProtoMsgType msg_type, const uint8_t *payload, size_t len)
{
	uint8_t frame[1024];
	frame[0] = FRAME_MAGIC_BYTE;
	frame[1] = static_cast<uint8_t>(msg_type);
	frame[2] = _tx_seq++;
	frame[3] = (uint8_t)(len & 0xFF);
	frame[4] = (uint8_t)((len >> 8) & 0xFF);

	uint8_t header_crc = 0;

	for (int i = 0; i < 5; i++) {
		header_crc ^= frame[i];
	}

	frame[5] = header_crc;
	memcpy(frame + 6, payload, len);

	// CRC covers msg_type, seq, len, header_crc, and payload.
	uint16_t frame_crc = crc16_signature(CRC16_INITIAL, 5 + len, frame + 1);
	frame[6 + len] = (uint8_t)(frame_crc & 0xFF);
	frame[7 + len] = (uint8_t)((frame_crc >> 8) & 0xFF);

	size_t frame_len = 8 + len;

	bool success = false;
	ssize_t bytes_sent = 0;

#ifdef PBLINK_UDP_SUPPORTED
	if (_transport == TransportMode::UDP && _udp_initialized) {
		bytes_sent = sendto(_udp_fd, frame, frame_len, 0,
		                    (struct sockaddr *)&_companion_addr, sizeof(_companion_addr));
		success = (bytes_sent == (ssize_t)frame_len);
	} else
#endif
	if (_transport == TransportMode::UART && _uart_initialized) {
		bytes_sent = write(_uart_fd, frame, frame_len);
		success = (bytes_sent == (ssize_t)frame_len);
	}

	if (success && bytes_sent > 0) {
		_stats.tx_total_bytes += bytes_sent;
		_stats.tx_message_count++;
		_stats.last_tx_errno = 0;
	} else {
		if (bytes_sent < 0) {
			_stats.last_tx_errno = errno;
		}
		_stats.tx_error_count++;
	}

	return success;
}

void PBLink::_process_incoming_data()
{
	uint8_t temp_buf[512];
	ssize_t nread = 0;

#ifdef PBLINK_UDP_SUPPORTED
	if (_transport == TransportMode::UDP && _udp_initialized) {
		struct sockaddr_in sender_addr {};
		socklen_t addr_len = sizeof(sender_addr);
		nread = recvfrom(_udp_fd, temp_buf, sizeof(temp_buf), 0,
				 (struct sockaddr *)&sender_addr, &addr_len);
	} else
#endif
	if (_transport == TransportMode::UART && _uart_initialized) {
		nread = read(_uart_fd, temp_buf, sizeof(temp_buf));
	}

	if (nread > 0) {
		_stats.rx_total_bytes += nread;

		if (_rx_buffer_len + (size_t)nread > RX_BUFFER_SIZE) {
			_rx_buffer_len = 0;
			_stats.rx_error_count++;
		}
		memcpy(_rx_buffer + _rx_buffer_len, temp_buf, nread);
		_rx_buffer_len += nread;
	}

	while (_rx_buffer_len >= 8) {
		size_t magic_idx = 0;

		while (magic_idx < _rx_buffer_len && _rx_buffer[magic_idx] != FRAME_MAGIC_BYTE) {
			magic_idx++;
		}

		if (magic_idx > 0) {
			memmove(_rx_buffer, _rx_buffer + magic_idx, _rx_buffer_len - magic_idx);
			_rx_buffer_len -= magic_idx;
		}

		if (_rx_buffer_len < 6) {
			break;
		}

		uint8_t expected_header_crc = 0;

		for (int i = 0; i < 5; i++) {
			expected_header_crc ^= _rx_buffer[i];
		}

		if (expected_header_crc != _rx_buffer[5]) {
			memmove(_rx_buffer, _rx_buffer + 1, _rx_buffer_len - 1);
			_rx_buffer_len -= 1;
			_stats.crc_error_count++;
			continue;
		}

		uint8_t msg_type = _rx_buffer[1];
		uint16_t payload_len = _rx_buffer[3] | (_rx_buffer[4] << 8);

		if (payload_len > 512) {
			memmove(_rx_buffer, _rx_buffer + 1, _rx_buffer_len - 1);
			_rx_buffer_len -= 1;
			continue;
		}

		size_t frame_len = 8 + payload_len;

		if (_rx_buffer_len < frame_len) {
			break;
		}

		uint16_t expected_frame_crc = crc16_signature(CRC16_INITIAL, 5 + payload_len, _rx_buffer + 1);
		uint16_t received_frame_crc = _rx_buffer[6 + payload_len] | (_rx_buffer[7 + payload_len] << 8);

		if (expected_frame_crc != received_frame_crc) {
			memmove(_rx_buffer, _rx_buffer + 1, _rx_buffer_len - 1);
			_rx_buffer_len -= 1;
			_stats.crc_error_count++;
			continue;
		}

		_handle_received_frame(msg_type, _rx_buffer + 6, payload_len);
		_stats.rx_message_count++;

		memmove(_rx_buffer, _rx_buffer + frame_len, _rx_buffer_len - frame_len);
		_rx_buffer_len -= frame_len;
	}
}

void PBLink::_handle_received_frame(uint8_t msg_type, const uint8_t *payload, size_t len)
{
	if (msg_type == static_cast<uint8_t>(ProtoMsgType::SUBSCRIPTION_REQUEST)) {
		px4_pblink_msgs_SubscriptionRequest proto_msg = px4_pblink_msgs_SubscriptionRequest_init_default;
		pb_istream_t stream = pb_istream_from_buffer(payload, len);

		if (pb_decode(&stream, px4_pblink_msgs_SubscriptionRequest_fields, &proto_msg)) {
			int idx = msg_type_id_to_index(proto_msg.msg_type_id);

			if (idx >= 0 && idx < (int)UPLINK_TOPICS_COUNT) {
				_set_uplink_interval((size_t)idx, proto_msg.interval_us);
				PX4_INFO("Topic 0x%02" PRIX32 " rate updated: %" PRIu32 " us",
					 proto_msg.msg_type_id, proto_msg.interval_us);
			}
		}

		return;
	}

	if (msg_type == static_cast<uint8_t>(ProtoMsgType::PARAMETER_REQUEST)) {
		px4_pblink_msgs_ParameterRequest proto_msg = px4_pblink_msgs_ParameterRequest_init_default;
		pb_istream_t stream = pb_istream_from_buffer(payload, len);

		if (pb_decode(&stream, px4_pblink_msgs_ParameterRequest_fields, &proto_msg)) {
			param_t handle = param_find(proto_msg.param_name);

			px4_pblink_msgs_ParameterValue resp = px4_pblink_msgs_ParameterValue_init_default;
			resp.timestamp = hrt_absolute_time();
			pblink_copy_string(resp.param_name, sizeof(resp.param_name),
					      proto_msg.param_name, sizeof(proto_msg.param_name));
			resp.request_id = proto_msg.request_id;
			resp.success = (handle != PARAM_INVALID);

			if (handle != PARAM_INVALID) {
				param_type_t type = param_type(handle);
				if (type == PARAM_TYPE_INT32) {
					int32_t val;
					param_get(handle, &val);
					resp.value_int = val;
					resp.param_type = PARAM_TYPE_INT_PROTO;
				} else if (type == PARAM_TYPE_FLOAT) {
					float val;
					param_get(handle, &val);
					resp.value_float = val;
					resp.param_type = PARAM_TYPE_FLOAT_PROTO;
				}
			}

			uint8_t buffer[256];
			pb_ostream_t ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
			if (pb_encode(&ostream, px4_pblink_msgs_ParameterValue_fields, &resp)) {
				_send_proto_frame(ProtoMsgType::PARAMETER_VALUE, buffer, ostream.bytes_written);
			}
		}
		return;
	}

	if (msg_type == static_cast<uint8_t>(ProtoMsgType::PARAMETER_SET)) {
		px4_pblink_msgs_ParameterSet proto_msg = px4_pblink_msgs_ParameterSet_init_default;
		pb_istream_t stream = pb_istream_from_buffer(payload, len);

		if (pb_decode(&stream, px4_pblink_msgs_ParameterSet_fields, &proto_msg)) {
			param_t handle = param_find(proto_msg.param_name);
			bool success = false;

			if (handle != PARAM_INVALID) {
				if (proto_msg.param_type == PARAM_TYPE_INT_PROTO) {
					int32_t val = proto_msg.value_int;
					param_set(handle, &val);
					success = true;
				} else {
					float val = proto_msg.value_float;
					param_set(handle, &val);
					success = true;
				}
			}

			px4_pblink_msgs_ParameterValue resp = px4_pblink_msgs_ParameterValue_init_default;
			resp.timestamp = hrt_absolute_time();
			pblink_copy_string(resp.param_name, sizeof(resp.param_name),
					      proto_msg.param_name, sizeof(proto_msg.param_name));
			resp.request_id = proto_msg.request_id;
			resp.success = success;

			if (success) {
				if (proto_msg.param_type == PARAM_TYPE_INT_PROTO) {
					resp.value_int = proto_msg.value_int;
					resp.param_type = PARAM_TYPE_INT_PROTO;
				} else {
					resp.value_float = proto_msg.value_float;
					resp.param_type = PARAM_TYPE_FLOAT_PROTO;
				}
			}

			uint8_t buffer[256];
			pb_ostream_t ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
			if (pb_encode(&ostream, px4_pblink_msgs_ParameterValue_fields, &resp)) {
				_send_proto_frame(ProtoMsgType::PARAMETER_VALUE, buffer, ostream.bytes_written);
			}
		}
		return;
	}

	// TimeSync response: echo companion tc1 and stamp PX4 ts1 in microseconds.
	if (msg_type == static_cast<uint8_t>(ProtoMsgType::TIME_SYNC)) {
		px4_pblink_msgs_TimeSync proto_msg = px4_pblink_msgs_TimeSync_init_default;
		pb_istream_t stream = pb_istream_from_buffer(payload, len);

		if (pb_decode(&stream, px4_pblink_msgs_TimeSync_fields, &proto_msg)) {
			px4_pblink_msgs_TimeSync resp = px4_pblink_msgs_TimeSync_init_default;
			resp.tc1 = proto_msg.tc1;
			resp.ts1 = hrt_absolute_time();

			uint8_t buffer[64];
			pb_ostream_t ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));

			if (pb_encode(&ostream, px4_pblink_msgs_TimeSync_fields, &resp)) {
				_send_proto_frame(ProtoMsgType::TIME_SYNC, buffer, ostream.bytes_written);
			}
		}

		return;
	}

	if (msg_type == static_cast<uint8_t>(ProtoMsgType::TOPIC_LIST_REQUEST)) {
		px4_pblink_msgs_TopicListRequest proto_msg = px4_pblink_msgs_TopicListRequest_init_default;
		pb_istream_t stream = pb_istream_from_buffer(payload, len);

		if (pb_decode(&stream, px4_pblink_msgs_TopicListRequest_fields, &proto_msg)) {
			px4_pblink_msgs_TopicListResponse resp = px4_pblink_msgs_TopicListResponse_init_default;
			resp.timestamp = hrt_absolute_time();
			resp.request_id = proto_msg.request_id;

			struct EncodeState {
				const char **topic_names;
				const uint32_t *msg_type_ids;
				const PBLink::RateLimiter *rate_limiters;
				size_t count;
			} state{UPLINK_TOPIC_NAMES, UPLINK_MSG_TYPE_IDS, _uplink_rate_limiters, UPLINK_TOPICS_COUNT};

			resp.topics.funcs.encode = [](pb_ostream_t *os, const pb_field_t *field, void * const *arg) -> bool {
				auto *st = static_cast<EncodeState *>(*arg);
				for (size_t i = 0; i < st->count; i++) {
					if (!pb_encode_tag_for_field(os, field)) {
						return false;
					}
					px4_pblink_msgs_TopicEntry entry = px4_pblink_msgs_TopicEntry_init_default;
					entry.msg_type_id = st->msg_type_ids[i];
					uint32_t interval_us = st->rate_limiters[i].get_interval();
					entry.current_rate_hz = (interval_us > 0) ? static_cast<uint32_t>(1e6f / interval_us) : 0;
					pblink_copy_string(entry.msg_name, sizeof(entry.msg_name), st->topic_names[i], strlen(st->topic_names[i]) + 1);

					if (!pb_encode_submessage(os, px4_pblink_msgs_TopicEntry_fields, &entry)) {
						return false;
					}
				}
				return true;
			};
			resp.topics.arg = &state;

			uint8_t buffer[2048];
			pb_ostream_t ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
			if (pb_encode(&ostream, px4_pblink_msgs_TopicListResponse_fields, &resp)) {
				_send_proto_frame(ProtoMsgType::TOPIC_LIST_RESPONSE, buffer, ostream.bytes_written);
			}
		}

		return;
	}

	if (msg_type == static_cast<uint8_t>(ProtoMsgType::LINK_STATUS_REQUEST)) {
		px4_pblink_msgs_LinkStatusRequest proto_msg = px4_pblink_msgs_LinkStatusRequest_init_default;
		pb_istream_t stream = pb_istream_from_buffer(payload, len);

		if (pb_decode(&stream, px4_pblink_msgs_LinkStatusRequest_fields, &proto_msg)) {
			px4_pblink_msgs_LinkStatusResponse resp = px4_pblink_msgs_LinkStatusResponse_init_default;
			resp.timestamp = hrt_absolute_time();
			resp.request_id = proto_msg.request_id;
			resp.tx_bytes_sec = static_cast<uint32_t>(_stats.tx_rate_avg);
			resp.rx_bytes_sec = static_cast<uint32_t>(_stats.rx_rate_avg);
			resp.tx_total_bytes = _stats.tx_total_bytes;
			resp.rx_total_bytes = _stats.rx_total_bytes;
			resp.tx_message_count = _stats.tx_message_count;
			resp.rx_message_count = _stats.rx_message_count;
			resp.tx_error_count = _stats.tx_error_count;
			resp.rx_error_count = _stats.rx_error_count;

			uint32_t active_count = 0;
			for (size_t i = 0; i < UPLINK_TOPICS_COUNT; i++) {
				if (_uplink_rate_limiters[i].get_interval() > 0) {
					active_count++;
				}
			}
			resp.active_topic_count = active_count;

			uint8_t buffer[128];
			pb_ostream_t ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
			if (pb_encode(&ostream, px4_pblink_msgs_LinkStatusResponse_fields, &resp)) {
				_send_proto_frame(ProtoMsgType::LINK_STATUS_RESPONSE, buffer, ostream.bytes_written);
			}
		}

		return;
	}

	PBLINK_HANDLE_DOWNLINK_SWITCH
}

bool PBLink::_transport_has_pending_data()
{
	int fd = -1;

#ifdef PBLINK_UDP_SUPPORTED
	if (_transport == TransportMode::UDP && _udp_initialized) {
		fd = _udp_fd;
	} else
#endif
	if (_transport == TransportMode::UART && _uart_initialized) {
		fd = _uart_fd;
	}

	if (fd < 0) {
		return false;
	}

	px4_pollfd_struct_t transport_pollfd {};
	transport_pollfd.fd = fd;
	transport_pollfd.events = POLLIN;

	return px4_poll(&transport_pollfd, 1, 0) > 0;
}

bool PBLink::_uplink_ready() const
{
#ifdef PBLINK_UDP_SUPPORTED
	if (_transport == TransportMode::UDP) {
		return _udp_initialized;
	}
#endif

	return _transport == TransportMode::UART && _uart_initialized;
}

void PBLink::_set_uplink_interval(size_t idx, uint32_t interval_us)
{
	if (idx >= UPLINK_TOPICS_COUNT) {
		return;
	}

	const uint32_t previous_interval_us = _uplink_rate_limiters[idx].get_interval();
	_uplink_rate_limiters[idx].configure(interval_us);

	if (previous_interval_us != interval_us) {
		_topic_stats[idx] = {};
	}

	for (size_t poll_idx = 0; poll_idx < UPLINK_POLL_TOPICS_COUNT; poll_idx++) {
		if (UPLINK_POLL_TOPIC_INDICES[poll_idx] == idx && _uplink_fds[poll_idx].fd >= 0) {
			_uplink_fds[poll_idx].events = (interval_us > 0) ? POLLIN : 0;
			_uplink_fds[poll_idx].revents = 0;
			orb_set_interval(_uplink_fds[poll_idx].fd, PBLINK_INTERVAL_US_TO_MS(interval_us));
			break;
		}
	}
}

void PBLink::run()
{
	(void)param_find("PBLINK_EN");

#ifdef PBLINK_UDP_SUPPORTED
	if (_transport == TransportMode::UDP) {
		if (_udp_init(_udp_peer_ip, _udp_port) != 0) {
			PX4_ERR("Failed to init UDP");
			return;
		}
	} else
#else
	if (_transport == TransportMode::UDP) {
		PX4_WARN("UDP not supported on this platform, switching to UART");
		_transport = TransportMode::UART;
	}
#endif
	{
		if (_uart_init(_uart_device, _uart_baud) != 0) {
			PX4_ERR("Failed to init UART %s", _uart_device);
			return;
		}
	}

	PX4_INFO("PBLink started (%s mode)", _transport == TransportMode::UDP ? "UDP" : "UART");

	for (size_t i = 0; i < UPLINK_TOPICS_COUNT; i++) {
		_set_uplink_interval(i, UPLINK_RATE_INTERVALS_US[i]);

		if (i < 8) {
			if (UPLINK_RATE_INTERVALS_US[i] > 0) {
				PX4_INFO("  Rate limiter[%zu]: %" PRIu32 " us (%.2f Hz)", i,
				         UPLINK_RATE_INTERVALS_US[i],
				         (double)(1e6f / UPLINK_RATE_INTERVALS_US[i]));
			} else {
				PX4_INFO("  Rate limiter[%zu]: unlimited", i);
			}
		}
	}

	bool uplink_subs_ok = true;
	PBLINK_INIT_UPLINK_SUBSCRIPTIONS(_uplink_fds, uplink_subs_ok);

	if (!uplink_subs_ok) {
		PX4_ERR("Failed to init uplink subscriptions");
		PBLINK_CLOSE_UPLINK_SUBSCRIPTIONS(_uplink_fds);
		return;
	}

	uint8_t proto_buffer[1024];
	int poll_error_counter = 0;

	while (!should_exit()) {
		if (!_uplink_ready()) {
			if (!_transport_has_pending_data()) {
				px4_usleep(10'000);
			}

			_process_incoming_data();
			_update_stats();
			continue;
		}

		const int orb_poll_timeout_ms = _transport_has_pending_data() ? 0 : 1;
		const int poll_ret = px4_poll(_uplink_fds, UPLINK_POLL_TOPICS_COUNT, orb_poll_timeout_ms);

		if (poll_ret > 0) {
			PBLINK_SEND_ALL_UPLINK;

		} else if (poll_ret < 0 && errno != EINTR) {
			if (poll_error_counter < 10 || poll_error_counter % 50 == 0) {
				PX4_ERR("ERROR while polling uorbs: %d", poll_ret);
			}

			poll_error_counter++;
		}

		_process_incoming_data();
		_update_stats();
	}

	PBLINK_CLOSE_UPLINK_SUBSCRIPTIONS(_uplink_fds);
}

void PBLink::_update_stats()
{
	hrt_abstime now = hrt_absolute_time();
	float dt = (now - _stats.last_update) / 1e6f;

	if (dt >= 1.0f) {
		const float alpha = 0.2f;

		static uint64_t last_tx_bytes = 0;
		static uint64_t last_rx_bytes = 0;

		uint64_t tx_bytes_delta = _stats.tx_total_bytes - last_tx_bytes;
		uint64_t rx_bytes_delta = _stats.rx_total_bytes - last_rx_bytes;

		float tx_rate_instant = tx_bytes_delta / dt;
		float rx_rate_instant = rx_bytes_delta / dt;

		_stats.tx_rate_avg = alpha * tx_rate_instant + (1.0f - alpha) * _stats.tx_rate_avg;
		_stats.rx_rate_avg = alpha * rx_rate_instant + (1.0f - alpha) * _stats.rx_rate_avg;

		last_tx_bytes = _stats.tx_total_bytes;
		last_rx_bytes = _stats.rx_total_bytes;
		_stats.last_update = now;
	}
}

void PBLink::_print_topic_stats()
{
	PX4_INFO("");
	PX4_INFO("Topic Statistics:");
	PX4_INFO("  %-30s %8s %10s %10s %10s", "Topic", "Sent", "Rate (Hz)", "Expected", "State");

	for (size_t i = 0; i < UPLINK_TOPICS_COUNT; i++) {
		const auto &stats = _topic_stats[i];
		uint32_t interval_us = _uplink_rate_limiters[i].get_interval();
		float expected_hz = (interval_us > 0) ? (1e6f / interval_us) : 0.0f;

		if (stats.send_count > 0 && stats.first_send > 0) {
			float duration_sec = (stats.last_send - stats.first_send) / 1e6f;
			float actual_hz = 0.0f;

			if (duration_sec > 1.0f) {
				actual_hz = stats.send_count / duration_sec;
			}

			PX4_INFO("  %-30s %8" PRIu32 " %10.2f %10.2f %10s",
			         UPLINK_TOPIC_NAMES[i],
			         stats.send_count,
			         (double)actual_hz,
			         (double)expected_hz,
			         interval_us > 0 ? "active" : "disabled");
		} else {
			PX4_INFO("  %-30s %8s %10s %10.2f %10s",
			         UPLINK_TOPIC_NAMES[i],
			         "no data",
			         "-",
			         (double)expected_hz,
			         interval_us > 0 ? "no data" : "disabled");
		}
	}
}

void PBLink::parameters_update(bool force) {}

int PBLink::print_usage(const char *reason)
{
	if (reason) PX4_WARN("%s", reason);
	PRINT_MODULE_DESCRIPTION("Pblink - Bidirectional UART/UDP communication");
	PRINT_MODULE_USAGE_NAME("pblink", "module");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_PARAM_STRING('t', "udp", "serial|udp", "Transport protocol", true);
	PRINT_MODULE_USAGE_PARAM_STRING('d', nullptr, "<file:dev>", "Serial device", true);
	PRINT_MODULE_USAGE_PARAM_INT('b', 115200, 9600, 3000000, "Baudrate (can also be p:<param_name>)", true);
	PRINT_MODULE_USAGE_PARAM_STRING('h', "127.0.0.1", "<IP|p:param>", "UDP peer IP", true);
	PRINT_MODULE_USAGE_PARAM_INT('u', 14556, 1024, 65535, "UDP local port (can also be p:<param_name>)", true);
	PRINT_MODULE_USAGE_COMMAND("stop");
	PRINT_MODULE_USAGE_COMMAND("status");
	return 0;
}

extern "C" __EXPORT int pblink_main(int argc, char *argv[])
{
	return ModuleBase::main(PBLink::desc, argc, argv);
}
