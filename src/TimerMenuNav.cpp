#include "TimerMenuNav.h"

// Display-free TIMER-menu navigation state machine (PRD #83 / issue #85). See the
// header for the input->outcome contract. The implementation is pure logic over
// {focus, index, origin, bounds} -- no device, no display, no storage -- which is
// exactly what makes the interaction model host-testable.

TimerMenuNav::TimerMenuNav()
    : _itemCount(1), _mainIndex(0), _index(0),
      _focus(TimerNavFocus::List), _origin(TimerNavOrigin::Menu) {}

TimerMenuNav::TimerMenuNav(uint8_t itemCount, uint8_t mainIndex, TimerNavOrigin origin)
    : _itemCount(itemCount ? itemCount : 1),
      _mainIndex(mainIndex),
      _index(0),
      _focus(TimerNavFocus::List),
      _origin(origin) {}

void TimerMenuNav::enter(uint8_t itemCount, uint8_t mainIndex, TimerNavOrigin origin)
{
    _itemCount = itemCount ? itemCount : 1;
    _mainIndex = mainIndex;
    _index     = 0;
    _focus     = TimerNavFocus::List;
    _origin    = origin;
}

TimerNavOutcome TimerMenuNav::navigate(int dir)
{
    if (_focus == TimerNavFocus::Editing)
        // The leaf value lives in the data model; the caller applies the step.
        return TimerNavOutcome::AdjustValue;

    // List focus: move the cursor with wrap.
    if (dir > 0)
        _index = (uint8_t)((_index + 1) % _itemCount);
    else
        _index = (uint8_t)((_index == 0) ? _itemCount - 1 : _index - 1);
    return TimerNavOutcome::None;
}

TimerNavOutcome TimerMenuNav::select()
{
    if (_focus == TimerNavFocus::Editing)
    {
        // Short press in a leaf: confirm and step back up to the list.
        _focus = TimerNavFocus::List;
        return TimerNavOutcome::ConfirmBackToList;
    }

    // List focus.
    if (onMain())
        return TimerNavOutcome::GoToMainMenu;   // commit handled by the device

    _focus = TimerNavFocus::Editing;            // drill into the value leaf
    return TimerNavOutcome::EnterLeaf;
}

TimerNavOutcome TimerMenuNav::back()
{
    if (_focus == TimerNavFocus::Editing)
    {
        // Long press in a leaf: save (value already live in RAM) and go up a level.
        _focus = TimerNavFocus::List;
        return TimerNavOutcome::BackToList;
    }

    // List focus: context-aware exit by entry origin.
    return (_origin == TimerNavOrigin::App)
               ? TimerNavOutcome::ExitMenu
               : TimerNavOutcome::GoToMainMenu;
}
