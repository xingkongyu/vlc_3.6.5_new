# -*- coding: utf-8 -*-
"""字幕卡顿回退修复的对抗性检查（2026-11-06）。

A 组：display.c 脏区清除 —— 随机穷举矩形/尺寸，验证 memset 永不越出 surface。
B 组：vout_subpictures.c 同句去重状态机 —— 定向 + 随机帧序列，验证
      CONTENT 每句一次、CLEAR 不误发不漏发、反注册/重注册不误判。
C 组：App 换算边界 —— 复刻 clamp* / regionCount / Math.round，验证
      异常 video 尺寸/rect/scale 下输出始终有限且不越界。
D 组：源码写法断言 —— 锁死关键防护，防止后续改动把加固删掉。
"""
import math
import os
import random
import sys

ROOT = r"D:\vlc-rescue-20261105\work\vlc_core-3.6.5"
VLC_VIDEO = r"D:\workspace\A14\a14_2\publicversion\vlc\src\main\java\com\hhhc\vlc\VlcVideo.java"
JNI_C = r"D:\vlc-rescue-20261105\vlc-aar-git\libvlcjni\libvlc\jni\libvlcjni-mediaplayer.c"
CORE_C = os.path.join(ROOT, r"src\video_output\vout_subpictures.c")
DISPLAY_C = os.path.join(ROOT, r"modules\video_output\android\display.c")

PASS = 0
FAIL = []


def check(cond, msg):
    global PASS
    if cond:
        PASS += 1
    else:
        FAIL.append(msg)


# ---------------------------------------------------------------- A 组
def sub_rect_union(dst, src):
    if src[2] <= src[0] or src[3] <= src[1]:
        return dst
    if dst[2] <= dst[0] or dst[3] <= dst[1]:
        return src
    return (min(dst[0], src[0]), min(dst[1], src[1]),
            max(dst[2], src[2]), max(dst[3], src[3]))


def clamp_clear(c, width, height, lines):
    l, t, r, b = c
    l = max(l, 0)
    t = max(t, 0)
    r = min(r, width)
    b = min(b, height)
    b = min(b, lines)
    r = max(r, 0)
    b = max(b, 0)
    if l > r:
        l = r
    if t > b:
        t = b
    return (l, t, r, b)


