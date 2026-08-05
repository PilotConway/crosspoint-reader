#include "DashboardMqttWorker.h"

#include <Logging.h>

#include <cassert>
#include <string.h>

#include "mqtt_dashboard_bridge/DashboardTopics.h"
#include "mqtt_dashboard_bridge/HaMessages.h"

namespace {
constexpr const char* kLogTag = "DASH_WORKER";
constexpr TickType_t kTaskLoopDelay = pdMS_TO_TICKS(20);
// Bound on how long the main/touch thread will ever wait for mutex_ in the
// TryTake*()/RequestPublish() calls below. The worker's own critical
// sections only ever do parsing/flag-setting (never network I/O - see the
// class comment in DashboardMqttWorker.h), so in practice this lock is held
// for at most a few ms and this timeout should never actually fire. It's
// here as a hard ceiling so the main thread can never block indefinitely on
// this lock even if that invariant is ever violated by a future change -
// the whole point of this worker is to keep MQTT from freezing touch input,
// so that guarantee shouldn't depend on every future edit getting the
// locking discipline right.
constexpr TickType_t kMainThreadMutexTimeout = pdMS_TO_TICKS(50);
constexpr uint32_t kHeartbeatIntervalMs = 5000;
}  // namespace

DashboardMqttWorker::DashboardMqttWorker(const char* host, int port, const char* user, const char* password)
    : mutex_(xSemaphoreCreateMutex()),
      host_(host != nullptr ? host : ""),
      port_(port),
      user_(user != nullptr ? user : ""),
      password_(password != nullptr ? password : "") {}

DashboardMqttWorker* DashboardMqttWorker::Start(const char* host, int port, const char* user,
                                                const char* password) {
  auto* worker = new DashboardMqttWorker(host, port, user, password);
  TaskHandle_t handle = nullptr;
  xTaskCreate(&DashboardMqttWorker::TaskTrampoline, "DashMqttWorker",
              // Stack size (bytes). This task runs a deep chain on every
              // incoming message - RunTaskLoop -> MqttClient::Loop ->
              // PubSubClient::loop -> OnMessage -> std::function ->
              // HandleMessage -> deserializeJson - and logPrintf() puts a
              // 256-byte buffer on the stack at several of those levels
              // (lib/Logging/Logging.cpp). 6144 was observed to wedge the task
              // partway through handling the 7.3KB tasks payload, which then
              // stopped anything from draining the socket at all. The
              // heartbeat in RunTaskLoop() reports the actual high-water mark
              // so this number can be tuned against measurement.
              12288,
              worker,
              1,  // Priority - matches ActivityManager's render task.
              &handle);
  assert(handle != nullptr && "Failed to create DashboardMqttWorker task");
  return worker;
}

void DashboardMqttWorker::RequestStop() { stopRequested_ = true; }

void DashboardMqttWorker::TaskTrampoline(void* param) {
  auto* self = static_cast<DashboardMqttWorker*>(param);
  self->RunTaskLoop();
}

void DashboardMqttWorker::RunTaskLoop() {
  mqttClient_.Begin(host_.c_str(), port_, user_.c_str(), password_.c_str(), "CrossPointDashboard");
  mqttClient_.SetRawMessageCallback(
      [this](const char* topic, const char* payload) { HandleMessage(topic, payload); });

  while (!stopRequested_) {
    const bool wasConnected = connected_;
    if (!mqttClient_.Connected()) {
      mqttClient_.ReconnectIfNeeded();
    } else {
      mqttClient_.Loop();
      DrainPendingCommand();
    }
    connected_ = mqttClient_.Connected();
    hasConnectedOnce_ = mqttClient_.HasConnectedOnce();
    if (connected_ && !wasConnected) {
      LOG_DBG(kLogTag, "MQTT connected, subscriptions active");
    } else if (wasConnected && !connected_) {
      LOG_DBG(kLogTag, "MQTT disconnected, will retry");
    }

    // Heartbeat. Without this there is no way to tell "the task is idle
    // because the broker sent nothing" from "the task is wedged and nothing is
    // draining the socket" - both look identical (silence) in the log, and the
    // second one was the actual cause of papers3/dashboard/layout never
    // arriving. stackFree is the low-water mark in bytes: if it approaches
    // zero, raise the stack size in Start().
    const uint32_t now = millis();
    if (now - lastHeartbeatMs_ >= kHeartbeatIntervalMs) {
      lastHeartbeatMs_ = now;
      LOG_DBG(kLogTag, "Alive: stackFree=%u connected=%d state=%d pending=%d",
              static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), connected_ ? 1 : 0, mqttClient_.State(),
              mqttClient_.Available());
    }

    vTaskDelay(kTaskLoopDelay);
  }

  LOG_DBG(kLogTag, "Stopping, disconnecting MQTT");
  mqttClient_.Disconnect();
  vSemaphoreDelete(mutex_);
  delete this;
  vTaskDelete(nullptr);
}

