# 2026-11-06 位图字幕接管链路改动归档

本目录归档 2026-11-06 一整轮排查/修复的**文档与补丁**。源码分属多个仓库，这里不放 App 源码（App 属内网工程 `publicversion`）。

## 仓库分工

| 仓库 | 本地路径 | 本次相关内容 |
|---|---|---|
| `xingkongyu/vlc_3.6.5_new` | `D:\vlc-rescue-20261105\work\vlc_core-3.6.5` | `vout_subpictures.c`（调色板改色回退为 alpha 遮罩 + 注册/空轨补发 CLEAR + **同句去重/不缩放**）、`display.c`（**按 buffer 脏区清除**）、`aar/` 稳定版、本 `docs/` 归档 |
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
| 6 | 加固：try-catch 全覆盖、缩放/尺寸/坐标夹紧、OOM 降级、对抗性检查（core/JNI 仅核查未改） | publicversion | APK `261106_112031` |
| 7 | **字幕出现时卡顿**：恢复 11-02 基线在 11-05 重装时被回退的性能路径（core 同句去重 / `scale=unit` 不放大 / 坐标基准；`display.c` 按 buffer 脏区清除替代 33MB 整屏 memset） | core | AAR `20261106_140824`（**当前稳定版**），用户真机确认卡顿已解决 |
| 8 | 提交前加固：对抗性检查（20 万组矩形 + 3000 帧状态机 + 10 万组换算 + 22 条源码断言）+ App 回调链路 5 层 try-catch、主线程 handler 快照判空 | publicversion（AAR 不变） | APK 由用户重编 |

`change_notes/2026-11-06_App位图字幕字号缩放修复.md` 在 10:27 补齐过一次（原文件曾被写成 0 字节）。

## 稳定版产物

- `aar/libvlc-release-20261106_095230.aar`（25,035,290 B）：JNI 签名修复后重编，`libvlcjni.so` 含正确签名 `([[B[IIIJJ)V`。
- `aar/libvlc-release-20261106_105410.aar`（25,035,064 B）：在 095230 基础上补 core 的 CLEAR；`libvlc.so` 含 `notify external before listener change`。
- `aar/libvlc-release-20261106_140824.aar`（25,036,544 B）：**当前稳定版**，恢复基线性能修复（同句去重 / 不缩放 / 脏区清除）；`libvlc.so` 含 `CLEAR rect=[...]`、不含 `CLEAR full rect=[...]`，并保留 105410 的 CLEAR 补发。MD5 `7658DF4C9C187B36620D1FC4DF578E74`。App `vlc/libs/` 已替换为该版本。
- APK 均为 `d14_wifi_rtc_zh` release；#1~#6 在设备 `1cf675004a230e19` 实测，#7 的重编 AAR 在设备 `42f6b29cec0fe663` 由用户实机验证。
- #8 只改 App Java（`VlcVideo.java`），**AAR 不变**，APK 由用户重编验证。

## 验证结论（摘要）

- 位图接管链路打通：`bitmap listener registered (takeover)`、App 日志 `bitmap cb / bitmap draw`，无 `VLCSubFix REDRAW`。
- 字号三档 `user=0.6667 / 1.6667 / 2.5`，绘制尺寸同比变化，动态切档下一帧生效。
- 黑边：亮画面下绿色字幕带黑色描边；字色设置仍生效（红/白切换）。
- 残留：切轨后旧句消失，无字幕间隙与切集起播屏幕干净。
- 部分字幕轨选中后长时间无回调，是 `Signs-&-Songs` 类轨本身内容稀疏（core 无 `spu subpic -> vout` 产出），选轨与回调链路均正常，非缺陷。
- 加固轮次：lint 0、编译通过；真机连续切字号/切轨/切集后进程存活、无崩溃、无任何 `FAILED` 兜底日志，`bitmap draw` 110 次正常。详见 `change_notes/2026-11-06_加固trycatch与对抗性检查.md`。
- #7：**用户真机确认「字幕出现时卡顿」已解决**；构建机 native + AAR 编译无 `error:` / `undefined reference`；AAR 内 `libvlc.so` 字符串指纹核对通过（`CLEAR full rect` 消失、`CLEAR rect` 出现）。本地 / SMB / 构建机三处源码 MD5 一致。
- #8：对抗性检查 A/B/C/D 四组共 **3,200,221 断言、0 FAIL**（`verify/adv_subperf.py`）；IDE lint 0 诊断；core/JNI 三文件 MD5 未变，AAR 无需重编。回归脚本内为本机绝对路径，换机需按实际路径调整。

## 文件清单

- `change_notes/`：7 份改动文档（原因、定位、改前后、备份、编译验证、回退）
- `diffs/`：10 份可审计补丁（`*_增量.diff` 为对应轮次 before → after）
- `verify/`：1 份对抗性回归脚本 `adv_subperf.py`

## 回退

**不要**用 `git checkout --`，也不要对整个全量 diff 做 `git apply -R`：该 worktree 长期存在跨提交的未提交改动，会连带删掉仍被依赖的实现。

回退请用 `D:\vlc-rescue-20261105\Backups\<时间戳>_*\before\` 中的源码/AAR 快照覆盖；`diffs/2026-11-06_*_增量.diff` 仅作反向审计凭据：

- #7 core 两文件 → `2026-11-06_14-02-02_字幕出现卡顿回退修复\before\`；App 旧 AAR → 同目录 `before_app_aar\`。
- #8 App Java → `2026-11-06_14-29-16_对抗性检查与trycatch加固\before\`。
- core 侧也可用构建机备份 `/home/work/data1/vlc_core-3.6.5/src/video_output/vout_subpictures.c.20261106_coreclear.bak` 回退后重编 AAR。
