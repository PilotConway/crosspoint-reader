#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <functional>

namespace dashboard {

/**
 * MQTT transport wrapper for connect/reconnect, subscribe, and publish.
 *
 * Ported from the standalone dashboard's paper::mqtt::MqttClient
 * (lib/mqtt_client/include/mqtt_client/MqttClient.h), trimmed to only the
 * weather topic for this DashboardActivity prototype.
 */
class MqttClient {
 public:
  using RawMessageCallback = std::function<void(const char* topic, const char* payload)>;

  MqttClient();

  void Begin(const char* host, int port, const char* username, const char* password, const char* client_id_prefix);

  // Returns true if this tick had bytes waiting on the socket (i.e. it went
  // through the read/diagnostic path), false if there was nothing to do.
  bool Loop();
  bool Connected();
  void Disconnect();

  // Diagnostics for the worker's heartbeat log. State() is PubSubClient's
  // MQTT_* code; Available() is unread bytes sitting in the socket.
  int State();
  int Available();

  // Attempts one non-blocking reconnect when disconnected. Tries at most once
  // per retry interval. On successful connect, subscribes to the weather topic.
  bool ReconnectIfNeeded();
  bool HasConnectedOnce() const;

  bool Publish(const char* topic, const char* payload);

  void SetRawMessageCallback(RawMessageCallback callback);

 private:
  static void StaticCallback(char* topic, byte* payload, unsigned int length);
  void OnMessage(char* topic, byte* payload, unsigned int length);
  void SubscribeDefaultTopics();

  WiFiClient wifi_client_;
  PubSubClient client_;

  String host_;
  int port_ = 1883;
  String username_;
  String password_;
  String client_id_prefix_;

  RawMessageCallback raw_callback_;
  uint32_t reconnect_interval_ms_ = 5000;
  uint32_t last_reconnect_attempt_ms_ = 0;
  uint32_t reconnect_log_interval_ms_ = 30000;
  uint32_t last_reconnect_log_ms_ = 0;
  uint32_t startup_delay_ms_ = 3000;
  uint32_t startup_not_before_ms_ = 0;
  bool has_connected_once_ = false;
  // Set by OnMessage(), cleared and read by Loop() around each client_.loop()
  // call, to tell "PubSubClient consumed bytes and silently discarded the
  // packet" apart from "the broker sent nothing" - see Loop().
  bool message_seen_ = false;
  static MqttClient* active_instance_;
};

}  // namespace dashboard
