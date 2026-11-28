#pragma once

namespace radio {

/*
 * 收音机的全屏页面。
 *
 * 版式与待机时钟页保持一致：顶部留 22 px 给状态栏（电量/网络图标不能被盖住），
 * 其余整块用作电台卡片。
 *
 * 文字只用 ASCII：本固件内置的 noto_sans_basic 字库**不含任何汉字**（逐字验证过），
 * 中文要靠服务器下发字形（text_glyph）才能显示，本地 UI 用不了。所以电台名取其中的
 * ASCII 片段（如 "CNR-1"）作为标识，没有则显示序号。
 */

// 预留接口：页面改为「长按 D 时按需创建」后，这里已不做任何事。
// 保留函数只是为了让板卡构造函数的调用点不用改，也便于以后加预热逻辑。
void RadioPageStart();

// 显示/隐藏。隐藏后待机时钟可以正常接管屏幕。
void RadioPageShow(bool show);

bool RadioPageVisible();

// 由控制器按固定节拍调用，刷新文本内容。
void RadioPageRefresh();

/*
 * 覆盖状态行文字（例如 "INIT FAIL"）。这块板的串口日志读不出来，失败原因只能
 * 直接写在屏上，否则用户看到的就是"按了没反应"。传 nullptr 恢复正常显示。
 */
void RadioPageSetStatus(const char* text);

}  // namespace radio