void DashboardMqttWorker::HandleMessage(const char* topic, const char* payload) {
  if (topic == nullptr) {
    return;
  }

  const size_t payloadLen = payload != nullptr ? strlen(payload) : 0;
  LOG_DBG(kLogTag, "Message on %s (%u bytes)", topic, static_cast<unsigned>(payloadLen));

  if (strcmp(topic, dashboard::Topics::kDashboardLayout) == 0 && payload != nullptr) {
    // First ~150 chars reveal the actual top-level JSON keys HA is publishing
    // (e.g. "dashboards":{"home":...) without spamming serial with a
    // potentially tens-of-KB payload.
    char snippet[151];
    const size_t copyLen = payloadLen < 150 ? payloadLen : 150;
    memcpy(snippet, payload, copyLen);
    snippet[copyLen] = '\0';
    LOG_DBG(kLogTag, "Layout payload starts: %s%s", snippet, payloadLen > 150 ? "..." : "");
  }

  if (strcmp(topic, dashboard::Topics::kWeatherState) == 0) {
    dashboard::WeatherState parsed;
    if (dashboard::ParseWeatherPayload(payload, parsed)) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      pendingWeather_ = parsed;
      weatherPending_ = true;
      xSemaphoreGive(mutex_);
    }
    return;
  }

  if (strcmp(topic, dashboard::Topics::kTasksState) == 0) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (dashboard::ParseTasksPayload(payload, pendingTasks_)) {
      tasksPending_ = true;
    }
    xSemaphoreGive(mutex_);
    return;
  }

  if (strcmp(topic, dashboard::Topics::kDashboardLayout) == 0) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool homeOk = dashboard::ParseDashboardCardsPayload(payload, "home", pendingHomeCards_);
    if (homeOk) {
      homeCardsPending_ = true;
    }
    const bool remoteOk = dashboard::ParseDashboardCardsPayload(payload, "remote", pendingRemoteCards_);
    if (remoteOk) {
      remoteCardsPending_ = true;
    }
    const int homeCount = homeOk ? pendingHomeCards_.count : -1;
    const int remoteCount = remoteOk ? pendingRemoteCards_.count : -1;
    xSemaphoreGive(mutex_);
    LOG_DBG(kLogTag, "Layout parsed: home ok=%d count=%d, remote ok=%d count=%d", homeOk, homeCount, remoteOk,
            remoteCount);
    return;
  }

  if (strcmp(topic, dashboard::Topics::kDashboardState) == 0) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (dashboard::ParseDashboardStatePayload(payload, "home", pendingHomeState_,
                                              dashboard::DashboardPageCards::kMaxCards, pendingHomeStateCount_)) {
      homeStatePending_ = true;
    }
    if (dashboard::ParseDashboardStatePayload(payload, "remote", pendingRemoteState_,
                                              dashboard::DashboardPageCards::kMaxCards, pendingRemoteStateCount_)) {
      remoteStatePending_ = true;
    }
    xSemaphoreGive(mutex_);
    return;
  }
}

