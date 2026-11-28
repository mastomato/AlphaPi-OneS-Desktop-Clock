#pragma once

#include <cstddef>

namespace radio {

/*
 * 网络收音机的总控：负责在「小智」与「收音机」之间交接播放通道。
 *
 * 交接的关键点（都是这块板的硬件特性决定的）：
 *   - 音频链路是半双工的，播放前必须让 N32 停掉麦克风，否则播放静音；
 *   - 收音机播放时不能同时跑小智的一轮对话，所以进入前把应用压到待机态；
 *   - 待机时钟页与电台页抢同一块屏，进收音机时要把时钟让出去。
 */
class RadioController {
public:
    static RadioController& Instance();

    // 板子构造函数里调用一次：初始化播放器与电台页（不进入收音机）。
    void Init();

    bool active() const { return active_; }

    // 长按 D：进入 / 退出收音机
    void Toggle();

    // 收音机模式下的按键动作
    void PlayPause();
    void Step(int delta);   // +1 下一台，-1 上一台
    void ToggleMute();

private:
    RadioController() = default;
    void Enter();
    void Exit();

    bool active_ = false;
    bool muted_ = false;
    int volume_before_mute_ = 70;
};

}  // namespace radio
