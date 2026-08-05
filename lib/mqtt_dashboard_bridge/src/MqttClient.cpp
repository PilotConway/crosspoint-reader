#include "mqtt_dashboard_bridge/MqttClient.h"

#include <Arduino.h>
#include <Logging.h>

#include "mqtt_dashboard_bridge/DashboardTopics.h"

namespace {
constexpr const char* kLogTag = "DASH_MQTT";
}

namespace dashboard {

MqttClient* MqttClient::active_instance_ = nullptr;

MqttClient::MqttClient() : client_(wifi_client_) { active_instance_ = this; }

void MqttClient::Begin(const char* host, int port, const char* username, const char* password,
                       const char* client_id_prefix) {
  host_ = host != nullptr ? String(host) : "";
  port_ = port;
  username_ = username != nullptr ? String(username) : "";
  password_ = password != nullptr ? String(password) : "";
  client_id_prefix_ = client_id_prefix != nullptr ? String(client_id_prefix) : "CrossPointDashboard";

  client_.setServer(host_.c_str(), port_);
  client_.setCallback(StaticCallback);
  // Dashboard layout payloads (card grid + labels) can be large - matches the
  // standalone dashboard's own buffer size (lib/mqtt_client/src/MqttClient.cpp)
  // since it's the same HA automation producing the same payload sizes. A
  // smaller buffer here would silently truncate large layouts and fail to
  // parse with no obvious symptom on-device.
  client_.setBufferSize(49152);
  // This value is a *blocking* budget, not just a patience knob: PubSubClient
  // busy-waits on it in two places (see
  // .pio/libdeps/default/PubSubClient/src/PubSubClient.cpp) - readByte()
  // spins `while(!_client->available())` mid-packet, and connect() spins
  // waiting for CONNACK. Both run on DashboardMqttWorker's task, so a large
  // value doesn't freeze the UI, but it does stall this client for that long,
  // which previously wedged the subscribe sequence partway through (see
  // SubscribeDefaultTopics()) and delayed every reconnect by the full
  // timeout. 5s is enough slack for a slow broker without making a single
  // stalled read cost most of a minute.
  client_.setSocketTimeout(5);
  startup_not_before_ms_ = millis() + startup_delay_ms_;
}

bool MqttClient::Loop() {
  // Diagnostic: PubSubClient has exactly one silent-drop path - readPacket()
  // sets `len = 0` when a packet exceeds bufferSize, and loop() then does
  // nothing at all: no callback, no state change, no disconnect. That is
  // indistinguishable from "the broker never sent it" unless we watch the
  // socket ourselves. Logging bytes-pending vs. callback-fired separates the
  // two: bytes consumed with callback=0 means PubSubClient ate and discarded
  // a packet; no bytes pending at all means it never arrived.
  const int pending = wifi_client_.available();
  if (pending <= 0) {
    client_.loop();
    return false;
  }
  message_seen_ = false;
  client_.loop();
  LOG_DBG(kLogTag, "Read: %d bytes pending -> %d left, callback=%d, state=%d, connected=%d", pending,
          wifi_client_.available(), message_seen_ ? 1 : 0, client_.state(), client_.connected() ? 1 : 0);
  return true;
}

bool MqttClient::Connected() { return client_.connected(); }

int MqttClient::State() { return client_.state(); }

int MqttClient::Available() { return wifi_client_.available(); }

void MqttClient::Disconnect() {
  if (client_.connected()) {
    client_.disconnect();
  }
}

bool MqttClient::ReconnectIfNeeded() {
  if (client_.connected()) {
    return true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (host_.isEmpty()) {
    const uint32_t now_log = millis();
    if (last_reconnect_log_ms_ == 0 || (now_log - last_reconnect_log_ms_) >= reconnect_log_interval_ms_) {
      LOG_ERR(kLogTag, "DASHBOARD_MQTT_SERVER is empty - set it in platformio.local.ini (see .example)");
      last_reconnect_log_ms_ = now_log;
    }
    return false;
  }

  if (!has_connected_once_ && startup_not_before_ms_ != 0) {
    const uint32_t now_startup = millis();
    if (now_startup < startup_not_before_ms_) {
      return false;
    }
  }

  const uint32_t now = millis();
  if (last_reconnect_attempt_ms_ != 0 && (now - last_reconnect_attempt_ms_) < reconnect_interval_ms_) {
    return false;
  }
  last_reconnect_attempt_ms_ = now;

  // Force a fresh TCP socket. PubSubClient::connect() skips its own
  // _client->connect() and reuses the existing socket whenever
  // _client->connected() is still true - so after a CONNACK timeout (which
  // leaves the socket in whatever state the broker left it), the next attempt
  // can write a second CONNECT onto a connection that already has a session.
  // The broker treats that as a protocol violation and drops us, which looks
  // from here like a connect that "succeeded" and then went silent: SUBSCRIBEs
  // get written and ACKed by nobody, no retained messages ever arrive, and
  // connected() only goes false half a second later.
  wifi_client_.stop();

  String client_id = client_id_prefix_ + "_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (client_.connect(client_id.c_str(), username_.c_str(), password_.c_str())) {
    SubscribeDefaultTopics();
    client_.publish(Topics::kTasksRefresh, "1");
    last_reconnect_attempt_ms_ = 0;
    last_reconnect_log_ms_ = 0;
    has_connected_once_ = true;
    return true;
  }

  const uint32_t now_log = millis();
  if (last_reconnect_log_ms_ == 0 || (now_log - last_reconnect_log_ms_) >= reconnect_log_interval_ms_) {
    LOG_ERR(kLogTag, "MQTT connect to %s:%d failed, PubSubClient state=%d", host_.c_str(), port_, client_.state());
    last_reconnect_log_ms_ = now_log;
  }
  return false;
}

bool MqttClient::HasConnectedOnce() const { return has_connected_once_; }

bool MqttClient::Publish(const char* topic, const char* payload) {
  if (!client_.connected() || topic == nullptr || payload == nullptr) {
    return false;
  }
  return client_.publish(topic, payload);
}

void MqttClient::SetRawMessageCallback(RawMessageCallback callback) { raw_callback_ = std::move(callback); }

void MqttClient::StaticCallback(char* topic, byte* payload, unsigned int length) {
  if (active_instance_ != nullptr) {
    active_instance_->OnMessage(topic, payload, length);
  }
}

void MqttClient::OnMessage(char* topic, byte* payload, unsigned int length) {
  message_seen_ = true;
  if (raw_callback_ == nullptr) {
    return;
  }

  String payload_text;
  payload_text.reserve(length + 1);
  for (unsigned int i = 0; i < length; ++i) {
    payload_text += static_cast<char>(payload[i]);
  }

  raw_callback_(topic, payload_text.c_str());
}

void MqttClient::SubscribeDefaultTopics() {
  // All four SUBSCRIBEs are written back-to-back with no reading in between,
  // and the backlog of SUBACKs plus retained-message pushes is drained by the
  // normal Loop() calls in DashboardMqttWorker::RunTaskLoop().
  //
  // An earlier version interleaved a short read window after each subscribe,
  // on the theory that PubSubClient::loop() draining only one packet per call
  // would let a backlog build. It does - but that backlog is harmless, and
  // reading here is not: readByte() busy-waits up to the socket timeout
  // mid-packet, so one stalled read stalls this whole function. That is what
  // kept papers3/dashboard/layout from ever being subscribed on a live
  // connection: the read after the tasks subscribe wedged, and the layout and
  // state SUBSCRIBEs below were simply never sent. Weather and tasks kept
  // working, so from the outside it looked like the broker was withholding
  // one specific retained topic.
  //
  // Note these booleans only report that the packet was written, NOT that the
  // broker granted the subscription - PubSubClient discards SUBACK return
  // codes entirely, so a topic denied by a broker ACL still reports 1 here.
  const bool weatherOk = client_.subscribe(Topics::kWeatherState);
  const bool tasksOk = client_.subscribe(Topics::kTasksState);
  const bool layoutOk = client_.subscribe(Topics::kDashboardLayout);
  const bool stateOk = client_.subscribe(Topics::kDashboardState);
  LOG_DBG(kLogTag, "Subscribe written (not granted): weather=%d tasks=%d layout=%d state=%d", weatherOk, tasksOk,
          layoutOk, stateOk);
}

}  // namespace dashboard
