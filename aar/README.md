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
| `libvlc-release-20261106_105410.aar` | 23.9 MB | 在 095230 基础上补 core 空轨/反注册 CLEAR（上一稳定版，可回退） |
| `libvlc-release-20261106_140824.aar` | 23.9 MB | **当前稳定版**：恢复基线性能修复，解决字幕出现时卡顿；MD5 `7658DF4C9C187B36620D1FC4DF578E74` |

## 20261106 版本说明

- `095230`：仅修 `libvlcjni.so` 的 `onSubtitleBitmaps` JNI 签名（原签名多一个 `I`，`GetMethodID` 返回 NULL、异常被 `ExceptionClear` 吞掉，Java 层假成功而 core `pf_bitmap` 始终为空）。
- `105410`：core `vout_subpictures.c` 把调色板改色回退为 bridge 内 alpha 遮罩改色，并在 `vlc_spu_set_bitmap_listener()` 注册/反注册前、`SpuRender()` 空队列提前返回前各补发一次 CLEAR；`libvlc.so` 含 `notify external before listener change`。
- `140824`：恢复 11-02 基线在 11-05 系统重装时被回退掉的**字幕渲染性能路径**（根因与取证见 `docs/2026-11-06_subtitle_takeover/change_notes/2026-11-06_字幕出现卡顿回退修复.md`）：
  - `vout_subpictures.c`：`spu_user_params` 增加 `i_last_order/start/stop`，`SpuRenderSubpictures()` 按 `(order,start,stop)` 同句去重（不再每帧重跑 `SpuRenderRegion`）；接管时 `scale = spu_scale_unit()` 不做 1080p→4K 的 swscale 2 倍放大；坐标与回传基准改回 `subpic->i_original_picture_*`；去重帧直接丢弃 `output` 返回 NULL，既不回传也不补发 CLEAR。
  - `display.c`：恢复 `SubRectUnion` / `SubtitleBufferBoundsGet/Set`，按 buffer 残留区 ∪ 本帧区域清除（`CLEAR rect=[...]`），替换 4K 下 33MB 的整屏 `CLEAR full rect=[...]`；补回空壳 subpicture 早退与输出矩形初始化。
  - `libvlc.so` 含 `CLEAR rect=[...]`、不含 `CLEAR full rect=[...]`，并保留 `105410` 的 CLEAR 补发。**用户真机确认字幕卡顿已解决**。
- 这几个版本均只编 `armeabi-v7a`；App `vlc/libs/` 当前使用 `140824`，`105410` 已移到本地 `Backups\2026-11-06_14-02-02_字幕出现卡顿回退修复\before_app_aar\`。
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
