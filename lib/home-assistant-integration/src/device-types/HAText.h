#ifndef AHA_HATEXT_H
#define AHA_HATEXT_H

#include "HABaseDeviceType.h"

#ifndef EX_ARDUINOHA_TEXT

#define HATEXT_CALLBACK(name) void (*name)(const char* message, uint16_t length, HAText* sender)

class HAText : public HABaseDeviceType
{
public:
    HAText(const char* uniqueId);

    inline void setIcon(const char* icon)
        { _icon = icon; }

    inline void setRetain(const bool retain)
        { _retain = retain; }

    inline void onMessage(HATEXT_CALLBACK(callback))
        { _messageCallback = callback; }

    bool setState(const char* value, bool force = false);

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
    HATEXT_CALLBACK(_messageCallback);
};

#endif
#endif
