# VLC 3.6.5 for Android —— hhhc 定制版（rk3528 / rk3518 盒子）

本仓库是 **VideoLAN VLC 3.0.x 的定制分支**，用于 `rk3528 / rk3518` 系列 Android 电视盒子的
libvlc 内核。基线为上游提交 `c9dde25a`（`3.0.22-rc1`），在其之上叠加了字幕、解码兼容与
构建工具链相关的改动。

产物形态：`libvlc.so` + `libvlcjni.so` + `libc++_shared.so`，打包为 AAR 供 Android App 使用。

---

## 一、为什么有这份定制

上游 3.6.5 直接用于该硬件时有几个硬伤：

| 问题 | 现象 |
|---|---|
| 内置 FFmpeg 版本旧 | 与新版 contrib（FFmpeg 8.x）头文件不兼容，`modules/codec/avcodec` 编不过 |
| libsmb2 API 变更 | `modules/access/smb2.c` 编不过（`LIBSMB2_SHARE_ENUM_V2` 等新接口） |
| Android 字幕渲染缺陷 | 字幕出现重叠、残影；4K 下位图字幕再叠一层 RGBA Surface，掉帧明显 |
| 文本字幕只能由内核渲染 | 无法交给 App 自绘（TextView / ImageView），样式与性能都受限 |
| 首次渲染文本字幕极慢 | fontconfig 全量扫描系统字体，首播可能卡顿数十秒 |
| rockit（原生播放器）解不了 HE-AAC | 显式 SBR 音轨 `failed to get output frame`，需要整条走 VLC |

---

## 二、相对上游的改动

### 2.1 解码与解复用兼容

| 文件 | 改动 |
|---|---|
| `modules/codec/avcodec/*`（22 个文件） | 回填新版 FFmpeg 8 的 API（`AVCodecContext` 字段、`avcodec_send_packet` 路径、`ch_layout` 等），使 3.6.5 能与新版 contrib 的 FFmpeg 一起编译 |
| `modules/access/smb2.c` | 适配新版 libsmb2（共享枚举接口变更） |
| `include/vlc_fourcc.h` | 补 6 个缺失的 fourcc 宏：`VLC_CODEC_APV`、`I422_16L`、`I422_16B`、`ATRAC3AL`、`ATRAC3PAL`、`ATRAC9` |

### 2.2 字幕渲染修复（Android）

| 文件 | 改动 |
|---|---|
| `modules/video_output/android/display.c` | 修复字幕重叠/残影：废弃「按 per-buffer dirty bounds 局部清除」（缓冲轮转时 bounds 失效导致旧字幕残留），改为重绘前整块清屏；跳过重绘的判定增加 `i_start/i_stop` 与 `b_sub_on_surface`，避免不同字幕共用 `i_order=0` 时永远 SKIP；新增 `VLCSubFix` 日志 |
| `src/video_output/vout_subpictures.c` | 新增文本/位图字幕桥实现、`SpuRenderSubpicture` 跳过纯文本字幕、`spu_SetUserScale/Color` 参数存储、`SpuRenderCreateAndLoadTextIfNeeded()`（listener-only 时不加载 text renderer，避免 fontconfig 扫描） |
| `include/vlc_subpicture.h` | 新增 `vlc_spu_set_text_listener()` / `vlc_spu_set_bitmap_listener()` / `spu_SetUserScale()` / `spu_SetUserColor()` 声明 |
| `include/vlc/libvlc_media_player.h`、`lib/video.c` | 新增公开 API `libvlc_video_set_sub_scale()` / `libvlc_video_set_sub_color()`（内部转发到 `spu_SetUser*`） |
| `src/input/es_out.c`、`src/input/decoder.c` | 新增 `VLCSpuTrace` 日志（字幕 ES 添加 / 解码器创建成功或失败 / 选中变化），便于定位「字幕不显示」类问题 |

### 2.3 构建工具链

| 文件 | 改动 |
|---|---|
| `contrib/src/main.mak` | 增加 `%.tar.zst` 解包规则；`prebuilt` 目标由 `.tar.bz2` 改为 `.tar.zst`（构建机可从预编译包直接展开，避免联网编译 100+ 依赖） |
| `src/revision.txt` | 固定版本串，避免 `stamp-revision` 在非 git 环境失败 |

### 2.4 配套的 JNI / Java 改动（不在本仓库）

`libvlcjni` 脚手架（`vlc-aar-3.6.5`）内有对应改动，重建时必须一并保留：

| 文件 | 改动 |
|---|---|
| `libvlc/jni/libvlcjni.c` | `setup_fontconfig()`：生成最小 `fonts.conf`（`/system/fonts`、`/product/fonts`、`/vendor/fonts` + 可写 cachedir），并 `setenv("FONTCONFIG_FILE")` |
| `libvlc/jni/libvlcjni-mediaplayer.c` | `nativeSetSubtitleTextListener` / `nativeSetSubtitleBitmapListener` / `nativeSetSubtitleScale` / `nativeSetSubtitleColor` 及其回调桥 |
| `libvlc/src/org/videolan/libvlc/LibVLC.java` | `prepareFontDir()` / `prepareFontCache()`：把 AAR 内 `assets/fonts`、`assets/fontconfig` 拷到应用私有目录 |
| `libvlc/src/org/videolan/libvlc/MediaPlayer.java` | `SubtitleTextListener`、`SubtitleBitmapListener` 接口与 `setSubtitleScale/Color` 方法 |
| `libvlc/assets/fontconfig/*.cache-7`、`assets/fonts/NotoSansSC-subset.otf` | 预生成的 fontconfig 字体缓存与中文子集字体 |

