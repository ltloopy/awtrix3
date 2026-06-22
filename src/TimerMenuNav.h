#ifndef TimerMenuNav_h
#define TimerMenuNav_h

#include <Arduino.h>

// Display-free navigation state machine for the TIMER global menu (PRD #83 /
// issue #85). It owns the whole interaction model behind a small interface:
// list/leaf focus, the selected index, and the entry origin. Button inputs map
// to OUTCOMES that the device layer (MenuManager) acts on; the device keeps only
// drawing (the list indicator / the leaf value) and the single commit. Because
// it depends on nothing but Arduino.h, the interaction model is host-testable
// under test_timer without a device. See CONTEXT.md and docs/adr/0016.
//
// The list is the TIMER menu's slot rows. Value rows (enum / number / bool) drill
// into a leaf editor; the lone navigation row (MAIN) returns to the main menu.
// MAIN's index is supplied by the device from the slot table, so the state
// machine never hard-codes which row is MAIN.

enum class TimerNavFocus  : uint8_t { List, Editing };
enum class TimerNavOrigin : uint8_t { Menu, App };

// Outcomes the device layer acts on. Anything not listed (plain list movement) is
// reported as None; the caller reads focus()/index() for the new cursor position.
enum class TimerNavOutcome : uint8_t
{
    None,              // handled internally (list cursor moved)
    AdjustValue,       // Editing focus: caller applies timerMenuAdjust(index(), dir)
    EnterLeaf,         // List focus, value row selected: drilled into its leaf
    ConfirmBackToList, // Editing focus, short press: confirm, back to the list
    BackToList,        // Editing focus, long press: save, back to the list
    GoToMainMenu,      // commit + return to the main menu
    ExitMenu,          // commit + return to the Timer app (origin == App)
};

class TimerMenuNav
{
public:
    // Default: an empty single-item list in List focus. Use enter() before driving.
    TimerMenuNav();
    // itemCount = number of list rows; mainIndex = the MAIN (navigation) row.
    TimerMenuNav(uint8_t itemCount, uint8_t mainIndex,
                 TimerNavOrigin origin = TimerNavOrigin::Menu);

    // (Re)enter the list: reset to List focus, index 0, with fresh bounds + origin.
    // Called by the device whenever the TIMER menu is opened.
    void enter(uint8_t itemCount, uint8_t mainIndex, TimerNavOrigin origin);

    TimerNavFocus  focus() const { return _focus; }
    uint8_t        index() const { return _index; }
    TimerNavOrigin origin() const { return _origin; }
    bool           onMain() const { return _index == _mainIndex; }

    // left/right. List focus: move the cursor with wrap (-> None). Editing focus:
    // -> AdjustValue (the caller mutates the value via timerMenuAdjust).
    TimerNavOutcome navigate(int dir);

    // short press (middle button).
    //   List focus, value row -> EnterLeaf (focus becomes Editing).
    //   List focus, MAIN      -> GoToMainMenu.
    //   Editing focus         -> ConfirmBackToList (focus becomes List).
    TimerNavOutcome select();

    // long press (middle button, held).
    //   List focus    -> context-aware exit: GoToMainMenu (origin Menu) or
    //                    ExitMenu (origin App).
    //   Editing focus -> BackToList (focus becomes List; value already live in RAM).
    TimerNavOutcome back();

private:
    uint8_t        _itemCount;
    uint8_t        _mainIndex;
    uint8_t        _index;
    TimerNavFocus  _focus;
    TimerNavOrigin _origin;
};

#endif
