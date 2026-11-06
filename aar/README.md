# AAR 产物目录

存放每个**已验证稳定**的 libvlc AAR，便于回滚与对照。文件名格式：

```
libvlc-release-<构建时间戳>.aar           # 本次构建产物
libvlc-release-<时间戳>_baseline.aar      # 对照基线（重装前的历史产物）
```

## 清单

| 文件 | 大小 | 说明 |
|---|---|---|
| `libvlc-release-20261102_112606_baseline.aar` | 23.7 MB | **基线**：系统重装前使用的版本，作为行为对照基准 |
| `libvlc-release-20261105_182029.aar` | 23.8 MB | 重建并补齐定制后的版本，见下 |
| `libvlc-release-20261106_095230.aar` | 23.9 MB | JNI 位图回调方法描述符修复后重编（`([[B[IIIJJ)V`） |
| `libvlc-release-20261106_105410.aar` | 23.9 MB | **当前稳定版**：在 095230 基础上补 core 空轨/反注册 CLEAR |

## 20261106 版本说明

- `095230`：仅修 `libvlcjni.so` 的 `onSubtitleBitmaps` JNI 签名（原签名多一个 `I`，`GetMethodID` 返回 NULL、异常被 `ExceptionClear` 吞掉，Java 层假成功而 core `pf_bitmap` 始终为空）。
- `105410`：core `vout_subpictures.c` 把调色板改色回退为 bridge 内 alpha 遮罩改色，并在 `vlc_spu_set_bitmap_listener()` 注册/反注册前、`SpuRender()` 空队列提前返回前各补发一次 CLEAR；`libvlc.so` 含 `notify external before listener change`。
- 两者均只编 `armeabi-v7a`；App `vlc/libs/` 当前使用 `105410`。
- 完整改动文档与补丁见 `docs/2026-11-06_subtitle_takeover/`。

## 20261105_182029 版本说明

源码 commit 见仓库 `main` 分支同期提交。相对基线差异（逐层核对结果）：

| 项 | 结果 |
|---|---|
| 文件清单 | 49 vs 49，一致 |
| `libvlcjni.so` JNI 符号 | 124 vs 124，丢失 0 |
| `libvlc.so` 对外 C API | 丢失 0 |
| 模块 | 少 `stream_out_chromecast`（`extras/tools` 的 protoc 3.1.0 与 contrib 的 protobuf 3.4.0 不兼容） |
| assets | 差异 0 |
| 定制字符串（`VLCSubFix`/`VLCSpuTrace`/`bitmap clear` 等） | 全部存在 |
| 关键函数规模 | `vlc_spu_set_text_listener` 80/80、`vlc_spu_set_bitmap_listener` 184/180、`libvlc_video_set_sub_scale` 52/52 |

已知实现差异（功能等价，属有意加固）：

- `spu_SetUserScale` 96B（基线 64B）、`spu_SetUserColor` 48B（基线 20B）——多了 `vlc_mutex` 保护，基线是无锁直接写。

## 使用方式

```gradle
// 或者直接放到 App 的 libs 目录（vlc/build.gradle 用 fileTree 引 libs）
api fileTree(dir: 'libs', include: ['*.jar', '*.aar'])
```

替换前建议备份 App 侧旧 AAR。
