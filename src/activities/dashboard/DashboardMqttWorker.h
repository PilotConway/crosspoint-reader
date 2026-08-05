#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "mqtt_dashboard_bridge/DashboardState.h"
#include "mqtt_dashboard_bridge/MqttClient.h"

// Runs dashboard::MqttClient on its own FreeRTOS task, decoupled from
// DashboardActivity's lifetime and from the main/render tasks.
//
// Why this exists: PubSubClient reads MQTT messages one byte at a time
// (see .pio/libdeps/default/PubSubClient/src/PubSubClient.cpp readPacket(),
// readByte()) - fine for small payloads, but the retained
// papers3/dashboard/layout message is large enough that reading it this way
// has been observed taking 50+ seconds, sometimes much longer if the
// connection is flaky and retries. Calling mqttClient.Loop() directly from
// DashboardActivity::loop() (as the weather/tasks-only prototype originally
// did) blocked touch input - including the Back gesture - for that entire
// stretch, making the whole screen appear frozen.
//
// Ownership/lifetime: heap-allocated via Start(); DashboardActivity never
// deletes it directly. RequestStop() is non-blocking (flips a flag) so
// DashboardActivity::onExit() never waits on a possibly-still-stuck network
// read. The worker never touches DashboardActivity - it's a passive data
// store that DashboardActivity polls via TryTake*() each loop() tick - so
// there's no dangling-pointer risk from the worker's side either: once
// RequestStop() returns, DashboardActivity drops its pointer and never calls
// into the worker again, while the worker finishes its current operation on
// its own schedule and self-deletes (`delete this` + vTaskDelete(nullptr)).
//
// dashboard::MqttClient itself is NOT thread-safe (PubSubClient shares one
// internal buffer + socket) - it must only ever be touched from this
// worker's own task. Cross-task data (inbound parsed payloads, outbound
// publish requests, connection status) all goes through mutex_ with short,
// network-I/O-free critical sections; the slow byte-at-a-time socket read
// itself happens outside any lock.
class DashboardMqttWorker {
 public:
  static DashboardMqttWorker* Start(const char* host, int port, const char* user, const char* password);

  // Non-blocking. Safe to call from the main thread. Do not use this pointer
  // again after calling this.
  void RequestStop();

  bool IsConnected() const { return connected_; }
  bool HasConnectedOnce() const { return hasConnectedOnce_; }

  // Each TryTake* copies out and clears whatever's pending, returning true if
  // there was anything new. Cheap - just takes mutex_ briefly, no network I/O.
  bool TryTakeWeather(dashboard::WeatherState& out);
  bool TryTakeTasks(dashboard::TasksState& out);
  bool TryTakeHomeCards(dashboard::DashboardPageCards& out);
  bool TryTakeRemoteCards(dashboard::DashboardPageCards& out);
  bool TryTakeHomeStatePatch(dashboard::DashboardCardStatePatch* out, int maxItems, int& outCount);
  bool TryTakeRemoteStatePatch(dashboard::DashboardCardStatePatch* out, int maxItems, int& outCount);

  // Non-blocking hand-off - the worker task publishes it on its own schedule.
  void RequestPublish(const String& action, const String& entityId, const String& dataJson);

 private:
  struct PendingCommand {
    bool hasCommand = false;
    String action;
    String entityId;
    String dataJson;
  };

  DashboardMqttWorker(const char* host, int port, const char* user, const char* password);

  static void TaskTrampoline(void* param);
  void RunTaskLoop();
  void HandleMessage(const char* topic, const char* payload);
  void DrainPendingCommand();

  SemaphoreHandle_t mutex_;
  volatile bool stopRequested_ = false;
  // Updated by the worker task each loop iteration; read directly (no mutex)
  // from the main thread - single-writer word-sized status flags, same
  // pattern as other volatile status flags in this codebase.
  volatile bool connected_ = false;
  volatile bool hasConnectedOnce_ = false;
  // Worker-task-only; see the heartbeat block in RunTaskLoop().
  uint32_t lastHeartbeatMs_ = 0;

  String host_;
  int port_;
  String user_;
  String password_;

  dashboard::MqttClient mqttClient_;

  // Guarded by mutex_.
  bool weatherPending_ = false;
  dashboard::WeatherState pendingWeather_;
  bool tasksPending_ = false;
  dashboard::TasksState pendingTasks_;
  bool homeCardsPending_ = false;
  dashboard::DashboardPageCards pendingHomeCards_;
  bool remoteCardsPending_ = false;
  dashboard::DashboardPageCards pendingRemoteCards_;
  bool homeStatePending_ = false;
  dashboard::DashboardCardStatePatch pendingHomeState_[dashboard::DashboardPageCards::kMaxCards];
  int pendingHomeStateCount_ = 0;
  bool remoteStatePending_ = false;
  dashboard::DashboardCardStatePatch pendingRemoteState_[dashboard::DashboardPageCards::kMaxCards];
  int pendingRemoteStateCount_ = 0;
  PendingCommand pendingCommand_;
};
