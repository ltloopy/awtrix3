#ifndef AHA_HANOTIFY_H
#define AHA_HANOTIFY_H

#include "HABaseDeviceType.h"

#ifndef EX_ARDUINOHA_NOTIFY

#define HANOTIFY_CALLBACK(name) void (*name)(const char* message, uint16_t length, HANotify* sender)

class HANotify : public HABaseDeviceType
{
public:
    HANotify(const char* uniqueId);

    inline void setIcon(const char* icon)
        { _icon = icon; }

    inline void setRetain(const bool retain)
        { _retain = retain; }

    inline void onMessage(HANOTIFY_CALLBACK(callback))
        { _messageCallback = callback; }

protected:
    virtual void buildSerializer() override;
    virtual void onMqttConnected() override;
    virtual void onMqttMessage(
        const char* topic,
        const uint8_t* payload,
        const uint16_t length
    ) override;

private:
    const char* _icon;
    bool _retain;
    HANOTIFY_CALLBACK(_messageCallback);
};

#endif
#endif
