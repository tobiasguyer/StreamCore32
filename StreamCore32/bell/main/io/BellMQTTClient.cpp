#include "BellMQTTClient.h"

#ifndef _WIN32
#include <sys/fcntl.h>  // for fcntl, F_GETFL, F_SETFL, O_NONBLOCK
#endif
#include <stddef.h>   // for NULL
#include <stdexcept>  // for runtime_error

#include "BellLogger.h"  // for AbstractLogger, BELL_LOG
#include "BellSocket.h"  // for bell
#include "TCPSocket.h"   // for TCPSocket

using namespace bell;

void cPublishCallback(void** unused, struct mqtt_response_publish* published) {
  // Map to class instance
  MQTTClient* client = (MQTTClient*)*unused;
  client->publishCallback(published);
}

void bell::MQTTClient::connect(const std::string& host, uint16_t port,
                               const std::string& username,
                               const std::string& password) {
  socket.open(host, port);

  // Verify that the socket is open
  if (!socket.isOpen()) {
    throw std::runtime_error("Could not connect to MQTT broker");
  }

  // Set the socket to non-blocking
#ifdef _WIN32
  u_long iMode = 1;
  ioctlsocket(socket.getFd(), FIONBIO, &iMode);
#else
  int status = fcntl(socket.getFd(), F_SETFL,
                     fcntl(socket.getFd(), F_GETFL, 0) | O_NONBLOCK);
#endif

  // Pass pointer to this object to the publish callback
  client.publish_response_callback_state = this;

  if (mqtt_init(&client, socket.getFd(), sendbuf, sizeof(sendbuf), recvbuf,
                sizeof(recvbuf), cPublishCallback) != MQTT_OK) {
    throw std::runtime_error("Cannot initialize MQTT structure");
  }

  const char* clientId = NULL;
  uint8_t connect_flags = MQTT_CONNECT_CLEAN_SESSION;
  if (mqtt_connect(&client, clientId, NULL, NULL, 0, username.c_str(),
                   password.c_str(), connect_flags, 400) != MQTT_OK) {
    throw std::runtime_error("MQTT connect failed");
  }

  // Mark as connected
  connected = true;
}

void MQTTClient::sync() {
  if (!connected) {
    throw std::runtime_error("MQTT client is not connected");
  }

  // Poll the socket
  // Process the data
  mqtt_sync(&client);
}

void MQTTClient::publish(const std::string& topic, const std::string& message,
                         QOS qos) {
  if (!connected) {
    throw std::runtime_error("MQTT client is not connected");
  }

  int err = mqtt_publish(&client, topic.c_str(), message.c_str(),
                         message.size(), (uint8_t)qos);

  if (err != MQTT_OK) {
    BELL_LOG(error, "mqtt", "MQTT publish failed: %d", err);
    throw std::runtime_error("MQTT publish failed");
  }
}

void MQTTClient::subscribe(const std::string& topic, QOS qos) {
  if (!connected) {
    throw std::runtime_error("MQTT client is not connected");
  }

  mqtt_subscribe(&client, topic.c_str(), (uint8_t)qos);
}

void MQTTClient::unsubscribe(const std::string& topic) {
  if (!connected) {
    throw std::runtime_error("MQTT client is not connected");
  }

  mqtt_unsubscribe(&client, topic.c_str());
}

void MQTTClient::disconnect() {
  if (!connected) {
    throw std::runtime_error("MQTT client is not connected");
  }

  mqtt_disconnect(&client);
  socket.close();
  connected = false;
}

bool MQTTClient::isConnected() {
  return connected;
}

void MQTTClient::publishCallback(struct mqtt_response_publish* published) {
  if (_publishCallback) {
    // Prepare the topic and message
    std::string topic((const char*)published->topic_name,
                      published->topic_name_size);
    std::string message((const char*)published->application_message,
                        published->application_message_size);
    _publishCallback(topic, message);
  }
}