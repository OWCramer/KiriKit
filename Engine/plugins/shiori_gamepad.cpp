/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// gamepad.dll: the `GamepadPort` and `Gamepad` classes and `gp*` constants some KiriKiri games
// (Yuzusoft's exgamepad.tjs) read controllers through on Windows. On the phone a controller
// already reaches the game as the keys KAG maps a pad to (the host's input), so the port
// reports no controllers of its own: the game's keyboard path drives the menus, as it does on a
// PC with no XInput pad. The names are the DLL's exported ones (its TJS constant table and
// member names); no other plug-in's code was read.
#include "tjsCommHead.h"
#include "ScriptMgnIntf.h"
#include <ncbind.hpp>

namespace {

const tjs_char *kGamepadScript = TJS_W(
    "const gpDInput = 3, gpFFDInput = 2, gpXInput = 1,"
    " gpButtonDpadUp = 0x00000001, gpButtonDpadDown = 0x00000002, gpButtonDpadLeft = 0x00000004,"
    " gpButtonDpadRight = 0x00000008, gpButtonStart = 0x00000010, gpButtonBack = 0x00000020,"
    " gpButtonLeftThumb = 0x00000040, gpButtonRightThumb = 0x00000080, gpButtonLeftShoulder = 0x00000100,"
    " gpButtonRightShoulder = 0x00000200, gpButtonA = 0x00001000, gpButtonB = 0x00002000,"
    " gpButtonX = 0x00004000, gpButtonY = 0x00008000, gpLeftAxisX = 0x00010000, gpLeftAxisY = 0x00020000,"
    " gpRightAxisX = 0x00040000, gpRightAxisY = 0x00080000, gpLeftTrigger = 0x00100000, gpRightTrigger = 0x00200000,"
    " gpDIAxisX = 0, gpDIAxisY = 1, gpDIAxisZ = 2, gpDIAxisRotX = 3, gpDIAxisRotY = 4, gpDIAxisRotZ = 5,"
    " gpDISlider_0 = 6, gpDISlider_1 = 7, gpDIPOV_0 = 8, gpDIPOV_1 = 9, gpDIPOV_2 = 10, gpDIPOV_3 = 11,"
    " gpDIButton1 = 12, gpDIButton2 = 13, gpDIButton3 = 14, gpDIButton4 = 15, gpDIButton5 = 16,"
    " gpDIButton6 = 17, gpDIButton7 = 18, gpDIButton8 = 19, gpDIButton9 = 20, gpDIButton10 = 21,"
    " gpDIButton11 = 22, gpDIButton12 = 23, gpDIButton13 = 24, gpDIButton14 = 25, gpDIButton15 = 26,"
    " gpDIButton16 = 27, gpDIButton17 = 28, gpDIButton18 = 29, gpDIButton19 = 30, gpDIButton20 = 31,"
    " gpDIButton21 = 32, gpDIButton22 = 33, gpDIButton23 = 34, gpDIButton24 = 35, gpDIButton25 = 36,"
    " gpDIButton26 = 37, gpDIButton27 = 38, gpDIButton28 = 39, gpDIButton29 = 40, gpDIButton30 = 41,"
    " gpDIButton31 = 42, gpDIButton32 = 43, gpDIDisable = 44,"
    " gpTouchNo = 0, gpTouchDown = 1, gpTouchLiftoff = 0;\n"
    // A controller's state, all at rest (none is ever handed out; here for scripts that test
    // `instanceof` or make one themselves).
    "class Gamepad {\n"
    " var name = '', type = 0, leftTrigger = 0, rightTrigger = 0, leftThumbStickX = 0, leftThumbStickY = 0,"
    " rightThumbStickX = 0, rightThumbStickY = 0, leftVibration = 0, rightVibration = 0, keyState = 0,"
    " analogLeftUpCount = 0, analogLeftDownCount = 0, analogLeftLeftCount = 0, analogLeftRightCount = 0,"
    " analogRightUpCount = 0, analogRightDownCount = 0, analogRightLeftCount = 0, analogRightRightCount = 0,"
    " degitalUpCount = 0, degitalDownCount = 0, degitalLeftCount = 0, degitalRightCount = 0,"
    " buttonStartCount = 0, buttonBackCount = 0, buttonLeftThumbCount = 0, buttonRightThumbCount = 0,"
    " buttonLeftShoulderCount = 0, buttonLeftTriggerCount = 0, buttonRightShoulderCount = 0,"
    " buttonRightTriggerCount = 0, buttonACount = 0, buttonBCount = 0, buttonXCount = 0, buttonYCount = 0;\n"
    " function Gamepad() {}\n"
    " function finalize() {}\n"
    " function update() {}\n"
    "}\n"
    // The port, usable as a class or an instance: no controllers.
    "class GamepadPort {\n"
    " function GamepadPort() {}\n"
    " function finalize() {}\n"
    " function initialize() { return true; }\n"
    " function update() {}\n"
    " function getController(index) { return void; }\n"
    " property count { getter() { return 0; } }\n"
    "}\n");

void linkGamepad() { TVPExecuteScript(ttstr(kGamepadScript)); }

} // namespace

#define NCB_MODULE_NAME TJS_W("gamepad.dll")
NCB_POST_REGIST_CALLBACK(linkGamepad);