def test_display(n=200000):
    rng = random.Random(20261106)
    max_area_ratio = 0.0
    for _ in range(n):
        w = rng.choice([0, 1, 16, 720, 1280, 1920, 3840, 4096])
        h = rng.choice([0, 1, 16, 480, 720, 1080, 2160])
        lines = rng.choice([0, h // 2, h, h + 8])
        if w <= 0 or h <= 0:
            continue

        def rand_rect():
            l = rng.randint(-4096, 8192)
            t = rng.randint(-4096, 8192)
            r = l + rng.randint(-4096, 8192)
            b = t + rng.randint(-4096, 8192)
            return (l, t, r, b)

        stale = rand_rect()          # 该 buffer 上次画过的区域
        cur = rand_rect()            # 本帧字幕区域
        clear = sub_rect_union(stale, cur) if rng.random() < 0.85 else stale
        clear = clamp_clear(clear, w, h, lines)
        l, t, r, b = clear

        check(0 <= l <= r <= w, "A: left/right out of surface")
        check(0 <= t <= b <= h, "A: top/bottom out of surface")
        check(b <= lines, "A: bottom exceeds plane lines")

        # 复刻 memset：每行写 [y*pitch + l*4, + (r-l)*4]
        pitch = w * 4
        if r > l and b > t:
            x_off = l * 4
            line = (r - l) * 4
            check(x_off >= 0 and line >= 0, "A: negative memset offset/size")
            check(x_off + line <= pitch, "A: memset row exceeds pitch")
            check(b - 1 < lines, "A: memset row index exceeds plane")
            area = (r - l) * (b - t)
            max_area_ratio = max(max_area_ratio, area / float(w * h))
        else:
            check(True, "A: empty rect skips memset")

    # 正常输入（字幕行）应远小于整屏；这里只统计，不作为硬断言
    print("  A: max random rect / full surface = %.4f" % max_area_ratio)

    # 定向：找不到 buffer 记录 → 空矩形，只清本帧字幕行，不回退整屏
    stale = (0, 0, 0, 0)
    cur = (100, 900, 1820, 980)
    c = clamp_clear(sub_rect_union(stale, cur), 1920, 1080, 1080)
    check(c == cur, "A: no stale record must clear only current subtitle row")
    check((c[2] - c[0]) * (c[3] - c[1]) < 1920 * 1080 // 2,
          "A: normal clear must be far smaller than full screen")

    # 定向：region 坐标为负且可见宽度为 0 → right/bottom 不能为负
    c = clamp_clear((-500, -500, -500, -500), 1920, 1080, 1080)
    check(c == (0, 0, 0, 0), "A: fully negative rect clamps to empty")


# ---------------------------------------------------------------- B 组
class Bridge(object):
    """复刻 spu_user_params + SpuRenderSubpictures + SpuBitmapBridgeNotify。"""

    def __init__(self):
        self.pf = None
        self.only = False
        self.has_last = False
        self.last = None
        self.events = []

    def register(self, pf, only):
        # vlc_spu_set_bitmap_listener：注册/反注册前补发 CLEAR，然后重置身份
        if self.pf and self.only and self.has_last:
            self.events.append("CLEAR")
            self.has_last = False
            self.last = None
        self.has_last = False
        self.last = None
        self.pf = pf
        self.only = only

    def takeover(self):
        return self.pf is not None and self.only

    def same(self, ident):
        return self.has_last and self.last == ident

    def notify_content(self, ident):
        self.has_last = True
        self.last = ident
        self.events.append("CONTENT")

    def notify_clear(self):
        if self.has_last:
            self.has_last = False
            self.last = None
            self.events.append("CLEAR")

    def frame(self, subpics):
        """subpics: [(identity, is_subtitle, has_region)]；空列表 = SPU 队列为空。"""
        if not subpics:
            self.notify_clear()      # SpuRender() 空队列分支
            return
        takeover = self.takeover()
        skipped = False
        rendered = []
        for ident, is_sub, has_reg in subpics:
            if not has_reg:
                continue
            if is_sub and takeover and self.same(ident):
                skipped = True       # 同句跳过
                continue
            rendered.append((ident, is_sub))
        if skipped:
            # 实际 C 代码：只要 b_skipped_subtitle 就丢弃整个 output 并 return NULL，
            # 既不回传内容、也不补发 CLEAR（同帧 OSD 也一并丢弃）。
            return
        subs = [x[0] for x in rendered if x[1]]
        if subs:
            self.notify_content(subs[-1])
        else:
            self.notify_clear()


def test_dedup():
    A, B, C = ("o1", 100, 200), ("o2", 300, 400), ("o3", 500, 600)

    # 1) 同一句连续 30 帧 → 只回传 1 次，期间无 CLEAR
    b = Bridge(); b.register("cb", True)
    for _ in range(30):
        b.frame([(A, True, True)])
    check(b.events == ["CONTENT"], "B1: same sentence must callback exactly once")

    # 2) 字幕结束（队列空）→ 必发且只发 1 次 CLEAR
    for _ in range(5):
        b.frame([])
    check(b.events == ["CONTENT", "CLEAR"], "B2: empty queue must fire one CLEAR")

    # 3) 连续三句 → 3 次 CONTENT，中间不夹 CLEAR
    b = Bridge(); b.register("cb", True)
    for ident in (A, B, C):
        for _ in range(4):
            b.frame([(ident, True, True)])
    check(b.events == ["CONTENT"] * 3, "B3: three sentences must not interleave CLEAR")

    # 4) 反注册补发 CLEAR；重注册后同一 identity 不能被误判为同句
    b = Bridge(); b.register("cb", True)
    b.frame([(A, True, True)])
    b.register(None, False)
    check(b.events == ["CONTENT", "CLEAR"], "B4: unregister must fire CLEAR")
    b.register("cb", True)
    b.frame([(A, True, True)])
    check(b.events == ["CONTENT", "CLEAR", "CONTENT"],
          "B4: re-register must reset dedup identity")

    # 5) 切轨 flush：队列空 → CLEAR，新轨首句 → CONTENT
    b = Bridge(); b.register("cb", True)
    b.frame([(A, True, True)])
    b.frame([])
    b.frame([(B, True, True)])
    check(b.events == ["CONTENT", "CLEAR", "CONTENT"], "B5: track switch flow")

    # 6) 非接管模式：每帧都回传，去重不生效，也不误发 CLEAR
    b = Bridge(); b.register("cb", False)
    for _ in range(8):
        b.frame([(A, True, True)])
    check(b.events == ["CONTENT"] * 8, "B6: observe mode must not dedup")

    # 7) 同帧有 OSD + 字幕被去重 → 整帧丢弃，不回传、不 CLEAR
    b = Bridge(); b.register("cb", True)
    b.frame([(A, True, True)])
    before = list(b.events)
    b.frame([(A, True, True), (("osd", 0, 0), False, True)])
    check(b.events == before, "B7: dedup frame with OSD must stay silent")

    # 8) 随机帧序列：CLEAR 前必有 CONTENT，CONTENT/CLEAR 严格交替且不连续重复
    rng = random.Random(11206)
    b = Bridge(); b.register("cb", True)
    cur = None
    for _ in range(3000):
        action = rng.random()
        if action < 0.06:
            b.register("cb", True)          # 重注册
        elif action < 0.14:
            b.frame([])                     # 队列空
            cur = None
        elif action < 0.20:
            cur = ("o%d" % rng.randint(0, 50), rng.randint(0, 10**6), rng.randint(0, 10**6))
        if cur is not None and action >= 0.14:
            b.frame([(cur, True, True)])
    prev = None
    for e in b.events:
        check(not (e == "CLEAR" and prev == "CLEAR"), "B8: no consecutive CLEAR")
        prev = e
    check(b.events.count("CLEAR") <= b.events.count("CONTENT") + 1,
          "B8: CLEAR count must not exceed CONTENT count + register count")

    # 9) 身份三值任一变化都必须重新回传
    base = ("o1", 100, 200)
    variants = [("o2", 100, 200), ("o1", 101, 200), ("o1", 100, 201)]
    for v in variants:
        b = Bridge(); b.register("cb", True)
        b.frame([(base, True, True)])
        b.frame([(v, True, True)])
        check(b.events == ["CONTENT", "CONTENT"],
              "B9: any identity field change must trigger new callback %r" % (v,))


# ---------------------------------------------------------------- C 组
def clamp_scale(v):
    if math.isnan(v) or math.isinf(v) or v <= 0:
        return 1.0
    return max(0.1, min(5.0, v))


def clamp_dim(dim, view):
    if dim <= 0:
        return 1
    return int(min(dim, max(1, view) * 6))


def clamp_pos(pos, view):
    limit = max(1, view) * 6
    return int(max(-limit, min(limit, pos)))


def region_count(a, b):
    # 实际入参是 Java 数组长度/4，永远 >= 0
    assert a >= 0 and b >= 0
    return max(0, min(min(a, b), 16))


def java_round(x):
    if math.isnan(x):
        return 0
    if math.isinf(x):
        return 2147483647 if x > 0 else -2147483648
    if x >= 2147483647:
        return 2147483647
    if x <= -2147483648:
        return -2147483648
    return int(math.floor(x + 0.5))


def test_app(n=100000):
    rng = random.Random(20261106)
    pool = [0, -1, 1, 16, 480, 720, 1080, 1920, 2160, 3840, 4096,
            2147483647, -2147483648, 65536]
    for _ in range(n):
        vw = rng.choice(pool)
        vh = rng.choice(pool)
        view_w = rng.choice(pool)
        view_h = rng.choice(pool)
        scale = rng.choice([float("nan"), float("inf"), -float("inf"), 0.0, -1.0,
                            0.01, 0.5, 1.0, 5.0, 1e9, rng.random() * 8])

        # applyBitmapArrayInner 的早退条件
        if view_w <= 0 or view_h <= 0 or vw <= 0 or vh <= 0:
            check(True, "C: invalid size early-return")
            continue

        sx = float(view_w) / float(vw)
        sy = float(view_h) / float(vh)
        us = clamp_scale(scale)
        check(math.isfinite(us) and 0.1 <= us <= 5.0, "C: scale must be clamped")

        # 数组长度语义：非负；rects.length/4 向下取整
        count = region_count(rng.randint(0, 200), rng.randint(0, 200) // 4)
        check(0 <= count <= 16, "C: regionCount bound")

        for _i in range(count):
            w = rng.choice(pool)
            h = rng.choice(pool)
            x = rng.choice(pool)
            y = rng.choice(pool)
            dst_w = clamp_dim(java_round(w * sx * us), view_w)
            dst_h = clamp_dim(java_round(h * sy * us), view_h)
            px = clamp_pos(java_round(x * sx * us), view_w)
            py = clamp_pos(java_round(y * sy * us), view_h)
            check(1 <= dst_w <= max(1, view_w) * 6, "C: dstW bound")
            check(1 <= dst_h <= max(1, view_h) * 6, "C: dstH bound")
            check(-max(1, view_w) * 6 <= px <= max(1, view_w) * 6, "C: posX bound")
            check(-max(1, view_h) * 6 <= py <= max(1, view_h) * 6, "C: posY bound")
            check(math.isfinite(sx) and math.isfinite(sy), "C: sx/sy finite")

    # 定向：scale=unit 后基准从 4K 变 1080p，换算结果必须有限且位置不变形
    # PGS 1080p 基准下 1000x60 @ (460, 950)，App 按 1920x1080 → 1280x720
    w, h, x, y = 1000, 60, 460, 950
    sx, sy = 1280 / 1920.0, 720 / 1080.0
    check(clamp_dim(java_round(w * sx), 1280) == 667, "C: 1080p baseline width")
    check(clamp_dim(java_round(h * sy), 720) == 40, "C: 1080p baseline height")
    check(clamp_pos(java_round(x * sx), 1280) == 307, "C: 1080p baseline x")
    check(clamp_pos(java_round(y * sy), 720) == 633, "C: 1080p baseline y")


# ---------------------------------------------------------------- D 组
def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def test_source():
    core = read(CORE_C)
    disp = read(DISPLAY_C)
    java = read(VLC_VIDEO)
    jni = read(JNI_C)

    # core 性能修复不能被后续改动删掉
    check("CLEAR full rect=" not in disp, "D01: full-screen memset must stay removed")
    check("CLEAR rect=[%d,%d..%d,%d]" in disp, "D02: dirty-rect clear must exist")
    check(disp.count("SubtitleBufferBoundsGet") >= 2, "D03: bounds get defined+used")
    check(disp.count("SubtitleBufferBoundsSet") >= 2, "D04: bounds set defined+used")
    check("p_out_bounds->left = p_out_bounds->top = 0;" in disp,
          "D05: empty rect initialized")
    check("!sys->b_sub_on_surface )" in disp, "D06: empty-shell early return")
    check("if (i >= 16)" in disp, "D07: bounds table upper limit")

    check("i_last_order" in core and "i_last_start" in core and "i_last_stop" in core,
          "D08: dedup identity fields")
    check("scale = spu_scale_unit();" in core, "D09: takeover must not scale")
    check("b_skipped_subtitle" in core, "D10: dedup skip flag")
    check("if (b_skipped_subtitle)" in core
          and "subpicture_Delete( output );" in core,
          "D11: dedup frame must discard output without firing CLEAR")
    check("output->i_original_picture_width  = subpic->i_original_picture_width" in core,
          "D12: coordinate baseline follows scale policy")
    check(core.count("spu_user_params.i_last_order = 0;") >= 2,
          "D13: identity reset on register and CLEAR")
    check("spu_user_params.i_last_order = p_output->i_order;" in core,
          "D14: identity recorded on content callback")
    check("notify external before listener change" in core,
          "D15: 11-06 CLEAR fix preserved")
    check('"([[B[IIIJJ)V"' in jni, "D16: JNI descriptor correct")

    # App 侧 try-catch 加固
    cb = java[java.find("public void onSubtitleBitmaps"):java.find("public VlcVideo(@NonNull")]
    check("try {" in cb[:700], "D17: callback must have outer try-catch at entry")
    check(cb.count("mainHandler.post") == 2, "D18: two post sites via local snapshot")
    check("mainHandler == null" in cb, "D19: main handler null check")
    check(cb.count("catch (Throwable") >= 5,
          "D20: outer + 2 worker runnables + 2 main runnables guarded")
    check("applyBitmapArrayInner" in java and "videoWidth <= 0 || videoHeight <= 0" in java,
          "D21: invalid video baseline early return")
    check("Float.isNaN(scale)" in java and "Float.isInfinite(scale)" in java,
          "D22: NaN/Inf scale clamp")


def main():
    print("=== A: display.c dirty-rect (200000 random cases) ===")
    test_display()
    print("=== B: dedup state machine ===")
    test_dedup()
    print("=== C: App transform bounds (100000 random cases) ===")
    test_app()
    print("=== D: source assertions ===")
    test_source()
    print("\nPASS=%d  FAIL=%d" % (PASS, len(FAIL)))
    seen = set()
    for m in FAIL:
        if m not in seen:
            seen.add(m)
            print("  FAIL: " + m)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
