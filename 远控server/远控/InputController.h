#pragma once
#include <windows.h>

class InputController {
public:
    InputController() = default;
    ~InputController() = default;

    // 鼠标
    void MoveMouseTo(int x, int y, int screenWidth, int screenHeight);
    void LeftClickDown();
    void LeftClickUp();
    void RightClickDown();
    void RightClickUp();
    void MiddleClickDown();
    void MiddleClickUp();
    void ScrollWheel(int delta);   // delta: +120=向上一格, -120=向下一格

    // 键盘: state=true 按下, state=false 抬起
    void PressKey(WORD vKey, bool state);
};