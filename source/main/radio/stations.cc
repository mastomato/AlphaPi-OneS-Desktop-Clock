#include "stations.h"

namespace radio {
namespace {

// Recovered from the 2026-09 "xiaozhi + radio" release image (firmware/xiaozhi.bin),
// preserving the original order exactly. 43 domestic MP3 streams + 4 overseas
// fallbacks, matching the radio the device currently ships with.
const Station kStations[] = {
    {"国际新闻", "新闻 · 64k", "http://lhttp.qtfm.cn/live/20500172/64k.mp3"},
    {"上海新闻广播", "新闻 · 64k", "http://lhttp.qingting.fm/live/270/64k.mp3"},
    {"CNR-1 中国之声", "新闻 · 64k", "http://lhttp.qtfm.cn/live/15318317/64k.mp3"},
    {"广州新闻资讯广播", "新闻 · 64k", "http://lhttp.qingting.fm/live/4848/64k.mp3"},
    {"深圳新闻广播", "新闻 · 64k", "http://lhttp.qingting.fm/live/1270/64k.mp3"},
    {"北京新闻广播", "新闻 · 64k", "http://lhttp.qtfm.cn/live/339/64k.mp3"},
    {"广东新闻广播", "新闻 · 64k", "http://lhttp.qtfm.cn/live/1254/64k.mp3"},
    {"CityFM城市音乐台", "音乐 · 64k", "http://lhttp.qtfm.cn/live/20500153/64k.mp3"},
    {"北京音乐广播", "音乐 · 64k", "http://lhttp.qtfm.cn/live/332/64k.mp3"},
    {"上海经典音乐广播", "音乐 · 64k", "http://lhttp.qingting.fm/live/267/64k.mp3"},
    {"上海流行音乐广播 动感101", "音乐 · 64k", "http://lhttp-hw.qtfm.cn/live/274/64k.mp3"},
    {"上海音乐广播", "音乐 · 64k", "http://lhttp.qingting.fm/live/273/64k.mp3"},
    {"江苏经典流行音乐广播", "音乐 · 64k", "http://lhttp.qtfm.cn/live/4938/64k.mp3"},
    {"云梦音乐台", "音乐 · 64k", "http://lhttp.qtfm.cn/live/20500187/64k.mp3"},
    {"星空电台 STAR RADIO", "音乐 · 64k", "http://lhttp-hw.qtfm.cn/live/5022379/64k.mp3"},
    {"动听音乐台", "音乐 · 64k", "http://lhttp-hw.qtfm.cn/live/5022107/64k.mp3"},
    {"怀集音乐之声", "音乐 · 64k", "http://lhttp.qingting.fm/live/4804/64k.mp3"},
    {"深圳音乐广播", "音乐 · 64k", "http://lhttp.qtfm.cn/live/1271/64k.mp3"},
    {"Fm891线上音乐台", "音乐 · 64k", "http://lhttp.qtfm.cn/live/20500215/64k.mp3"},
    {"两广之声音乐台", "音乐 · 64k", "http://lhttp.qtfm.cn/live/20500149/64k.mp3"},
    {"广东音乐之声", "音乐 · 64k", "http://lhttp.qtfm.cn/live/1260/64k.mp3"},
    {"广州金曲音乐广播", "音乐 · 64k", "http://lhttp.qingting.fm/live/20192/64k.mp3"},
    {"500首华语经典", "音乐 · 64k", "http://lhttp.qtfm.cn/live/5022308/64k.mp3"},
    {"AsiaFM亚洲经典台", "音乐 · 64k", "http://lhttp.qingting.fm/live/5021912/64k.mp3"},
    {"AsiaFM 亚洲粤语台", "音乐 · 64k", "http://lhttp.qtfm.cn/live/15318569/64k.mp3"},
    {"清晨音乐台", "音乐 · 64k", "http://lhttp.qingting.fm/live/4915/64k.mp3"},
    {"Chinese Classical", "民乐 · 128k", "http://radio.chinesemusicworld.com/chinesemusic.mp3"},
    {"顺德音乐之声", "音乐 · 64k", "http://lhttp.qtfm.cn/live/20500150/64k.mp3"},
    {"内蒙古交通之声", "交通 · 64k", "http://lhttp-hw.qtfm.cn/live/1884/64k.mp3"},
    {"CRI劲曲调频 HIT FM", "音乐 · 64k", "http://lhttp.qingting.fm/live/15318703/64k.mp3"},
    {"第一财经", "财经 · 64k", "http://lhttp.qingting.fm/live/276/64k.mp3"},
    {"广东珠江经济台", "财经 · 64k", "http://lhttp.qtfm.cn/live/1259/64k.mp3"},
    {"羊城交通电台", "交通 · 64k", "http://lhttp.qtfm.cn/live/1262/64k.mp3"},
    {"广东股市广播", "财经 · 64k", "http://lhttp.qtfm.cn/live/4847/64k.mp3"},
    {"吉林市广播电视台经济广播", "财经 · 64k", "http://lhttp.qingting.fm/live/1823/64k.mp3"},
    {"茂名交通广播", "交通 · 64k", "http://lhttp.qingting.fm/live/20211574/64k.mp3"},
    {"北京文艺广播", "文艺 · 64k", "http://lhttp.qtfm.cn/live/333/64k.mp3"},
    {"故事广播-阿基米德故事会", "故事 · 64k", "http://lhttp.qtfm.cn/live/20500182/64k.mp3"},
    {"安徽小说评书广播", "故事 · 64k", "http://lhttp.qtfm.cn/live/1951/64k.mp3"},
    {"商丘综合广播", "综合 · 64k", "http://lhttp-hw.qtfm.cn/live/5022443/64k.mp3"},
    {"济南故事广播", "故事 · 64k", "http://lhttp.qtfm.cn/live/1672/64k.mp3"},
    {"广汉人民广播电台", "综合 · 64k", "http://lhttp.qingting.fm/live/20212405/64k.mp3"},
    {"德阳综合广播", "综合 · 64k", "http://lhttp-hw.qtfm.cn/live/4987/64k.mp3"},
    {"DEF CON Radio", "备用 · 128k", "http://ice1.somafm.com/defcon-128-mp3"},
    {"Drone Zone", "备用 · 128k", "http://ice1.somafm.com/dronezone-128-mp3"},
    {"Groove Salad", "备用 · 128k", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"Secret Agent", "备用 · 128k", "http://ice1.somafm.com/secretagent-128-mp3"},
};

}  // namespace

const Station* BuiltinStations() { return kStations; }
size_t BuiltinStationCount() { return sizeof(kStations) / sizeof(kStations[0]); }

}  // namespace radio
