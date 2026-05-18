#include "InputController.h"

// 判断是否为扩展键（需要加 KEYEVENTF_EXTENDEDKEY 标志）
static bool IsExtendedKey(WORD vk) {
    switch (vk) {
        // 方向键
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        // 编辑键
    case VK_INSERT:   case VK_DELETE:
    case VK_HOME:     case VK_END:
    case VK_PRIOR:    case VK_NEXT:   // PageUp / PageDown
        // 右侧修饰键
    case VK_RCONTROL: case VK_RMENU:
        // 小键盘 Enter 和 /
    case VK_DIVIDE:
        // Win 键
    case VK_LWIN:     case VK_RWIN:
        // 截图键、Pause
    case VK_SNAPSHOT: case VK_CANCEL:
    case VK_NUMLOCK:
        return true;
    default:
        return false;
    }
}

void InputController::MoveMouseTo(int x, int y, int screenWidth, int screenHeight) {
    if (screenWidth <= 0 || screenHeight <= 0) return;

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = (x * 65535) / screenWidth;
    input.mi.dy = (y * 65535) / screenHeight;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &input, sizeof(INPUT));
}

void InputController::LeftClickDown() {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

void InputController::LeftClickUp() {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void InputController::RightClickDown() {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

void InputController::RightClickUp() {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void InputController::MiddleClickDown() {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

void InputController::MiddleClickUp() {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
    SendInput(1, &input, sizeof(INPUT));
}

void InputController::ScrollWheel(int delta) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = (DWORD)delta;   // 正数向上，负数向下
    SendInput(1, &input, sizeof(INPUT));
}

void InputController::PressKey(WORD vKey, bool state) {
    if (vKey == 0) return;

    INPUT input = {};
    input.type = INPUT_KEYBOARD;

    UINT scanCode = MapVirtualKey(vKey, MAPVK_VK_TO_VSC);
    input.ki.wVk = vKey;
    input.ki.wScan = (WORD)scanCode;
    input.ki.dwExtraInfo = GetMessageExtraInfo();

    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (!state)
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    if (IsExtendedKey(vKey))
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;

    SendInput(1, &input, sizeof(INPUT));
}