void DashboardMqttWorker::DrainPendingCommand() {
  PendingCommand cmd;
  bool has = false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (pendingCommand_.hasCommand) {
    cmd = pendingCommand_;
    pendingCommand_.hasCommand = false;
    has = true;
  }
  xSemaphoreGive(mutex_);
  if (!has) {
    return;
  }

  dashboard::EntityActionCommand actionCmd;
  actionCmd.action = cmd.action;
  actionCmd.entity_id = cmd.entityId;
  actionCmd.data_json = cmd.dataJson;

  String payload;
  if (!dashboard::BuildEntityActionPayload(actionCmd, payload)) {
    LOG_ERR(kLogTag, "Failed to build dashboard action payload for %s", cmd.entityId.c_str());
    return;
  }
  if (!mqttClient_.Publish(dashboard::Topics::kCmd, payload.c_str())) {
    LOG_ERR(kLogTag, "Failed to publish dashboard action for %s", cmd.entityId.c_str());
  }
}

void DashboardMqttWorker::RequestPublish(const String& action, const String& entityId, const String& dataJson) {
  if (xSemaphoreTake(mutex_, kMainThreadMutexTimeout) != pdTRUE) {
    LOG_ERR(kLogTag, "Timed out taking mutex_ in RequestPublish, dropping command for %s", entityId.c_str());
    return;
  }
  pendingCommand_.hasCommand = true;
  pendingCommand_.action = action;
  pendingCommand_.entityId = entityId;
  pendingCommand_.dataJson = dataJson;
  xSemaphoreGive(mutex_);
}

bool DashboardMqttWorker::TryTakeWeather(dashboard::WeatherState& out) {
  if (xSemaphoreTake(mutex_, kMainThreadMutexTimeout) != pdTRUE) {
    return false;
  }
  const bool has = weatherPending_;
  if (has) {
    out = pendingWeather_;
    weatherPending_ = false;
  }
  xSemaphoreGive(mutex_);
  return has;
}

bool DashboardMqttWorker::TryTakeTasks(dashboard::TasksState& out) {
  if (xSemaphoreTake(mutex_, kMainThreadMutexTimeout) != pdTRUE) {
    return false;
  }
  const bool has = tasksPending_;
  if (has) {
    out = pendingTasks_;
    tasksPending_ = false;
  }
  xSemaphoreGive(mutex_);
  return has;
}

bool DashboardMqttWorker::TryTakeHomeCards(dashboard::DashboardPageCards& out) {
  if (xSemaphoreTake(mutex_, kMainThreadMutexTimeout) != pdTRUE) {
    return false;
  }
  const bool has = homeCardsPending_;
  if (has) {
    out = pendingHomeCards_;
    homeCardsPending_ = false;
  }
  xSemaphoreGive(mutex_);
  return has;
}

bool DashboardMqttWorker::TryTakeRemoteCards(dashboard::DashboardPageCards& out) {
  if (xSemaphoreTake(mutex_, kMainThreadMutexTimeout) != pdTRUE) {
    return false;
  }
  const bool has = remoteCardsPending_;
  if (has) {
    out = pendingRemoteCards_;
    remoteCardsPending_ = false;
  }
  xSemaphoreGive(mutex_);
  return has;
}

bool DashboardMqttWorker::TryTakeHomeStatePatch(dashboard::DashboardCardStatePatch* out, int maxItems,
                                                int& outCount) {
  if (xSemaphoreTake(mutex_, kMainThreadMutexTimeout) != pdTRUE) {
    return false;
  }
  const bool has = homeStatePending_;
  if (has) {
    outCount = pendingHomeStateCount_ > maxItems ? maxItems : pendingHomeStateCount_;
    for (int i = 0; i < outCount; ++i) {
      out[i] = pendingHomeState_[i];
    }
    homeStatePending_ = false;
  }
  xSemaphoreGive(mutex_);
  return has;
}

bool DashboardMqttWorker::TryTakeRemoteStatePatch(dashboard::DashboardCardStatePatch* out, int maxItems,
                                                  int& outCount) {
  if (xSemaphoreTake(mutex_, kMainThreadMutexTimeout) != pdTRUE) {
    return false;
  }
  const bool has = remoteStatePending_;
  if (has) {
    outCount = pendingRemoteStateCount_ > maxItems ? maxItems : pendingRemoteStateCount_;
    for (int i = 0; i < outCount; ++i) {
      out[i] = pendingRemoteState_[i];
    }
    remoteStatePending_ = false;
  }
  xSemaphoreGive(mutex_);
  return has;
}
