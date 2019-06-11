#include "api_connection.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"
#include "esphome/core/version.h"

#ifdef USE_DEEP_SLEEP
#include "esphome/components/deep_sleep/deep_sleep_component.h"
#endif
#ifdef USE_HOMEASSISTANT_TIME
#include "esphome/components/homeassistant/time/homeassistant_time.h"
#endif

namespace esphome {
namespace api {

static const char *TAG = "api.connection";

APIConnection::APIConnection(AsyncClient *client, APIServer *parent)
    : client_(client), parent_(parent), initial_state_iterator_(parent, this), list_entities_iterator_(parent, this) {
  this->client_->onError([](void *s, AsyncClient *c, int8_t error) { ((APIConnection *) s)->on_error_(error); }, this);
  this->client_->onDisconnect([](void *s, AsyncClient *c) {
    ((APIConnection *) s)->on_disconnect_();
    }, this);
  this->client_->onTimeout([](void *s, AsyncClient *c, uint32_t time) { ((APIConnection *) s)->on_timeout_(time); },
                           this);
  this->client_->onData([](void *s, AsyncClient *c, void *buf,
                           size_t len) { ((APIConnection *) s)->on_data_(reinterpret_cast<uint8_t *>(buf), len); },
                        this);

  this->send_buffer_.reserve(64);
  this->recv_buffer_.reserve(32);
  this->client_info_ = this->client_->remoteIP().toString().c_str();
  this->last_traffic_ = millis();
}
APIConnection::~APIConnection() { delete this->client_; }
void APIConnection::on_error_(int8_t error) {
  this->remove_ = true;
}
void APIConnection::on_disconnect_() {
  this->remove_ = true;
}
void APIConnection::on_timeout_(uint32_t time) {
  this->on_fatal_error();
}
void APIConnection::on_data_(uint8_t *buf, size_t len) {
  if (len == 0 || buf == nullptr)
    return;
  this->recv_buffer_.insert(this->recv_buffer_.end(), buf, buf + len);
}
void APIConnection::parse_recv_buffer_() {
  if (this->recv_buffer_.empty() || this->remove_)
    return;

  while (!this->recv_buffer_.empty()) {
    if (this->recv_buffer_[0] != 0x00) {
      ESP_LOGW(TAG, "Invalid preamble from %s", this->client_info_.c_str());
      this->on_fatal_error();
      return;
    }
    uint32_t i = 1;
    const uint32_t size = this->recv_buffer_.size();
    uint32_t msg_size = 0;
    while (i < size) {
      const uint8_t dat = this->recv_buffer_[i];
      msg_size |= (dat & 0x7F);
      // consume
      i += 1;
      if ((dat & 0x80) == 0x00) {
        break;
      } else {
        msg_size <<= 7;
      }
    }
    if (i == size)
      // not enough data there yet
      return;

    uint32_t msg_type = 0;
    bool msg_type_done = false;
    while (i < size) {
      const uint8_t dat = this->recv_buffer_[i];
      msg_type |= (dat & 0x7F);
      // consume
      i += 1;
      if ((dat & 0x80) == 0x00) {
        msg_type_done = true;
        break;
      } else {
        msg_type <<= 7;
      }
    }
    if (!msg_type_done)
      // not enough data there yet
      return;

    if (size - i < msg_size)
      // message body not fully received
      return;

    uint8_t *msg = &this->recv_buffer_[i];
    this->read_message(msg_size, msg_type, msg);
    if (this->remove_)
      return;
    // pop front
    uint32_t total = i + msg_size;
    this->recv_buffer_.erase(this->recv_buffer_.begin(), this->recv_buffer_.begin() + total);
    this->last_traffic_ = millis();
  }
}

void APIConnection::disconnect_client() {
  this->client_->close();
  this->remove_ = true;
}

void APIConnection::loop() {
  if (this->remove_)
    return;

  if (!network_is_connected()) {
    // when network is disconnected force disconnect immediately
    // don't wait for timeout
    this->on_fatal_error();
    return;
  }
  if (this->client_->disconnected()) {
    // failsafe for disconnect logic
    ESP_LOGV(TAG, "this->client_->disconnected()");
    this->on_disconnect_();
    return;
  }
  this->parse_recv_buffer_();

  this->list_entities_iterator_.advance();
  this->initial_state_iterator_.advance();

  const uint32_t keepalive = 60000;
  if (this->sent_ping_) {
    if (millis() - this->last_traffic_ > (keepalive * 3) / 2) {
      ESP_LOGW(TAG, "'%s' didn't respond to ping request in time. Disconnecting...", this->client_info_.c_str());
      this->disconnect_client();
    }
  } else if (millis() - this->last_traffic_ > keepalive) {
    this->sent_ping_ = true;
    this->send_ping_request(PingRequest());
  }

#ifdef USE_ESP32_CAMERA
  if (this->image_reader_.available()) {
    uint32_t space = this->client_->space();
    // reserve 15 bytes for metadata, and at least 64 bytes of data
    if (space >= 15 + 64) {
      uint32_t to_send = std::min(space - 15, this->image_reader_.available());
      auto buffer = this->get_buffer();
      // fixed32 key = 1;
      buffer.encode_fixed32(1, esp32_camera::global_esp32_camera->get_object_id_hash());
      // bytes data = 2;
      buffer.encode_bytes(2, this->image_reader_.peek_data_buffer(), to_send);
      // bool done = 3;
      bool done = this->image_reader_.available() == to_send;
      buffer.encode_bool(3, done);
      bool success = this->send_buffer(APIMessageType::CAMERA_IMAGE_RESPONSE);
      if (success) {
        this->image_reader_.consume_data(to_send);
      }
      if (success && done) {
        this->image_reader_.return_image();
      }
    }
  }
#endif
}

#ifdef USE_BINARY_SENSOR
bool APIConnection::send_binary_sensor_state(binary_sensor::BinarySensor *binary_sensor, bool state) {
  if (!this->state_subscription_)
    return false;

  BinarySensorStateResponse resp;
  resp.key = binary_sensor->get_object_id_hash();
  resp.state = state;
  return this->send_binary_sensor_state_response(resp);
}
#endif

#ifdef USE_COVER
bool APIConnection::send_cover_state(cover::Cover *cover) {
  if (!this->state_subscription_)
    return false;

  auto traits = cover->get_traits();
  CoverStateResponse resp{};
  resp.key = cover->get_object_id_hash();
  resp.legacy_state = (cover->position == cover::COVER_OPEN) ? LEGACY_COVER_STATE_OPEN : LEGACY_COVER_STATE_CLOSED;
  resp.position = cover->position;
  if (traits.get_supports_tilt())
    resp.tilt = cover->tilt;
  resp.current_operation = static_cast<CoverOperation>(cover->current_operation);
  return this->send_cover_state_response(resp);
}
#endif

#ifdef USE_FAN
bool APIConnection::send_fan_state(fan::FanState *fan) {
  if (!this->state_subscription_)
    return false;

  auto traits = fan->get_traits();
  FanStateResponse resp{};
  resp.key = fan->get_object_id_hash();
  resp.state = fan->state;
  if (traits.supports_oscillation())
    resp.oscillating = fan->oscillating;
  if (traits.supports_speed())
    resp.speed = static_cast<FanSpeed>(fan->speed);
  return this->send_fan_state_response(resp);
}
#endif

#ifdef USE_LIGHT
bool APIConnection::send_light_state(light::LightState *light) {
  if (!this->state_subscription_)
    return false;

  auto traits = light->get_traits();
  auto values = light->remote_values;
  LightStateResponse resp{};

  resp.key = light->get_object_id_hash();
  resp.state = values.is_on();
  if (traits.get_supports_brightness())
    resp.brightness = values.get_brightness();
  if (traits.get_supports_rgb()) {
    resp.red = values.get_red();
    resp.green = values.get_green();
    resp.blue = values.get_blue();
  }
  if (traits.get_supports_rgb_white_value())
    resp.white = values.get_white();
  if (traits.get_supports_color_temperature())
    resp.color_temperature = values.get_color_temperature();
  if (light->supports_effects())
    resp.effect = light->get_effect_name();
  return this->send_light_state_response(resp);
}
#endif

#ifdef USE_SENSOR
bool APIConnection::send_sensor_state(sensor::Sensor *sensor, float state) {
  if (!this->state_subscription_)
    return false;

  SensorStateResponse resp{};
  resp.key = sensor->get_object_id_hash();
  resp.state = state;
  return this->send_sensor_state_response(resp);
}
#endif

#ifdef USE_SWITCH
bool APIConnection::send_switch_state(switch_::Switch *a_switch, bool state) {
  if (!this->state_subscription_)
    return false;

  SwitchStateResponse resp{};
  resp.key = a_switch->get_object_id_hash();
  resp.state = state;
  return this->send_switch_state_response(resp);
}
#endif

#ifdef USE_TEXT_SENSOR
bool APIConnection::send_text_sensor_state(text_sensor::TextSensor *text_sensor, std::string state) {
  if (!this->state_subscription_)
    return false;

  TextSensorStateResponse resp{};
  resp.key = text_sensor->get_object_id_hash();
  resp.state = std::move(state);
  return this->send_text_sensor_state_response(resp);
}
#endif

#ifdef USE_CLIMATE
bool APIConnection::send_climate_state(climate::Climate *climate) {
  if (!this->state_subscription_)
    return false;

  auto traits = climate->get_traits();
  ClimateStateResponse resp{};
  resp.key = climate->get_object_id_hash();
  resp.mode = static_cast<ClimateMode>(climate->mode);
  if (traits.get_supports_current_temperature())
    resp.current_temperature = climate->current_temperature;
  if (traits.get_supports_two_point_target_temperature()) {
    resp.target_temperature_low = climate->target_temperature_low;
    resp.target_temperature_high = climate->target_temperature_high;
  } else {
    resp.target_temperature = climate->target_temperature;
  }
  if (traits.get_supports_away())
    resp.away = climate->away;
  return this->send_climate_state_response(resp);
}
#endif

bool APIConnection::send_log_message(int level, const char *tag, const char *line) {
  if (this->log_subscription_ < level)
    return false;

  // Send raw so that we don't copy too much
  auto buffer = this->create_buffer();
  // LogLevel level = 1;
  buffer.encode_uint32(1, static_cast<uint32_t>(level));
  // string tag = 2;
  // buffer.encode_string(2, tag, strlen(tag));
  // string message = 3;
  buffer.encode_string(3, line, strlen(line));
  // SubscribeLogsResponse - 29
  bool success = this->send_buffer(buffer, 29);
  if (!success) {
    buffer = this->create_buffer();
    // bool send_failed = 4;
    buffer.encode_bool(4, true);
    return this->send_buffer(buffer, 29);
  } else {
    return true;
  }
}

HelloResponse APIConnection::hello(const HelloRequest &msg) {
  this->client_info_ = msg.client_info + " (" + this->client_->remoteIP().toString().c_str();
  this->client_info_ += ")";
  ESP_LOGV(TAG, "Hello from client: '%s'", this->client_info_.c_str());

  HelloResponse resp;
  resp.api_version_major = 1;
  resp.api_version_minor = 2;
  resp.server_info = App.get_name() + " (esphome v" ESPHOME_VERSION ")";
  this->connection_state_ = ConnectionState::CONNECTED;
  return resp;
}
ConnectResponse APIConnection::connect(const ConnectRequest &msg) {
  bool correct = this->parent_->check_password(msg.password);

  ConnectResponse resp;
  // bool invalid_password = 1;
  resp.invalid_password = !correct;
  if (correct) {
    ESP_LOGD(TAG, "Client '%s' connected successfully!", this->client_info_.c_str());
    this->connection_state_ = ConnectionState::AUTHENTICATED;

#ifdef USE_HOMEASSISTANT_TIME
    if (homeassistant::global_homeassistant_time != nullptr) {
      this->send_time_request();
    }
#endif
  }
  return resp;
}
DeviceInfoResponse APIConnection::device_info(const DeviceInfoRequest &msg) {
  DeviceInfoResponse resp{};
  resp.uses_password = this->parent_->uses_password();
  resp.name = App.get_name();
  resp.mac_address = get_mac_address_pretty();
  resp.esphome_version = ESPHOME_VERSION;
  resp.compilation_time = App.get_compilation_time();
#ifdef ARDUINO_BOARD
  resp.model = ARDUINO_BOARD;
#endif
#ifdef USE_DEEP_SLEEP
  resp.has_deep_sleep = deep_sleep::global_has_deep_sleep;
#endif
  return resp;
}
void APIConnection::on_home_assistant_state_response(const HomeAssistantStateResponse &msg) {
  for (auto &it : this->parent_->get_state_subs())
    if (it.entity_id == msg.entity_id)
      it.callback(msg.state);
}
void APIConnection::execute_service(const ExecuteServiceRequest &msg) {
  bool found = false;
  for (auto *service : this->parent_->get_user_services()) {
    if (service->execute_service(msg)) {
      found = true;
    }
  }
  if (!found) {
    ESP_LOGV(TAG, "Could not find matching service!");
  }
}
void APIConnection::subscribe_home_assistant_states(const SubscribeHomeAssistantStatesRequest &msg) {
  for (auto &it : this->parent_->get_state_subs()) {
    SubscribeHomeAssistantStateResponse resp;
    resp.entity_id = it.entity_id;
    if (!this->send_subscribe_home_assistant_state_response(resp)) {
      this->on_fatal_error();
      return;
    }
  }
}
bool APIConnection::send_buffer(ProtoWriteBuffer buffer, uint32_t message_type) {
  if (this->remove_)
    return false;

  std::vector<uint8_t> header;
  header.push_back(0x00);
  ProtoVarInt(buffer.get_buffer()->size()).encode(header);
  ProtoVarInt(message_type).encode(header);

  size_t needed_space = buffer.get_buffer()->size() + header.size();

  if (needed_space > this->client_->space()) {
    delay(0);
    if (needed_space > this->client_->space()) {
      // SubscribeLogsResponse
      if (message_type != 29) {
        ESP_LOGV(TAG, "Cannot send message because of TCP buffer space");
      }
      delay(0);
      return false;
    }
  }

  this->client_->add(reinterpret_cast<char *>(header.data()), header.size());
  this->client_->add(reinterpret_cast<char *>(buffer.get_buffer()->data()), buffer.get_buffer()->size());
  bool ret = this->client_->send();
  return ret;
}
void APIConnection::on_unauthenticated_access() {
  ESP_LOGD(TAG, "'%s' tried to access without authentication.", this->client_info_.c_str());
  this->on_fatal_error();
}
void APIConnection::on_no_setup_connection() {
  ESP_LOGD(TAG, "'%s' tried to access without full connection.", this->client_info_.c_str());
  this->on_fatal_error();
}
void APIConnection::on_fatal_error() {
  ESP_LOGV(TAG, "Error: Disconnecting %s", this->client_info_.c_str());
  this->client_->close();
  this->remove_ = true;
}

#ifdef USE_HOMEASSISTANT_TIME
void APIConnection::on_get_time_response(const GetTimeResponse &value) {
  if (homeassistant::global_homeassistant_time != nullptr)
    homeassistant::global_homeassistant_time->set_epoch_time(value.epoch_seconds);
}
#endif

#ifdef USE_ESP32_CAMERA
void APIConnection::send_camera_state(std::shared_ptr<esp32_camera::CameraImage> image) {
  if (!this->state_subscription_)
    return;
  if (this->image_reader_.available())
    return;
  this->image_reader_.set_image(image);
}
#endif

}  // namespace api
}  // namespace esphome