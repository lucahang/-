#pragma once

namespace Config {
    // 画面设置
    const int TARGET_FPS = 60;           // 目标帧率
    const int WAIT_TIMEOUT_MS = 1000 / TARGET_FPS; // 采集等待时间 (约 33ms)

    // 网络设置
    inline int SERVER_PORT = 8808;        // 被控端监听的端口
}
