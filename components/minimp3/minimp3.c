// components/minimp3/minimp3.c
// 单一实现单元：minimp3 是 header-only 解码器，这里只在一个 .c 中定义实现。
// minimp3 由 lieff 维护，授权为 CC0-1.0（见 minimp3.h 文件头）。
// 选它替换 libhelix 的原因：libhelix 无法解码 LiveATC 部分 8 kHz VBR
// MPEG-2.5 流（实测 200KB 样本中 448 次 INVALID_FRAMEHEADER，几乎全部帧失败），
// minimp3 对同一份样本 1621 帧全部解码成功。
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
