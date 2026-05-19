// Minimal MenuManager stub. The production MenuManager pulls in
// DisplayManager + EasyButton + a lot of UI state we don't need on host.
// TimerManager only reads `MenuManager.inMenu`, so that's all we expose.
#ifndef MenuManager_h
#define MenuManager_h

class MenuManager_
{
public:
    bool inMenu = false;
};

extern MenuManager_ MenuManager;

#endif
