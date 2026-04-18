#include "HANotify.h"
#ifndef EX_ARDUINOHA_NOTIFY

#include "../HAMqtt.h"
#include "../utils/HASerializer.h"

HANotify::HANotify(const char* uniqueId) :
    HABaseDeviceType(AHATOFSTR(HAComponentNotify), uniqueId),
    _icon(nullptr),
    _retain(false),
    _messageCallback(nullptr)
{

}

void HANotify::buildSerializer()
{
    if (_serializer || !uniqueId()) {
        return;
    }

    _serializer = new HASerializer(this, 8);
    _serializer->set(AHATOFSTR(HANameProperty), _name);
    _serializer->set(AHATOFSTR(HAUniqueIdProperty), _uniqueId);
    _serializer->set(AHATOFSTR(HAIconProperty), _icon);

    if (_retain) {
        _serializer->set(
            AHATOFSTR(HARetainProperty),
            &_retain,
            HASerializer::BoolPropertyType
        );
    }

    _serializer->set(HASerializer::WithDevice);
    _serializer->set(HASerializer::WithAvailability);
    _serializer->topic(AHATOFSTR(HACommandTopic));
}

void HANotify::onMqttConnected()
{
    if (!uniqueId()) {
        return;
    }

    publishConfig();
    publishAvailability();
    subscribeTopic(uniqueId(), AHATOFSTR(HACommandTopic));
}

void HANotify::onMqttMessage(
    const char* topic,
    const uint8_t* payload,
    const uint16_t length
)
{
    if (_messageCallback && HASerializer::compareDataTopics(
        topic,
        uniqueId(),
        AHATOFSTR(HACommandTopic)
    )) {
        char* message = new char[length + 1];
        if (message) {
            memcpy(message, payload, length);
            message[length] = '\0';
            _messageCallback(message, length, this);
            delete[] message;
        }
    }
}

#endif
