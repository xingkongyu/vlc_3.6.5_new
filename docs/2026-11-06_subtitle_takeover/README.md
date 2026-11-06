# 2026-11-06 位图字幕接管链路改动归档

本目录归档 2026-11-06 一整轮排查/修复的**文档与补丁**。源码分属多个仓库，这里不放 App 源码（App 属内网工程 `publicversion`）。

## 仓库分工

| 仓库 | 本地路径 | 本次相关内容 |
|---|---|---|
| `xingkongyu/vlc_3.6.5_new` | `D:\vlc-rescue-20261105\work\vlc_core-3.6.5` | `src/video_output/vout_subpictures.c`（调色板改色回退为 alpha 遮罩 + 注册/空轨补发 CLEAR）、`aar/` 新增两个稳定版、本 `docs/` 归档 |
| `xingkongyu/vlc_3.6.5_jni` | `D:\vlc-rescue-20261105\vlc-aar-git` | `libvlcjni/libvlc/jni/libvlcjni-mediaplayer.c` 的 `onSubtitleBitmaps` 方法描述符 |
| 内网 `publicversion`（**未推送**） | `D:\workspace\A14\a14_2\publicversion` | App 侧 Java/XML 改动，仅以 diff 形式归档在此 |

## 改动清单

| # | 主题 | 落点 | 产物 |
|---|---|---|---|
| 1 | JNI 位图回调签名修复：`([[B[IIIIJJ)V` → `([[B[IIIJJ)V` | jni 仓库 | AAR `20261106_095230` |
| 2 | App 位图字幕字号缩放：`applyBitmapArray()` 应用 `mSubtitleBitmapScale`，`getSubtitleSize()` 档位映射统一 | publicversion | APK `261106_101021` |
| 3 | 字幕文字边框统一黑色：core 回传原色 + App 按亮度分离暗/亮像素；文本字幕补黑色 `shadow*` | publicversion | APK `261106_103431` |
| 4 | 字幕残留修复 A（代际/清屏）+ core 根治（空轨与反注册补发 CLEAR） | core + publicversion | AAR `20261106_105410`、APK `261106_105506` |
| 5 | 残留修复第二轮：切轨时同时清文本 TextView、空文本回调无条件隐藏 | publicversion | APK `261106_110237` |
| 6 | 加固：try-catch 全覆盖、缩放/尺寸/坐标夹紧、OOM 降级、对抗性检查（core/JNI 仅核查未改） | publicversion | APK `261106_112031`（**当前设备版本**） |

`change_notes/2026-11-06_App位图字幕字号缩放修复.md` 在 10:27 补齐过一次（原文件曾被写成 0 字节）。

## 稳定版产物

- `aar/libvlc-release-20261106_095230.aar`（25,035,290 B）：JNI 签名修复后重编，`libvlcjni.so` 含正确签名 `([[B[IIIJJ)V`。
- `aar/libvlc-release-20261106_105410.aar`（25,035,064 B）：**当前稳定版**，在 095230 基础上补 core 的 CLEAR；`libvlc.so` 含 `notify external before listener change`。App `vlc/libs/` 已替换为该版本。
- APK 均为 `d14_wifi_rtc_zh` release，设备 `1cf675004a230e19` 实测。

## 验证结论（摘要）

- 位图接管链路打通：`bitmap listener registered (takeover)`、App 日志 `bitmap cb / bitmap draw`，无 `VLCSubFix REDRAW`。
- 字号三档 `user=0.6667 / 1.6667 / 2.5`，绘制尺寸同比变化，动态切档下一帧生效。
- 黑边：亮画面下绿色字幕带黑色描边；字色设置仍生效（红/白切换）。
- 残留：切轨后旧句消失，无字幕间隙与切集起播屏幕干净。
- 部分字幕轨选中后长时间无回调，是 `Signs-&-Songs` 类轨本身内容稀疏（core 无 `spu subpic -> vout` 产出），选轨与回调链路均正常，非缺陷。
- 加固轮次：lint 0、编译通过；真机连续切字号/切轨/切集后进程存活、无崩溃、无任何 `FAILED` 兜底日志，`bitmap draw` 110 次正常。详见 `change_notes/2026-11-06_加固trycatch与对抗性检查.md`。

## 文件清单

- `change_notes/`：5 份改动文档（原因、定位、改前后、备份、编译验证、回退）
- `diffs/`：8 份可审计补丁（`*_增量.diff` 为对应轮次 before → after）

## 回退

**不要**用 `git checkout --`，也不要对整个全量 diff 做 `git apply -R`：该 worktree 长期存在跨提交的未提交改动，会连带删掉仍被依赖的实现。

回退请用 `D:\vlc-rescue-20261105\Backups\<时间戳>_*\before\` 中的源码/AAR 快照覆盖；`diffs/2026-11-06_*_增量.diff` 仅作反向审计凭据。core 侧也可用构建机备份 `/home/work/data1/vlc_core-3.6.5/src/video_output/vout_subpictures.c.20261106_coreclear.bak` 回退后重编 AAR。
