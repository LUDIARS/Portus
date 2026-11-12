#pragma once
//MQTT 3.1.1 の購読用クライアント。使う機能 (CONNECT / SUBSCRIBE / 受信 PUBLISH の QoS 0・1 / keep-alive / 自動再接続) だけを持つ
//  送信 PUBLISH・QoS 2・will・TLS (mqtts://)・MQTT 5 は持たない
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "portus/event_loop.hpp"

namespace portus {

//---- パケットの組み立てと解析 (クライアントが送る分・受ける分だけ) ----

enum class MqttType : int {
	Connect = 1, Connack = 2, Publish = 3, Puback = 4, Pubrec = 5, Pubrel = 6, Pubcomp = 7,
	Subscribe = 8, Suback = 9, Unsubscribe = 10, Unsuback = 11, Pingreq = 12, Pingresp = 13, Disconnect = 14,
};

//固定ヘッダを外したパケット 1 つ。flags は固定ヘッダ下位 4 ビット
struct MqttPacket {
	MqttType type = MqttType::Connect;
	unsigned flags = 0;
	std::string body;
};

//壊れたパケット・想定外のパケット。受けたら接続を捨てる
class MqttProtocolError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

constexpr std::size_t kMqttDefaultMaxPacket = 1024 * 1024;
constexpr std::size_t kMqttMaxRemainingLength = 268435455;	//残り長さ (4 バイトの可変長整数) で表せる最大

//残り長さの可変長整数。表せない長さは std::invalid_argument
std::string encode_mqtt_remaining_length(std::size_t n);

//clean session で接続する。username / password は空なら送らない (password だけは std::invalid_argument)
std::string encode_mqtt_connect(std::string_view client_id, std::string_view username, std::string_view password, int keep_alive_sec);
std::string encode_mqtt_subscribe(std::uint16_t packet_id, std::string_view topic_filter, int qos);
std::string encode_mqtt_puback(std::uint16_t packet_id);
std::string encode_mqtt_pingreq();
std::string encode_mqtt_disconnect();

struct MqttConnack {
	bool session_present = false;
	int return_code = 0;
};

struct MqttSuback {
	std::uint16_t packet_id = 0;
	std::vector<int> return_codes;	//0x00 / 0x01 / 0x02 = 許可された QoS、0x80 = 拒否
};

//topic / payload は元のパケットの body を指す (パケットより長く持たない)
struct MqttPublish {
	std::string_view topic;
	std::string_view payload;
	int qos = 0;
	bool dup = false;
	bool retain = false;
	std::uint16_t packet_id = 0;	//QoS 0 では 0
};

//中身が仕様に合わなければ MqttProtocolError (topic は UTF-8 として正しく、空でなく、ワイルドカードを含まない)
MqttConnack decode_mqtt_connack(const MqttPacket& packet);
MqttSuback decode_mqtt_suback(const MqttPacket& packet);
MqttPublish decode_mqtt_publish(const MqttPacket& packet);

//受信バイト列からパケットを取り出す。固定ヘッダの時点で、本体を溜める前に上限を判断する
//  5 バイト以上の残り長さ・max_packet_bytes (固定ヘッダ込み) を超えるパケット・不正な種別やフラグは MqttProtocolError
class MqttPacketParser {
public:
	explicit MqttPacketParser(std::size_t max_packet_bytes = kMqttDefaultMaxPacket) : max_packet_(max_packet_bytes) {}
	std::vector<MqttPacket> push(std::string_view chunk);

private:
	bool next(MqttPacket& out);

	std::size_t max_packet_;
	std::string buffer_;
	std::size_t pos_ = 0;
};

//---- クライアント ----

struct MqttOptions {
	std::string url;			//mqtt://host[:port] (既定 1883)。mqtts:// は持たない
	std::string client_id;
	std::string username;		//空なら送らない
	std::string password;		//空なら送らない。username 無しでは指定できない
	int keep_alive_sec = 60;	//0 で keep-alive なし
	int reconnect_ms = 5000;	//切れてから次の接続を始めるまで
	int connect_timeout_ms = 10000;	//TCP 接続から CONNACK まで
	std::size_t max_packet_bytes = kMqttDefaultMaxPacket;	//超えるパケットが来たら接続を捨てて再接続
};

//購読用の MQTT クライアント。切れたら reconnect_ms 後に繋ぎ直す (end を呼ぶまで続ける)
//  コールバックはイベントループのスレッドで呼ばれる (中でブロックしない)。start の前、またはループスレッドで設定する
//  start / subscribe / end はどのスレッドからでも呼べる。コールバックの中から end を呼んでも、手放してもよい
//  購読は接続をまたいで覚えない。on_connect の中で毎回 subscribe する
class MqttClient : public std::enable_shared_from_this<MqttClient> {
public:
	std::function<void()> on_connect;	//CONNACK で受け入れられるたび
	std::function<void(std::string_view topic, std::string_view payload)> on_message;	//QoS 1 は戻った後に PUBACK を返す
	std::function<void()> on_reconnect;	//再接続を始めるとき
	std::function<void()> on_close;		//接続が終わった / 接続できなかったとき
	std::function<void(const std::string& message)> on_error;

	//URL と設定を検証する。不正なら std::invalid_argument
	static std::shared_ptr<MqttClient> create(EventLoop& loop, MqttOptions options);
	~MqttClient();
	MqttClient(const MqttClient&) = delete;
	MqttClient& operator=(const MqttClient&) = delete;

	void start();
	//接続中だけ有効。done(false): 未接続 / ブローカーが 0x80 を返した / 応答の前に切れた。done はループスレッドで呼ばれる
	//qos は 0 か 1、topic_filter は空でない UTF-8 (違えば std::invalid_argument)
	void subscribe(std::string topic_filter, int qos, std::function<void(bool ok)> done = {});
	//接続中なら DISCONNECT を送って閉じ、再接続をやめる。接続 (または試行) の途中なら on_close が 1 回呼ばれる
	//以後コールバックは外す (クライアント自身を掴んだコールバックも解放されるように)
	void end();

private:
	struct Impl;
	MqttClient(EventLoop& loop, MqttOptions options);
	std::unique_ptr<Impl> impl_;
	friend void mqtt_set_keep_alive_ms_for_test(MqttClient& client, int ms);
};

//テスト用: keep-alive の間隔をミリ秒で上書きする (CONNECT で送る値は keep_alive_sec のまま)。start の前に呼ぶ
void mqtt_set_keep_alive_ms_for_test(MqttClient& client, int ms);

}