---

## 三、构建

本仓库只包含 **VLC core 源码**。完整 AAR 构建需要配套的 `vlc-aar-3.6.5` 脚手架
（vlc-android + libvlcjni + gradle 工程），其目录约定为：

```
vlc-aar-3.6.5/
├── libvlcjni/
│   └── vlc -> ../../vlc_core-3.6.5      # 软链到本仓库
└── out/
```

### 3.1 环境要求（Ubuntu 24.04 实测）

- gcc-13 / g++-13、make、autoconf、automake、libtool、pkg-config
- cmake、yasm、nasm、meson、ninja、gettext/autopoint、python3
- JDK 17（AGP 8.11 要求）
- Android SDK：`build-tools;36.0.0`、`platforms;android-36`
- Android NDK：`21.4.7075529`（32 位 arm 必需）
- 预编译 contrib 包：`vlc-contrib-arm-linux-androideabi-latest.tar.zst`（约 140 MB）

### 3.2 编译

```bash
# 软链 core 源码
ln -sfn /path/to/vlc_core-3.6.5 vlc-aar-3.6.5/libvlcjni/vlc

# 编 native（-a arm = armeabi-v7a；-t 使用预编译 contrib）
cd vlc-aar-3.6.5
./buildsystem/compile.sh -a arm -l -t -b release

# 打包 AAR
GRADLE_ABI=ARMv7 ./gradlew -p libvlcjni publishToMavenLocal
```

产物：`~/.m2/repository/org/videolan/android/libvlc-all/3.6.5/libvlc-all-3.6.5.aar`

### 3.3 注意事项

- `--disable-nfs` 需要保留（contrib 的 libnfs 16.x 与 3.6.5 core 的 5.x API 不兼容）
- `--disable-chromecast` 在部分环境需要（`extras/tools` 的 protoc 3.1.0 与 contrib 的
  protobuf 3.4.0 生成代码不兼容；若把 protoc 升到 3.4+ 可启用）
- 源码目录**不要**在 `core.autocrlf=true` 的 Windows 侧做 `git checkout`，会把 LF 写成 CRLF
  并破坏 `#define` 续行与 shell 脚本
- 构建机的 `extras/tools` 首次会自编 protoc 等宿主工具（约 5~10 分钟）

---

## 四、目录结构（仅列改动相关）

```
include/
  vlc_subpicture.h                 # 字幕桥 + 用户参数 API
  vlc/libvlc_media_player.h        # libvlc_video_set_sub_scale/color
  vlc_fourcc.h                     # 补充 fourcc 宏
lib/
  video.c                          # 上述 libvlc API 实现
modules/
  codec/avcodec/                   # FFmpeg 8 适配（22 文件）
  access/smb2.c                    # libsmb2 新 API
  video_output/android/display.c   # 字幕重叠/残影修复
src/
  video_output/vout_subpictures.c  # 字幕桥与渲染开关
  input/es_out.c                   # VLCSpuTrace
  input/decoder.c                  # VLCSpuTrace
  revision.txt
contrib/src/main.mak               # tar.zst 支持
```

---

## 五、版本记录

| 日期 | 内容 |
|---|---|
| 2026-09 | 基线建立：字幕重叠/残影修复、文本字幕桥、FFmpeg 8 适配、smb2 适配 |
| 2026-11-02 | 位图字幕 App 接管正式版（`vlc_spu_set_bitmap_listener`、`sub-scale`/`sub-color`） |
| 2026-11-04 | App 侧：HE-AAC（显式 SBR）判定走 VLC；低电平增益补偿 |
| 2026-11-05 | 系统重装后完整重建本树：补齐 `libvlc_video_set_sub_scale/color`、`es_out.c` 的 `VLCSpuTrace`、位图字幕接管（含 `b_has_last` 去重与 CLEAR 通知）、`sub-scale`/`sub-color` 变量化；AAR 逐层比对与基线一致（仅 chromecast 模块因 protoc 版本未启用）。稳定版 AAR 存于 [`aar/`](aar/README.md) |

> **AAR 产物**：每个验证通过的稳定版都放在 [`aar/`](aar/README.md) 目录，含构建时间戳与基线对照，便于回滚。

---

## 六、许可

本仓库基于 VideoLAN VLC，遵循其原始许可（`COPYING`，GPLv2+/LGPLv2.1+，具体见文件头）。
新增代码同样遵循 LGPLv2.1+。VLC 为 VideoLAN 的商标，本仓库为非官方定制分支。
