/*****************************************************************************
 * vout_subpictures.c : subpicture management functions
 *****************************************************************************
 * Copyright (C) 2000-2007 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Vincent Seguin <seguin@via.ecp.fr>
 *          Samuel Hocevar <sam@zoy.org>
 *          Gildas Bazin <gbazin@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <limits.h>

#include <vlc_common.h>
#include <vlc_modules.h>
#include <vlc_input.h>
#include <vlc_vout.h>
#include <vlc_filter.h>
#include <vlc_spu.h>
/* 文本/位图字幕桥与用户参数声明（VLC_API 决定符号可见性，缺了会导致
 * libvlcjni 链接时报 undefined reference） */
#include <vlc_subpicture.h>
#include <vlc_fourcc.h>   /* VLC_CODEC_RGBA（位图字幕回传的格式判定） */

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "../libvlc.h"
#include "vout_internal.h"
#include "../misc/subpicture.h"

/*****************************************************************************
 * 文本字幕外部渲染桥（Android 自绘场景）
 *
 * 外部（如 libvlcjni/Java 层）注册 vlc_spu_text_cb 后：
 *  - 每当"当前应显示的文本字幕"变化（含清空）时，在 spu 渲染路径上以
 *    与显示完全同步的时机回调一次（去重后约每条字幕一次）；
 *  - b_listener_only=true 时，纯文本字幕不再上屏（由外部自绘），
 *    图形字幕（PGS 等无文本的位图字幕）不受影响、照常上屏。
 *
 * 注意：回调在 vout/spu 渲染线程触发，且此时 spu 内部锁被持有——
 * 回调实现必须只做轻量转发（如 post 到其它线程），严禁同步调用
 * 会进入 vlc 核心的 API。
 *****************************************************************************/
static struct {
    vlc_mutex_t  lock;
    vlc_spu_text_cb pf_text;
    void        *p_data;
    bool         b_listener_only;
    char        *psz_last;          /* 上次回调的文本（去重用） */
} spu_text_bridge = { VLC_STATIC_MUTEX, NULL, NULL, false, NULL };

void vlc_spu_set_text_listener( vlc_spu_text_cb pf_text, void *p_data,
                                bool b_listener_only )
{
    vlc_mutex_lock( &spu_text_bridge.lock );
    spu_text_bridge.pf_text = pf_text;
    spu_text_bridge.p_data = p_data;
    spu_text_bridge.b_listener_only = b_listener_only;
    free( spu_text_bridge.psz_last );
    spu_text_bridge.psz_last = NULL;
    vlc_mutex_unlock( &spu_text_bridge.lock );
}

/* 组装一条字幕 subpicture 的全部文本（各 region/segment 以换行连接）。
 * 返回堆上字符串；无文本 region（纯图形字幕）返回 NULL。 */
static char *spu_text_bridge_text( subpicture_t *subpic )
{
    size_t len = 0;
    for ( subpicture_region_t *r = subpic->p_region; r; r = r->p_next ) {
        if ( !r->p_text )
            continue;
        for ( text_segment_t *s = r->p_text; s; s = s->p_next ) {
            if ( !s->psz_text )
                continue;
            len += strlen( s->psz_text );
        }
        len += 1; /* 换行 */
    }
    if ( len == 0 )
        return NULL;

    char *psz = malloc( len + 1 );
    if ( !psz )
        return NULL;
    size_t off = 0;
    for ( subpicture_region_t *r = subpic->p_region; r; r = r->p_next ) {
        if ( !r->p_text )
            continue;
        for ( text_segment_t *s = r->p_text; s; s = s->p_next ) {
            if ( !s->psz_text )
                continue;
            size_t l = strlen( s->psz_text );
            memcpy( psz + off, s->psz_text, l );
            off += l;
        }
        psz[off++] = '\n';
    }
    psz[off] = '\0';
    return psz;
}

/* 去重并回调（psz_text 为 "" 表示清空）。须在持有 sys->lock 时调用。 */
static void spu_text_bridge_update( const char *psz_text,
                                    int64_t i_start, int64_t i_stop )
{
    vlc_mutex_lock( &spu_text_bridge.lock );
    vlc_spu_text_cb pf = spu_text_bridge.pf_text;
    if ( !pf ) {
        vlc_mutex_unlock( &spu_text_bridge.lock );
        return;
    }
    if ( spu_text_bridge.psz_last && psz_text
      && !strcmp( spu_text_bridge.psz_last, psz_text ) ) {
        vlc_mutex_unlock( &spu_text_bridge.lock );
        return; /* 与上次相同，不重复回调 */
    }
    free( spu_text_bridge.psz_last );
    spu_text_bridge.psz_last = strdup( psz_text ? psz_text : "" );
    void *p_data = spu_text_bridge.p_data;
    vlc_mutex_unlock( &spu_text_bridge.lock );

    pf( p_data, psz_text ? psz_text : "", i_start, i_stop );
}

/*****************************************************************************
 * 图形字幕用户参数（缩放 / 颜色）与位图回传监听
 *
 * 由 libvlcjni 通过 MediaPlayer.setSubtitleScale / setSubtitleColor /
 * setSubtitleBitmapListener 下发。参数是进程级（App 只用一个 LibVLC 实例）。
 *
 * 位图字幕接管（b_bitmap_only）时，core 把渲染好的 RGBA region 回传给 App，
 * 不再上屏；i_last_* 记录上一次回传的字幕身份，用于避免 spu_Render 每帧
 * 重复执行 SpuRenderRegion（见 SpuRenderSubpictures 里的同句跳过）。
 *****************************************************************************/
static struct {
    vlc_mutex_t       lock;
    float             f_scale;        /* 0 = 未显式设置，回落到继承值（媒体选项） */
    int               i_color;        /* 0 = 不重着色 */
    vlc_spu_bitmap_cb pf_bitmap;
    void             *p_bitmap_data;
    bool              b_bitmap_only;
    bool              b_has_last;     /* 上次是否已回传内容，用于补发 CLEAR */
    int64_t           i_last_order;   /* 上次回传字幕的身份（同句去重） */
    int64_t           i_last_start;
    int64_t           i_last_stop;
} spu_user_params = { VLC_STATIC_MUTEX, 0.f, 0, NULL, NULL, false, false, 0, 0, 0 };

#ifdef __ANDROID__
# define SPUBMPTRACE(...) __android_log_print( ANDROID_LOG_INFO, "VLCSubBitmap", __VA_ARGS__ )
#else
# define SPUBMPTRACE(...) do {} while ( 0 )
#endif

void spu_SetUserScale( float f_scale )
{
    if ( f_scale < 0.1f )
        f_scale = 0.1f;
    else if ( f_scale > 5.f )
        f_scale = 5.f;

    vlc_mutex_lock( &spu_user_params.lock );
    spu_user_params.f_scale = f_scale;
    vlc_mutex_unlock( &spu_user_params.lock );
}

void spu_SetUserColor( int i_color )
{
    vlc_mutex_lock( &spu_user_params.lock );
    spu_user_params.i_color = i_color;
    vlc_mutex_unlock( &spu_user_params.lock );
}

void vlc_spu_set_bitmap_listener( vlc_spu_bitmap_cb pf_bitmap, void *p_data,
                                  bool b_listener_only )
{
    /* 注册/反注册之前先补发一次 CLEAR：如果接管方屏幕上还留着上一轨
     * (或上一集) 的最后一句字幕，这里不清就会一直挂着 —— 尤其
     * setSpuTrack(-1) 关闭字幕后 SPU 队列变空、SpuRender() 直接 return，
     * 内核不会再有任何清空通知。
     *
     * 必须在本函数的持锁区之外调用回调：SpuBitmapBridgeNotify() 内部也要拿
     * spu_user_params.lock，同一把锁重入会死锁，所以这里手工取快照再补发。 */
    vlc_mutex_lock( &spu_user_params.lock );
    const vlc_spu_bitmap_cb pf_old = spu_user_params.pf_bitmap;
    void *p_old_data = spu_user_params.p_bitmap_data;
    const bool b_old_only = spu_user_params.b_bitmap_only;
    const bool b_old_has_last = spu_user_params.b_has_last;
    vlc_mutex_unlock( &spu_user_params.lock );

    if ( pf_old && b_old_only && b_old_has_last )
    {
        pf_old( p_old_data, 0, NULL, NULL, 0, 0, 0, 0 );
        SPUBMPTRACE( "bitmap clear: notify external before listener change" );
    }

    vlc_mutex_lock( &spu_user_params.lock );
    /* 重新注册时必须清掉"上次已回传"标记：否则切轨后相同内容不再回调，
     * App 侧会停在上一轨的旧画面上。 */
    spu_user_params.b_has_last = false;
    spu_user_params.i_last_order = 0;
    spu_user_params.i_last_start = 0;
    spu_user_params.i_last_stop  = 0;
    spu_user_params.pf_bitmap = pf_bitmap;
    spu_user_params.p_bitmap_data = p_data;
    spu_user_params.b_bitmap_only = b_listener_only;
    vlc_mutex_unlock( &spu_user_params.lock );

    SPUBMPTRACE( "bitmap listener %s (%s)",
                 pf_bitmap ? "registered" : "cleared",
                 b_listener_only ? "takeover" : "observe" );
}

/* 生效中的缩放：显式设置优先，否则继承 playlist / media option 的 sub-scale */
static float SpuUserScaleGet( spu_t *spu )
{
    vlc_mutex_lock( &spu_user_params.lock );
    const float f = spu_user_params.f_scale;
    vlc_mutex_unlock( &spu_user_params.lock );

    if ( f > 0.f )
        return f;
    return var_InheritFloat( spu, "sub-scale" );
}

static int SpuUserColorGet( void )
{
    vlc_mutex_lock( &spu_user_params.lock );
    const int c = spu_user_params.i_color;
    vlc_mutex_unlock( &spu_user_params.lock );
    return c;
}

/**
 * 把渲染完成的位图 region 回传给外部（App 用 ImageView 绘制）。
 *
 * @return true  已接管，调用方不要再上屏（输出已被消费）
 *         false 未接管，按内置渲染正常上屏
 *
 * 只在「注册了回调 + listener_only」且所有 region 都是 RGBA 时才接管；
 * 遇到其它 chroma 主动放弃，宁可退化到内置渲染，也不能出现"接管了却画不出来"。
 */
static bool SpuBitmapBridgeNotify( spu_t *spu, subpicture_t *p_output )
{
    vlc_mutex_lock( &spu_user_params.lock );
    vlc_spu_bitmap_cb pf = spu_user_params.pf_bitmap;
    void *p_data = spu_user_params.p_bitmap_data;
    const bool b_only = spu_user_params.b_bitmap_only;
    bool b_has_last = spu_user_params.b_has_last;
    vlc_mutex_unlock( &spu_user_params.lock );

    if ( !pf || !b_only )
        return false;

    int i_count = 0;
    for ( subpicture_region_t *r = p_output ? p_output->p_region : NULL;
          r; r = r->p_next )
    {
        if ( !r->p_picture )
            continue;
        if ( r->fmt.i_chroma != VLC_CODEC_RGBA )
            return false;
        i_count++;
    }

    if ( i_count == 0 )
    {
        /* 无内容：通知外部清空（仅在上次确实有内容时发，避免重复 CLEAR） */
        if ( b_has_last )
        {
            vlc_mutex_lock( &spu_user_params.lock );
            spu_user_params.b_has_last = false;
            spu_user_params.i_last_order = 0;
            spu_user_params.i_last_start = 0;
            spu_user_params.i_last_stop  = 0;
            vlc_mutex_unlock( &spu_user_params.lock );

            SPUBMPTRACE( "bitmap clear: notify external to hide subtitle" );
            pf( p_data, 0, NULL, NULL, 0, 0, 0, 0 );
        }
        return p_output != NULL;
    }

    const int i_video_w = p_output->i_original_picture_width;
    const int i_video_h = p_output->i_original_picture_height;

    const uint8_t **pp_rgba = vlc_alloc( i_count, sizeof(*pp_rgba) );
    int *p_rects = vlc_alloc( (size_t) i_count * 4, sizeof(int) );
    if ( !pp_rgba || !p_rects )
    {
        free( pp_rgba );
        free( p_rects );
        return false;
    }

    const int i_color = SpuUserColorGet();
    const int i_r = ( i_color >> 16 ) & 0xff;
    const int i_g = ( i_color >>  8 ) & 0xff;
    const int i_b =   i_color         & 0xff;

    int n = 0;
    for ( subpicture_region_t *r = p_output->p_region; r && n < i_count; r = r->p_next )
    {
        if ( !r->p_picture )
            continue;
        picture_t *pic = r->p_picture;
        const int w = pic->format.i_width;
        const int h = pic->format.i_height;
        if ( w <= 0 || h <= 0 )
            continue;

        uint8_t *buf = malloc( (size_t) w * h * 4 );
        if ( !buf )
            break;

        for ( int y = 0; y < h; y++ )
        {
            const uint8_t *src = pic->p[0].p_pixels + (size_t) y * pic->p[0].i_pitch;
            uint8_t *dst = buf + (size_t) y * w * 4;
            if ( i_color == 0 )
            {
                memcpy( dst, src, (size_t) w * 4 );
            }
            else
            {   /* 用原 alpha 作遮罩替换 RGB（0xAARRGGBB） */
                for ( int x = 0; x < w; x++ )
                {
                    dst[0] = (uint8_t) i_r;
                    dst[1] = (uint8_t) i_g;
                    dst[2] = (uint8_t) i_b;
                    dst[3] = src[3];
                    src += 4;
                    dst += 4;
                }
            }
        }

        pp_rgba[n] = buf;
        p_rects[n * 4 + 0] = r->i_x;   /* 位置在 region 上，不在 fmt 里 */
        p_rects[n * 4 + 1] = r->i_y;
        p_rects[n * 4 + 2] = w;
        p_rects[n * 4 + 3] = h;
        n++;
    }

    if ( n == 0 )
    {
        free( pp_rgba );
        free( p_rects );
        return false;
    }

    vlc_mutex_lock( &spu_user_params.lock );
    spu_user_params.b_has_last = true;
    spu_user_params.i_last_order = p_output->i_order;
    spu_user_params.i_last_start = p_output->i_start;
    spu_user_params.i_last_stop  = p_output->i_stop;
    vlc_mutex_unlock( &spu_user_params.lock );

    pf( p_data, n, pp_rgba, p_rects, i_video_w, i_video_h,
        p_output->i_start, p_output->i_stop );

    for ( int i = 0; i < n; i++ )
        free( (void *) pp_rgba[i] );
    free( pp_rgba );
    free( p_rects );
    return true;
}

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

/* Number of simultaneous subpictures */
#define VOUT_MAX_SUBPICTURES (__MAX(VOUT_MAX_PICTURES, SPU_MAX_PREPARE_TIME/5000))

/* */
typedef struct {
    subpicture_t *subpicture;
    bool          reject;
} spu_heap_entry_t;

typedef struct {
    spu_heap_entry_t entry[VOUT_MAX_SUBPICTURES];
} spu_heap_t;

struct spu_private_t {
    vlc_mutex_t  lock;            /* lock to protect all followings fields */
    vlc_object_t *input;

    spu_heap_t   heap;

    int channel;             /**< number of subpicture channels registered */
    filter_t *text;                              /**< text renderer module */
    filter_t *scale_yuvp;                     /**< scaling module for YUVP */
    filter_t *scale;                    /**< scaling module (all but YUVP) */
    bool force_crop;                     /**< force cropping of subpicture */
    struct {
        int x;
        int y;
        int width;
        int height;
    } crop;                                                  /**< cropping */

    int     margin;                    /**< force position of a subpicture */
    bool    force_palette;                /**< force palette of subpicture */
    uint8_t palette[4][4];                             /**< forced palette */

    /* Subpiture filters */
    char           *source_chain_current;
    char           *source_chain_update;
    vlc_mutex_t    source_chain_lock;
    filter_chain_t *source_chain;
    char           *filter_chain_current;
    char           *filter_chain_update;
    vlc_mutex_t    filter_chain_lock;
    filter_chain_t *filter_chain;

    /* */
    vlc_tick_t          last_sort_date;
    vout_thread_t       *vout;
};

/*****************************************************************************
 * heap management
 *****************************************************************************/
static void SpuHeapInit(spu_heap_t *heap)
{
    for (int i = 0; i < VOUT_MAX_SUBPICTURES; i++) {
        spu_heap_entry_t *e = &heap->entry[i];

        e->subpicture = NULL;
        e->reject     = false;
    }
}

static int SpuHeapPush(spu_heap_t *heap, subpicture_t *subpic)
{
    for (int i = 0; i < VOUT_MAX_SUBPICTURES; i++) {
        spu_heap_entry_t *e = &heap->entry[i];

        if (e->subpicture)
            continue;

        e->subpicture = subpic;
        e->reject     = false;
        return VLC_SUCCESS;
    }
    return VLC_EGENERIC;
}

static void SpuHeapDeleteAt(spu_heap_t *heap, int index)
{
    spu_heap_entry_t *e = &heap->entry[index];

    if (e->subpicture)
        subpicture_Delete(e->subpicture);

    e->subpicture = NULL;
}

static int SpuHeapDeleteSubpicture(spu_heap_t *heap, subpicture_t *subpic)
{
    for (int i = 0; i < VOUT_MAX_SUBPICTURES; i++) {
        spu_heap_entry_t *e = &heap->entry[i];

        if (e->subpicture != subpic)
            continue;

        SpuHeapDeleteAt(heap, i);
        return VLC_SUCCESS;
    }
    return VLC_EGENERIC;
}

static void SpuHeapClean(spu_heap_t *heap)
{
    for (int i = 0; i < VOUT_MAX_SUBPICTURES; i++) {
        spu_heap_entry_t *e = &heap->entry[i];
        if (e->subpicture)
            subpicture_Delete(e->subpicture);
    }
}

static void FilterRelease(filter_t *filter)
{
    if (filter->p_module)
        module_unneed(filter, filter->p_module);
    vlc_object_release(filter);
}

static picture_t *spu_new_video_buffer(filter_t *filter)
{
    const video_format_t *fmt = &filter->fmt_out.video;

    return picture_NewFromFormat(fmt);
}

static int spu_get_attachments(filter_t *filter,
                               input_attachment_t ***attachment_ptr,
                               int *attachment_count)
{
    spu_t *spu = filter->owner.sys;

    int ret = VLC_EGENERIC;
    if (spu->p->input)
        ret = input_Control((input_thread_t*)spu->p->input,
                            INPUT_GET_ATTACHMENTS,
                            attachment_ptr, attachment_count);
    return ret;
}

static filter_t *SpuRenderCreateAndLoadText(spu_t *spu)
{
    filter_t *text = vlc_custom_create(spu, sizeof(*text), "spu text");
    if (!text)
        return NULL;

    text->owner.sys = spu;

    es_format_Init(&text->fmt_in, VIDEO_ES, 0);

    es_format_Init(&text->fmt_out, VIDEO_ES, 0);
    text->fmt_out.video.i_width          =
    text->fmt_out.video.i_visible_width  = 32;
    text->fmt_out.video.i_height         =
    text->fmt_out.video.i_visible_height = 32;

    text->pf_get_attachments = spu_get_attachments;

    text->p_module = module_need(text, "text renderer", "$text-renderer", false);

    /* Create a few variables used for enhanced text rendering */
    var_Create(text, "spu-elapsed",   VLC_VAR_INTEGER);
    var_Create(text, "text-rerender", VLC_VAR_BOOL);

    return text;
}

/* 文本字幕外部自绘（listener-only）时不需要内核文本渲染。
 * 直接跳过加载 text renderer，可避免 freetype → fontconfig 全量扫描字体
 * （实测首播卡顿/黑屏的根因之一），同时省掉无用的模块初始化。 */
static filter_t *SpuRenderCreateAndLoadTextIfNeeded(spu_t *spu)
{
    vlc_mutex_lock(&spu_text_bridge.lock);
    const bool b_listener_only = spu_text_bridge.b_listener_only;
    vlc_mutex_unlock(&spu_text_bridge.lock);

    if (b_listener_only)
        return NULL;
    return SpuRenderCreateAndLoadText(spu);
}

static filter_t *SpuRenderCreateAndLoadScale(vlc_object_t *object,
                                             vlc_fourcc_t src_chroma,
                                             vlc_fourcc_t dst_chroma,
                                             bool require_resize)
{
    filter_t *scale = vlc_custom_create(object, sizeof(*scale), "scale");
    if (!scale)
        return NULL;

    es_format_Init(&scale->fmt_in, VIDEO_ES, 0);
    scale->fmt_in.video.i_chroma = src_chroma;
    scale->fmt_in.video.i_width =
    scale->fmt_in.video.i_visible_width =
    scale->fmt_in.video.i_height =
    scale->fmt_in.video.i_visible_height = 32;

    es_format_Init(&scale->fmt_out, VIDEO_ES, 0);
    scale->fmt_out.video.i_chroma = dst_chroma;
    scale->fmt_out.video.i_width =
    scale->fmt_out.video.i_visible_width =
    scale->fmt_out.video.i_height =
    scale->fmt_out.video.i_visible_height = require_resize ? 16 : 32;

    scale->owner.video.buffer_new = spu_new_video_buffer;

    scale->p_module = module_need(scale, "video converter", NULL, false);

    return scale;
}

static void SpuRenderText(spu_t *spu, bool *rerender_text,
                          subpicture_region_t *region,
                          const vlc_fourcc_t *chroma_list,
                          vlc_tick_t elapsed_time)
{
    filter_t *text = spu->p->text;

    assert(region->fmt.i_chroma == VLC_CODEC_TEXT);

    if (!text || !text->p_module)
        return;

    /* Setup 3 variables which can be used to render
     * time-dependent text (and effects). The first indicates
     * the total amount of time the text will be on screen,
     * the second the amount of time it has already been on
     * screen (can be a negative value as text is laid out
     * before it is rendered) and the third is a feedback
     * variable from the renderer - if the renderer sets it
     * then this particular text is time-dependent, eg. the
     * visual progress bar inside the text in karaoke and the
     * text needs to be rendered multiple times in order for
     * the effect to work - we therefore need to return the
     * region to its original state at the end of the loop,
     * instead of leaving it in YUVA or YUVP.
     * Any renderer which is unaware of how to render
     * time-dependent text can happily ignore the variables
     * and render the text the same as usual - it should at
     * least show up on screen, but the effect won't change
     * the text over time.
     */
    var_SetInteger(text, "spu-elapsed", elapsed_time);
    var_SetBool(text, "text-rerender", false);

    if ( region->p_text )
        text->pf_render(text, region, region, chroma_list);
    *rerender_text = var_GetBool(text, "text-rerender");
}

/**
 * A few scale functions helpers.
 */

#define SCALE_UNIT (10000)
typedef struct {
    int w;
    int h;
} spu_scale_t;

static spu_scale_t spu_scale_create(int w, int h)
{
    spu_scale_t s = { .w = w, .h = h };
    if (s.w <= 0)
        s.w = SCALE_UNIT;
    if (s.h <= 0)
        s.h = SCALE_UNIT;
    return s;
}
static spu_scale_t spu_scale_unit(void)
{
    return spu_scale_create(SCALE_UNIT, SCALE_UNIT);
}
static spu_scale_t spu_scale_createq(int64_t wn, int64_t wd, int64_t hn, int64_t hd)
{
    return spu_scale_create(wn * SCALE_UNIT / wd,
                            hn * SCALE_UNIT / hd);
}
static int spu_scale_w(int v, const spu_scale_t s)
{
    return v * s.w / SCALE_UNIT;
}
static int spu_scale_h(int v, const spu_scale_t s)
{
    return v * s.h / SCALE_UNIT;
}
static int spu_invscale_w(int v, const spu_scale_t s)
{
    return v * SCALE_UNIT / s.w;
}
static int spu_invscale_h(int v, const spu_scale_t s)
{
    return v * SCALE_UNIT / s.h;
}

/**
 * A few area functions helpers
 */
typedef struct {
    int x;
    int y;
    int width;
    int height;

    spu_scale_t scale;
} spu_area_t;

static spu_area_t spu_area_create(int x, int y, int w, int h, spu_scale_t s)
{
    spu_area_t a = { .x = x, .y = y, .width = w, .height = h, .scale = s };
    return a;
}
static spu_area_t spu_area_scaled(spu_area_t a)
{
    if (a.scale.w == SCALE_UNIT && a.scale.h == SCALE_UNIT)
        return a;

    a.x      = spu_scale_w(a.x,      a.scale);
    a.y      = spu_scale_h(a.y,      a.scale);
    a.width  = spu_scale_w(a.width,  a.scale);
    a.height = spu_scale_h(a.height, a.scale);

    a.scale = spu_scale_unit();
    return a;
}
static spu_area_t spu_area_unscaled(spu_area_t a, spu_scale_t s)
{
    if (a.scale.w == s.w && a.scale.h == s.h)
        return a;

    a = spu_area_scaled(a);

    a.x      = spu_invscale_w(a.x,      s);
    a.y      = spu_invscale_h(a.y,      s);
    a.width  = spu_invscale_w(a.width,  s);
    a.height = spu_invscale_h(a.height, s);

    a.scale = s;
    return a;
}
static bool spu_area_overlap(spu_area_t a, spu_area_t b)
{
    a = spu_area_scaled(a);
    b = spu_area_scaled(b);

    return __MAX(a.x, b.x) < __MIN(a.x + a.width , b.x + b.width ) &&
           __MAX(a.y, b.y) < __MIN(a.y + a.height, b.y + b.height);
}

/**
 * Avoid area overlapping
 */
static void SpuAreaFixOverlap(spu_area_t *dst,
                              const spu_area_t *sub_array, int sub_count, int align)
{
    spu_area_t a = spu_area_scaled(*dst);
    bool is_moved = false;
    bool is_ok;

    /* Check for overlap
     * XXX It is not fast O(n^2) but we should not have a lot of region */
    do {
        is_ok = true;
        for (int i = 0; i < sub_count; i++) {
            spu_area_t sub = spu_area_scaled(sub_array[i]);

            if (!spu_area_overlap(a, sub))
                continue;

            if (align & SUBPICTURE_ALIGN_TOP) {
                /* We go down */
                int i_y = sub.y + sub.height;
                a.y = i_y;
                is_moved = true;
            } else if (align & SUBPICTURE_ALIGN_BOTTOM) {
                /* We go up */
                int i_y = sub.y - a.height;
                a.y = i_y;
                is_moved = true;
            } else {
                /* TODO what to do in this case? */
                //fprintf(stderr, "Overlap with unsupported alignment\n");
                break;
            }

            is_ok = false;
            break;
        }
    } while (!is_ok);

    if (is_moved)
        *dst = spu_area_unscaled(a, dst->scale);
}


static void SpuAreaFitInside(spu_area_t *area, const spu_area_t *boundary)
{
    spu_area_t a = spu_area_scaled(*area);

    const int i_error_x = (a.x + a.width) - boundary->width;
    if (i_error_x > 0)
        a.x -= i_error_x;
    if (a.x < 0)
        a.x = 0;

    const int i_error_y = (a.y + a.height) - boundary->height;
    if (i_error_y > 0)
        a.y -= i_error_y;
    if (a.y < 0)
        a.y = 0;

    *area = spu_area_unscaled(a, area->scale);
}

/**
 * Place a region
 */
static void SpuRegionPlace(int *x, int *y,
                           const subpicture_t *subpic,
                           const subpicture_region_t *region)
{
    assert(region->i_x != INT_MAX && region->i_y != INT_MAX);
    if (subpic->b_absolute) {
        *x = region->i_x;
        *y = region->i_y;
    } else {
        if (region->i_align & SUBPICTURE_ALIGN_TOP)
            *y = region->i_y;
        else if (region->i_align & SUBPICTURE_ALIGN_BOTTOM)
            *y = subpic->i_original_picture_height - region->fmt.i_visible_height - region->i_y;
        else
            *y = subpic->i_original_picture_height / 2 - region->fmt.i_visible_height / 2;

        if (region->i_align & SUBPICTURE_ALIGN_LEFT)
            *x = region->i_x;
        else if (region->i_align & SUBPICTURE_ALIGN_RIGHT)
            *x = subpic->i_original_picture_width - region->fmt.i_visible_width - region->i_x;
        else
            *x = subpic->i_original_picture_width / 2 - region->fmt.i_visible_width / 2;
    }
}

/**
 * This function compares two 64 bits integers.
 * It can be used by qsort.
 */
static int IntegerCmp(int64_t i0, int64_t i1)
{
    return i0 < i1 ? -1 : i0 > i1 ? 1 : 0;
}
/**
 * This function compares 2 subpictures using the following properties
 * (ordered by priority)
 * 1. absolute positionning
 * 2. start time
 * 3. creation order (per channel)
 *
 * It can be used by qsort.
 *
 * XXX spu_RenderSubpictures depends heavily on this order.
 */
static int SubpictureCmp(const void *s0, const void *s1)
{
    subpicture_t *subpic0 = *(subpicture_t**)s0;
    subpicture_t *subpic1 = *(subpicture_t**)s1;
    int r;

    r = IntegerCmp(!subpic0->b_absolute, !subpic1->b_absolute);
    if (!r)
        r = IntegerCmp(subpic0->i_start, subpic1->i_start);
    if (!r)
        r = IntegerCmp(subpic0->i_channel, subpic1->i_channel);
    if (!r)
        r = IntegerCmp(subpic0->i_order, subpic1->i_order);
    return r;
}

/*****************************************************************************
 * SpuSelectSubpictures: find the subpictures to display
 *****************************************************************************
 * This function parses all subpictures and decides which ones need to be
 * displayed. If no picture has been selected, display_date will depend on
 * the subpicture.
 * We also check for ephemer DVD subpictures (subpictures that have
 * to be removed if a newer one is available), which makes it a lot
 * more difficult to guess if a subpicture has to be rendered or not.
 *****************************************************************************/
static void SpuSelectSubpictures(spu_t *spu,
                                 unsigned int *subpicture_count,
                                 subpicture_t **subpicture_array,
                                 vlc_tick_t render_subtitle_date,
                                 vlc_tick_t render_osd_date,
                                 bool ignore_osd)
{
    spu_private_t *sys = spu->p;

    /* */
    *subpicture_count = 0;

    /* Create a list of channels */
    int channel[VOUT_MAX_SUBPICTURES];
    int channel_count = 0;

    for (int index = 0; index < VOUT_MAX_SUBPICTURES; index++) {
        spu_heap_entry_t *entry = &sys->heap.entry[index];
        if (!entry->subpicture || entry->reject)
            continue;
        const int i_channel = entry->subpicture->i_channel;
        int i;
        for (i = 0; i < channel_count; i++) {
            if (channel[i] == i_channel)
                break;
        }
        if (channel_count <= i)
            channel[channel_count++] = i_channel;
    }

    /* Ensure a 1 pass garbage collection for rejected subpictures */
    if(channel_count == 0)
        channel[channel_count++] = VOUT_SPU_CHANNEL_INVALID;

    /* Fill up the subpicture_array arrays with relevant pictures */
    for (int i = 0; i < channel_count; i++) {
        subpicture_t *available_subpic[VOUT_MAX_SUBPICTURES];
        bool         is_available_late[VOUT_MAX_SUBPICTURES];
        int          available_count = 0;

        vlc_tick_t   start_date = render_subtitle_date;
        vlc_tick_t   ephemer_subtitle_date = 0;
        vlc_tick_t   ephemer_osd_date = 0;
        int64_t      ephemer_subtitle_order = INT64_MIN;
        int64_t      ephemer_system_order = INT64_MIN;

        /* Select available pictures */
        for (int index = 0; index < VOUT_MAX_SUBPICTURES; index++) {
            spu_heap_entry_t *entry = &sys->heap.entry[index];
            subpicture_t *current = entry->subpicture;
            bool is_stop_valid;
            bool is_late;

            if (!current || entry->reject) {
                if (entry->reject)
                    SpuHeapDeleteAt(&sys->heap, index);
                continue;
            }

            if (current->i_channel != channel[i] ||
               (ignore_osd && !current->b_subtitle))
                continue;

            const vlc_tick_t render_date = current->b_subtitle ? render_subtitle_date : render_osd_date;
            if (render_date &&
                render_date < current->i_start) {
                /* Too early, come back next monday */
                continue;
            }

            vlc_tick_t *ephemer_date_ptr  = current->b_subtitle ? &ephemer_subtitle_date  : &ephemer_osd_date;
            int64_t *ephemer_order_ptr = current->b_subtitle ? &ephemer_subtitle_order : &ephemer_system_order;
            if (current->i_start >= *ephemer_date_ptr) {
                *ephemer_date_ptr = current->i_start;
                if (current->i_order > *ephemer_order_ptr)
                    *ephemer_order_ptr = current->i_order;
            }

            is_stop_valid = !current->b_ephemer || current->i_stop > current->i_start;

            is_late = is_stop_valid && current->i_stop <= render_date;

            /* start_date will be used for correct automatic overlap support
             * in case picture that should not be displayed anymore (display_time)
             * overlap with a picture to be displayed (current->i_start)  */
            if (current->b_subtitle && !is_late && !current->b_ephemer)
                start_date = current->i_start;

            /* */
            available_subpic[available_count] = current;
            is_available_late[available_count] = is_late;
            available_count++;
        }

        /* Only forced old picture display at the transition */
        if (start_date < sys->last_sort_date)
            start_date = sys->last_sort_date;
        if (start_date <= 0)
            start_date = INT64_MAX;

        /* Select pictures to be displayed */
        for (int index = 0; index < available_count; index++) {
            subpicture_t *current = available_subpic[index];
            bool is_late = is_available_late[index];

            const vlc_tick_t stop_date = current->b_subtitle ? __MAX(start_date, sys->last_sort_date) : render_osd_date;
            const vlc_tick_t ephemer_date  = current->b_subtitle ? ephemer_subtitle_date  : ephemer_osd_date;
            const int64_t ephemer_order = current->b_subtitle ? ephemer_subtitle_order : ephemer_system_order;

            /* Destroy late and obsolete ephemer subpictures */
            bool is_rejeted = is_late && current->i_stop <= stop_date;
            if (current->b_ephemer) {
                if (current->i_start < ephemer_date)
                    is_rejeted = true;
                else if (current->i_start == ephemer_date &&
                         current->i_order < ephemer_order)
                    is_rejeted = true;
            }

            if (is_rejeted)
                SpuHeapDeleteSubpicture(&sys->heap, current);
            else
                subpicture_array[(*subpicture_count)++] = current;
        }
    }

    sys->last_sort_date = render_subtitle_date;
}



/**
 * It will transform the provided region into another region suitable for rendering.
 */
static void SpuRenderRegion(spu_t *spu,
                            subpicture_region_t **dst_ptr, spu_area_t *dst_area,
                            subpicture_t *subpic, subpicture_region_t *region,
                            const spu_scale_t scale_size,
                            const vlc_fourcc_t *chroma_list,
                            const video_format_t *fmt,
                            const spu_area_t *subtitle_area, int subtitle_area_count,
                            vlc_tick_t render_date)
{
    spu_private_t *sys = spu->p;

    video_format_t fmt_original = region->fmt;
    bool restore_text = false;
    int x_offset;
    int y_offset;

    video_format_t region_fmt;
    picture_t *region_picture;

    /* Invalidate area by default */
    *dst_area = spu_area_create(0,0, 0,0, scale_size);
    *dst_ptr  = NULL;

    /* Render text region */
    if (region->fmt.i_chroma == VLC_CODEC_TEXT) {
        // assume rendered text is in sRGB if nothing is set
        if (region->fmt.transfer == TRANSFER_FUNC_UNDEF)
            region->fmt.transfer = TRANSFER_FUNC_SRGB;
        if (region->fmt.primaries == COLOR_PRIMARIES_UNDEF)
            region->fmt.primaries = COLOR_PRIMARIES_SRGB;
        if (region->fmt.space == COLOR_SPACE_UNDEF)
            region->fmt.space = COLOR_SPACE_SRGB;

        SpuRenderText(spu, &restore_text, region,
                      chroma_list,
                      render_date - subpic->i_start);

        /* Check if the rendering has failed ... */
        if (region->fmt.i_chroma == VLC_CODEC_TEXT)
            goto exit;
    }

    video_format_AdjustColorSpace(&region->fmt);

    /* Force palette if requested
     * FIXME b_force_palette and force_crop are applied to all subpictures using palette
     * instead of only the right one (being the dvd spu).
     */
    const bool using_palette = region->fmt.i_chroma == VLC_CODEC_YUVP;
    const bool force_palette = using_palette && sys->force_palette;
    const bool crop_requested = (force_palette && sys->force_crop) ||
                                region->i_max_width || region->i_max_height;
    bool changed_palette     = false;

    /* Compute the margin which is expressed in destination pixel unit
     * The margin is applied only to subtitle and when no forced crop is
     * requested (dvd menu) */
    int y_margin = 0;
    if (!crop_requested && subpic->b_subtitle)
        y_margin = spu_invscale_h(sys->margin, scale_size);

    /* Place the picture
     * We compute the position in the rendered size */
    SpuRegionPlace(&x_offset, &y_offset,
                   subpic, region);

    /* Save this position for subtitle overlap support
     * it is really important that there are given without scale_size applied */
    *dst_area = spu_area_create(x_offset, y_offset,
                                region->fmt.i_visible_width,
                                region->fmt.i_visible_height,
                                scale_size);

    /* Handle overlapping subtitles when possible */
    if (subpic->b_subtitle && !subpic->b_absolute)
        SpuAreaFixOverlap(dst_area, subtitle_area, subtitle_area_count,
                          region->i_align);

    /* we copy the area: for the subtitle overlap support we want
     * to only save the area without margin applied */
    spu_area_t restrained = *dst_area;

    /* apply margin to subtitles and correct if they go over the picture edge */
    if (subpic->b_subtitle)
        restrained.y -= y_margin;

    spu_area_t display = spu_area_create(0, 0, fmt->i_visible_width,
                                         fmt->i_visible_height,
                                         spu_scale_unit());
    //fprintf("
    SpuAreaFitInside(&restrained, &display);

    /* Fix the position for the current scale_size */
    x_offset = spu_scale_w(restrained.x, restrained.scale);
    y_offset = spu_scale_h(restrained.y, restrained.scale);

    /* */
    if (force_palette) {
        video_palette_t *old_palette = region->fmt.p_palette;
        video_palette_t new_palette;
        bool b_opaque = false;
        bool b_old_opaque = false;

        /* We suppose DVD palette here */
        new_palette.i_entries = 4;
        for (int i = 0; i < 4; i++)
        {
            for (int j = 0; j < 4; j++)
                new_palette.palette[i][j] = sys->palette[i][j];
            b_opaque |= (new_palette.palette[i][3] > 0x00);
        }

        if (old_palette->i_entries == new_palette.i_entries) {
            for (int i = 0; i < old_palette->i_entries; i++)
            {
                for (int j = 0; j < 4; j++)
                    changed_palette |= old_palette->palette[i][j] != new_palette.palette[i][j];
                b_old_opaque |= (old_palette->palette[i][3] > 0x00);
            }
        } else {
            changed_palette = true;
            b_old_opaque = true;
        }

        /* Reject or patch fully transparent broken palette used for dvd menus */
        if( !b_opaque )
        {
            if( !b_old_opaque )
            {
                /* replace with new one and fixed alpha */
                old_palette->palette[1][3] = 0x80;
                old_palette->palette[2][3] = 0x80;
                old_palette->palette[3][3] = 0x80;
            }
            /* keep old visible palette */
            else changed_palette = false;
        }

        if( changed_palette )
            *old_palette = new_palette;
    }

    /* */
    region_fmt = region->fmt;
    region_picture = region->p_picture;

    bool convert_chroma = true;
    for (int i = 0; chroma_list[i] && convert_chroma; i++) {
        if (region_fmt.i_chroma == chroma_list[i])
            convert_chroma = false;
    }

    /* Scale from rendered size to destination size */
    if (sys->scale && sys->scale->p_module &&
        (!using_palette || (sys->scale_yuvp && sys->scale_yuvp->p_module)) &&
        (scale_size.w != SCALE_UNIT || scale_size.h != SCALE_UNIT ||
        using_palette || convert_chroma)) {
        const unsigned dst_width  = spu_scale_w(region->fmt.i_visible_width,  scale_size);
        const unsigned dst_height = spu_scale_h(region->fmt.i_visible_height, scale_size);

        /* Destroy the cache if unusable */
        if (region->p_private) {
            subpicture_region_private_t *private = region->p_private;
            bool is_changed = false;

            /* Check resize changes */
            if (dst_width  != private->fmt.i_visible_width ||
                dst_height != private->fmt.i_visible_height)
                is_changed = true;

            /* Check forced palette changes */
            if (changed_palette)
                is_changed = true;

            if (convert_chroma && private->fmt.i_chroma != chroma_list[0])
                is_changed = true;

            if (is_changed) {
                subpicture_region_private_Delete(private);
                region->p_private = NULL;
            }
        }

        /* Scale if needed into cache */
        if (!region->p_private && dst_width > 0 && dst_height > 0) {
            filter_t *scale = sys->scale;

            picture_t *picture = region->p_picture;
            picture_Hold(picture);

            /* Convert YUVP to YUVA/RGBA first for better scaling quality */
            if (using_palette) {
                filter_t *scale_yuvp = sys->scale_yuvp;

                scale_yuvp->fmt_in.video = region->fmt;

                scale_yuvp->fmt_out.video = region->fmt;
                scale_yuvp->fmt_out.video.i_chroma = chroma_list[0];

                picture = scale_yuvp->pf_video_filter(scale_yuvp, picture);
                if (!picture) {
                    /* Well we will try conversion+scaling */
                    msg_Warn(spu, "%4.4s to %4.4s conversion failed",
                             (const char*)&scale_yuvp->fmt_in.video.i_chroma,
                             (const char*)&scale_yuvp->fmt_out.video.i_chroma);
                }
            }

            /* Conversion(except from YUVP)/Scaling */
            if (picture &&
                (picture->format.i_visible_width  != dst_width ||
                 picture->format.i_visible_height != dst_height ||
                 (convert_chroma && !using_palette)))
            {
                scale->fmt_in.video  = picture->format;
                scale->fmt_out.video = picture->format;
                if (using_palette)
                    scale->fmt_in.video.i_chroma = chroma_list[0];
                if (convert_chroma)
                    scale->fmt_out.i_codec        =
                    scale->fmt_out.video.i_chroma = chroma_list[0];

                scale->fmt_out.video.i_width  = dst_width;
                scale->fmt_out.video.i_height = dst_height;

                scale->fmt_out.video.i_visible_width =
                    spu_scale_w(region->fmt.i_visible_width, scale_size);
                scale->fmt_out.video.i_visible_height =
                    spu_scale_h(region->fmt.i_visible_height, scale_size);

                picture = scale->pf_video_filter(scale, picture);
                if (!picture)
                    msg_Err(spu, "scaling failed");
            }

            /* */
            if (picture) {
                region->p_private = subpicture_region_private_New(&picture->format);
                if (region->p_private) {
                    region->p_private->p_picture = picture;
                    if (!region->p_private->p_picture) {
                        subpicture_region_private_Delete(region->p_private);
                        region->p_private = NULL;
                    }
                } else {
                    picture_Release(picture);
                }
            }
        }

        /* And use the scaled picture */
        if (region->p_private) {
            region_fmt     = region->p_private->fmt;
            region_picture = region->p_private->p_picture;
        }
    }

    /* Force cropping if requested */
    if (crop_requested) {
        int crop_x, crop_y, crop_width, crop_height;
        if(sys->force_crop){
            crop_x     = spu_scale_w(sys->crop.x, scale_size);
            crop_y     = spu_scale_h(sys->crop.y, scale_size);
            crop_width = spu_scale_w(sys->crop.width,  scale_size);
            crop_height= spu_scale_h(sys->crop.height, scale_size);
        }
        else
        {
            crop_x = x_offset;
            crop_y = y_offset;
            crop_width = region_fmt.i_visible_width;
            crop_height = region_fmt.i_visible_height;
        }

        if(region->i_max_width && spu_scale_w(region->i_max_width, scale_size) < crop_width)
            crop_width = spu_scale_w(region->i_max_width, scale_size);

        if(region->i_max_height && spu_scale_h(region->i_max_height, scale_size) < crop_height)
            crop_height = spu_scale_h(region->i_max_height, scale_size);

        /* Find the intersection */
        if (crop_x + crop_width <= x_offset ||
            x_offset + (int)region_fmt.i_visible_width  < crop_x ||
            crop_y + crop_height <= y_offset ||
            y_offset + (int)region_fmt.i_visible_height < crop_y) {
            /* No intersection */
            region_fmt.i_visible_width  =
            region_fmt.i_visible_height = 0;
        } else {
            int x, y, x_end, y_end;
            x = __MAX(crop_x, x_offset);
            y = __MAX(crop_y, y_offset);
            x_end = __MIN(crop_x + crop_width,
                          x_offset + (int)region_fmt.i_visible_width);
            y_end = __MIN(crop_y + crop_height,
                          y_offset + (int)region_fmt.i_visible_height);

            region_fmt.i_x_offset       = x - x_offset;
            region_fmt.i_y_offset       = y - y_offset;
            region_fmt.i_visible_width  = x_end - x;
            region_fmt.i_visible_height = y_end - y;

            x_offset = __MAX(x, 0);
            y_offset = __MAX(y, 0);
        }
    }

    subpicture_region_t *dst = *dst_ptr = subpicture_region_New(&region_fmt);
    if (dst) {
        dst->i_x       = x_offset;
        dst->i_y       = y_offset;
        dst->i_align   = 0;
        if (dst->p_picture)
            picture_Release(dst->p_picture);
        dst->p_picture = picture_Hold(region_picture);
        int fade_alpha = 255;
        if (subpic->b_fade) {
            vlc_tick_t fade_start = subpic->i_start + 3 * (subpic->i_stop - subpic->i_start) / 4;

            if (fade_start <= render_date && fade_start < subpic->i_stop)
                fade_alpha = 255 * (subpic->i_stop - render_date) /
                                   (subpic->i_stop - fade_start);
        }
        dst->i_alpha   = fade_alpha * subpic->i_alpha * region->i_alpha / 65025;
    }

exit:
    if (restore_text) {
        /* Some forms of subtitles need to be re-rendered more than
         * once, eg. karaoke. We therefore restore the region to its
         * pre-rendered state, so the next time through everything is
         * calculated again.
         */
        if (region->p_picture) {
            picture_Release(region->p_picture);
            region->p_picture = NULL;
        }
        if (region->p_private) {
            subpicture_region_private_Delete(region->p_private);
            region->p_private = NULL;
        }
        region->fmt = fmt_original;
    }
}

/**
 * This function renders all sub picture units in the list.
 */
static subpicture_t *SpuRenderSubpictures(spu_t *spu,
                                          unsigned int i_subpicture,
                                          subpicture_t **pp_subpicture,
                                          const vlc_fourcc_t *chroma_list,
                                          const video_format_t *fmt_dst,
                                          const video_format_t *fmt_src,
                                          vlc_tick_t render_subtitle_date,
                                          vlc_tick_t render_osd_date)
{
    spu_private_t *sys = spu->p;

    /* Count the number of regions and subtitle regions */
    unsigned int subtitle_region_count = 0;
    unsigned int region_count          = 0;
    for (unsigned i = 0; i < i_subpicture; i++) {
        const subpicture_t *subpic = pp_subpicture[i];

        unsigned count = 0;
        for (subpicture_region_t *r = subpic->p_region; r != NULL; r = r->p_next)
            count++;

        if (subpic->b_subtitle)
            subtitle_region_count += count;
        region_count += count;
    }
    if (region_count <= 0)
    {
        /* 当前没有可显示的字幕：若外部正在接管，通知其清空画面 */
        SpuBitmapBridgeNotify( spu, NULL );
        return NULL;
    }

    /* 位图字幕是否由外部接管。spu_Render() 是逐帧调用的，接管模式下
     * 同一句字幕只需渲染/回传一次；不去重会每帧重跑 SpuRenderRegion
     * （YUVP->RGBA 转换 + 缩放），4K/PGS 实测单次 70~160ms。 */
    bool b_bitmap_takeover = false;
    vlc_mutex_lock( &spu_user_params.lock );
    b_bitmap_takeover = spu_user_params.pf_bitmap != NULL
                     && spu_user_params.b_bitmap_only;
    vlc_mutex_unlock( &spu_user_params.lock );
    bool b_skipped_subtitle = false;

    /* Create the output subpicture */
    subpicture_t *output = subpicture_New(NULL);
    if (!output)
        return NULL;
    output->i_order = pp_subpicture[i_subpicture - 1]->i_order;
    output->i_original_picture_width  = fmt_dst->i_visible_width;
    output->i_original_picture_height = fmt_dst->i_visible_height;
    subpicture_region_t **output_last_ptr = &output->p_region;

    /* Allocate area array for subtitle overlap */
    spu_area_t subtitle_area_buffer[VOUT_MAX_SUBPICTURES];
    spu_area_t *subtitle_area;
    int subtitle_area_count;

    subtitle_area_count = 0;
    subtitle_area = subtitle_area_buffer;
    if (subtitle_region_count > sizeof(subtitle_area_buffer)/sizeof(*subtitle_area_buffer))
        subtitle_area = calloc(subtitle_region_count, sizeof(*subtitle_area));

    /* Process all subpictures and regions (in the right order) */
    for (unsigned int index = 0; index < i_subpicture; index++) {
        subpicture_t        *subpic = pp_subpicture[index];
        subpicture_region_t *region;

        if (!subpic->p_region)
            continue;

        /* 接管模式：与上次回传的是同一句字幕 ⇒ 整条跳过渲染。
         * 外部（App）已持有上一帧的位图，跳过是安全的；但必须记住
         * "本帧是因去重跳过"，末尾绝不能因此补发 CLEAR。 */
        if (subpic->b_subtitle && b_bitmap_takeover)
        {
            bool b_same = false;
            vlc_mutex_lock( &spu_user_params.lock );
            b_same = spu_user_params.b_has_last
                  && spu_user_params.i_last_order == subpic->i_order
                  && spu_user_params.i_last_start == subpic->i_start
                  && spu_user_params.i_last_stop  == subpic->i_stop;
            vlc_mutex_unlock( &spu_user_params.lock );
            if (b_same)
            {
                b_skipped_subtitle = true;
                continue;
            }
        }

        if (subpic->i_original_picture_width  <= 0 ||
            subpic->i_original_picture_height <= 0) {
            if (subpic->i_original_picture_width  > 0 ||
                subpic->i_original_picture_height > 0)
                msg_Err(spu, "original picture size %dx%d is unsupported",
                         subpic->i_original_picture_width,
                         subpic->i_original_picture_height);
            else
                msg_Warn(spu, "original picture size is undefined");

            subpic->i_original_picture_width  = fmt_src->i_visible_width;
            subpic->i_original_picture_height = fmt_src->i_visible_height;
        }

        /* 接管模式不做缩放（见下方 scale = spu_scale_unit()），坐标与尺寸
         * 都落在"字幕原始分辨率"坐标系：回传给 App 的基准尺寸、以及用于
         * 去重的字幕身份都必须取当前 subpic，不能用 fmt_dst（4K）或
         * subpicture_New 出来的默认值。 */
        if (subpic->b_subtitle && b_bitmap_takeover)
        {
            output->i_order = subpic->i_order;
            output->i_start = subpic->i_start;
            output->i_stop  = subpic->i_stop;
            output->i_original_picture_width  = subpic->i_original_picture_width;
            output->i_original_picture_height = subpic->i_original_picture_height;
        }

        /* 文本字幕只回调不上屏（外部自绘）：跳过纯文本 subpicture */
        if (subpic->b_subtitle && spu_text_bridge.b_listener_only) {
            bool b_all_text = true;
            for (subpicture_region_t *r = subpic->p_region; r; r = r->p_next)
                if (!r->p_text) { b_all_text = false; break; }
            if (b_all_text)
                continue;
        }

        if (sys->text) {
            /* FIXME aspect ratio ? */
            sys->text->fmt_out.video.i_width          =
            sys->text->fmt_out.video.i_visible_width  = subpic->i_original_picture_width;

            sys->text->fmt_out.video.i_height         =
            sys->text->fmt_out.video.i_visible_height = subpic->i_original_picture_height;
        }

        /* Render all regions
         * We always transform non absolute subtitle into absolute one on the
         * first rendering to allow good subtitle overlap support.
         */
        for (region = subpic->p_region; region != NULL; region = region->p_next) {
            spu_area_t area;

            /* Compute region scale AR */
            video_format_t region_fmt = region->fmt;
            if (region_fmt.i_sar_num <= 0 || region_fmt.i_sar_den <= 0) {

                const uint64_t i_sar_num = (uint64_t)fmt_dst->i_visible_width  *
                                           fmt_dst->i_sar_num * subpic->i_original_picture_height;
                const uint64_t i_sar_den = (uint64_t)fmt_dst->i_visible_height *
                                           fmt_dst->i_sar_den * subpic->i_original_picture_width;

                vlc_ureduce(&region_fmt.i_sar_num, &region_fmt.i_sar_den,
                            i_sar_num, i_sar_den, 65536);
            }

            /* Compute scaling from original size to destination size
             * FIXME The current scaling ensure that the heights match, the width being
             * cropped.
             */
            spu_scale_t scale = spu_scale_createq((int64_t)fmt_dst->i_visible_height                 * fmt_dst->i_sar_den * region_fmt.i_sar_num,
                                                  (int64_t)subpic->i_original_picture_height * fmt_dst->i_sar_num * region_fmt.i_sar_den,
                                                  fmt_dst->i_visible_height,
                                                  subpic->i_original_picture_height);

            /* PGS 原生多为 1080p，视频可能是 4K，上面算出的 scale 就是 2 倍
             * 放大。SpuRenderRegion 走通用 swscale，实测单条字幕 70~160ms，
             * 直接把 vout 线程拖成 "picture is too late"。接管模式由 App 的
             * ImageView 用 GPU 放大，core 不做缩放，只保留必要的色度转换。 */
            if (subpic->b_subtitle && b_bitmap_takeover)
                scale = spu_scale_unit();

            /* Check scale validity */
            if (scale.w <= 0 || scale.h <= 0)
                continue;

            /* */
            SpuRenderRegion(spu, output_last_ptr, &area,
                            subpic, region, scale,
                            chroma_list, fmt_dst,
                            subtitle_area, subtitle_area_count,
                            subpic->b_subtitle ? render_subtitle_date : render_osd_date);
            if (*output_last_ptr)
                output_last_ptr = &(*output_last_ptr)->p_next;

            if (subpic->b_subtitle) {
                area = spu_area_unscaled(area, scale);
                if (!subpic->b_absolute && area.width > 0 && area.height > 0) {
                    region->i_x = area.x;
                    region->i_y = area.y;
                }
                if (subtitle_area)
                    subtitle_area[subtitle_area_count++] = area;
            }
        }
        if (subpic->b_subtitle && subpic->p_region)
            subpic->b_absolute = true;
    }

    /* */
    if (subtitle_area != subtitle_area_buffer)
        free(subtitle_area);

    /* 本帧的字幕都因"与上次同一句"被跳过：字幕仍在显示，既不用重绘，
     * 也不能触发 SpuBitmapBridgeNotify 的 CLEAR 分支（否则 App 先隐藏再
     * 重画，表现为闪烁）。同时不能把同帧残留的 OSD region 当作字幕回传，
     * 所以整个 output 直接丢弃。 */
    if (b_skipped_subtitle)
    {
        subpicture_Delete( output );
        return NULL;
    }

    /* 位图字幕接管：把 RGBA region 回传给外部后丢弃，不再上屏。
     * （4K 下这条独立 RGBA Surface 是掉帧的主要来源，接管后由 App 用
     *  ImageView 画在 UI 层，内核不再参与合成。） */
    if ( SpuBitmapBridgeNotify( spu, output ) )
    {
        subpicture_Delete( output );
        return NULL;
    }

    return output;
}

/*****************************************************************************
 * Object variables callbacks
 *****************************************************************************/

/*****************************************************************************
 * UpdateSPU: update subpicture settings
 *****************************************************************************
 * This function is called from CropCallback and at initialization time, to
 * retrieve crop information from the input.
 *****************************************************************************/
static void UpdateSPU(spu_t *spu, vlc_object_t *object)
{
    spu_private_t *sys = spu->p;
    vlc_value_t val;

    vlc_mutex_lock(&sys->lock);

    sys->force_palette = false;
    sys->force_crop = false;

    if (var_Get(object, "highlight", &val) || !val.b_bool) {
        vlc_mutex_unlock(&sys->lock);
        return;
    }

    sys->force_crop = true;
    sys->crop.x      = var_GetInteger(object, "x-start");
    sys->crop.y      = var_GetInteger(object, "y-start");
    sys->crop.width  = var_GetInteger(object, "x-end") - sys->crop.x;
    sys->crop.height = var_GetInteger(object, "y-end") - sys->crop.y;

    if (var_Get(object, "menu-palette", &val) == VLC_SUCCESS) {
        memcpy(sys->palette, val.p_address, 16);
        sys->force_palette = true;
    }
    vlc_mutex_unlock(&sys->lock);

    msg_Dbg(object, "crop: %i,%i,%i,%i, palette forced: %i",
            sys->crop.x, sys->crop.y,
            sys->crop.width, sys->crop.height,
            sys->force_palette);
}

/*****************************************************************************
 * CropCallback: called when the highlight properties are changed
 *****************************************************************************
 * This callback is called from the input thread when we need cropping
 *****************************************************************************/
static int CropCallback(vlc_object_t *object, char const *var,
                        vlc_value_t oldval, vlc_value_t newval, void *data)
{
    VLC_UNUSED(oldval); VLC_UNUSED(newval); VLC_UNUSED(var);

    UpdateSPU((spu_t *)data, object);
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Buffers allocation callbacks for the filters
 *****************************************************************************/

static subpicture_t *sub_new_buffer(filter_t *filter)
{
    int channel = (intptr_t)filter->owner.sys;

    subpicture_t *subpicture = subpicture_New(NULL);
    if (subpicture)
        subpicture->i_channel = channel;
    return subpicture;
}

static int SubSourceInit(filter_t *filter, void *data)
{
    spu_t *spu = data;
    int channel = spu_RegisterChannel(spu);

    filter->owner.sys = (void *)(intptr_t)channel;
    filter->owner.sub.buffer_new = sub_new_buffer;
    return VLC_SUCCESS;
}

static int SubSourceClean(filter_t *filter, void *data)
{
    spu_t *spu = data;
    int channel = (intptr_t)filter->owner.sys;

    spu_ClearChannel(spu, channel);
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Proxy callbacks
 *****************************************************************************/

static int RestartSubFilterCallback(vlc_object_t *obj, char const *psz_var,
                                    vlc_value_t oldval, vlc_value_t newval,
                                    void *p_data)
{ VLC_UNUSED(obj); VLC_UNUSED(psz_var); VLC_UNUSED(oldval); VLC_UNUSED(newval);
    vout_ControlChangeSubFilters((vout_thread_t *)p_data, NULL);
    return VLC_SUCCESS;
}

static int SubFilterAddProxyCallbacks(filter_t *filter, void *opaque)
{
    filter_AddProxyCallbacks((vlc_object_t *)opaque, filter,
                             RestartSubFilterCallback);
    return VLC_SUCCESS;
}

static int SubFilterDelProxyCallbacks(filter_t *filter, void *opaque)
{
    filter_DelProxyCallbacks((vlc_object_t *)opaque, filter,
                             RestartSubFilterCallback);
    return VLC_SUCCESS;
}

static int RestartSubSourceCallback(vlc_object_t *obj, char const *psz_var,
                                    vlc_value_t oldval, vlc_value_t newval,
                                    void *p_data)
{ VLC_UNUSED(obj); VLC_UNUSED(psz_var); VLC_UNUSED(oldval); VLC_UNUSED(newval);
    vout_ControlChangeSubSources((vout_thread_t *)p_data, NULL);
    return VLC_SUCCESS;
}

static int SubSourceAddProxyCallbacks(filter_t *filter, void *opaque)
{
    filter_AddProxyCallbacks((vlc_object_t *)opaque, filter,
                             RestartSubSourceCallback);
    return VLC_SUCCESS;
}

static int SubSourceDelProxyCallbacks(filter_t *filter, void *opaque)
{
    filter_DelProxyCallbacks((vlc_object_t *)opaque, filter,
                             RestartSubSourceCallback);
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Public API
 *****************************************************************************/

#undef spu_Create
/**
 * Creates the subpicture unit
 *
 * \param p_this the parent object which creates the subpicture unit
 */
spu_t *spu_Create(vlc_object_t *object, vout_thread_t *vout)
{
    spu_t *spu = vlc_custom_create(object,
                                   sizeof(spu_t) + sizeof(spu_private_t),
                                   "subpicture");
    if (!spu)
        return NULL;

    /* Initialize spu fields */
    spu_private_t *sys = spu->p = (spu_private_t*)&spu[1];

    /* Initialize private fields */
    vlc_mutex_init(&sys->lock);

    SpuHeapInit(&sys->heap);

    sys->text = NULL;
    sys->scale = NULL;
    sys->scale_yuvp = NULL;

    sys->margin = var_InheritInteger(spu, "sub-margin");

    /* Register the default subpicture channel */
    sys->channel = VOUT_SPU_CHANNEL_AVAIL_FIRST;

    sys->source_chain_update = NULL;
    sys->filter_chain_update = NULL;
    vlc_mutex_init(&sys->source_chain_lock);
    vlc_mutex_init(&sys->filter_chain_lock);
    sys->source_chain = filter_chain_New(spu, "sub source", SPU_ES);
    sys->filter_chain = filter_chain_New(spu, "sub filter", SPU_ES);

    /* Load text and scale module（listener-only 时 text 为 NULL，见函数注释） */
    sys->text = SpuRenderCreateAndLoadTextIfNeeded(spu);

    /* XXX spu->p_scale is used for all conversion/scaling except yuvp to
     * yuva/rgba */
    sys->scale = SpuRenderCreateAndLoadScale(VLC_OBJECT(spu),
                                             VLC_CODEC_YUVA, VLC_CODEC_RGBA, true);

    /* This one is used for YUVP to YUVA/RGBA without scaling
     * FIXME rename it */
    sys->scale_yuvp = SpuRenderCreateAndLoadScale(VLC_OBJECT(spu),
                                                  VLC_CODEC_YUVP, VLC_CODEC_YUVA, false);

    /* */
    sys->last_sort_date = -1;
    sys->vout = vout;

    return spu;
}

/**
 * Destroy the subpicture unit
 *
 * \param p_this the parent object which destroys the subpicture unit
 */
void spu_Destroy(spu_t *spu)
{
    spu_private_t *sys = spu->p;

    if (sys->text)
        FilterRelease(sys->text);

    if (sys->scale_yuvp)
        FilterRelease(sys->scale_yuvp);

    if (sys->scale)
        FilterRelease(sys->scale);

    filter_chain_ForEach(sys->source_chain, SubSourceClean, spu);
    if (sys->vout)
        filter_chain_ForEach(sys->source_chain,
                             SubSourceDelProxyCallbacks, sys->vout);
    filter_chain_Delete(sys->source_chain);
    free(sys->source_chain_current);
    if (sys->vout)
        filter_chain_ForEach(sys->filter_chain,
                             SubFilterDelProxyCallbacks, sys->vout);
    filter_chain_Delete(sys->filter_chain);
    free(sys->filter_chain_current);
    vlc_mutex_destroy(&sys->source_chain_lock);
    vlc_mutex_destroy(&sys->filter_chain_lock);
    free(sys->source_chain_update);
    free(sys->filter_chain_update);

    /* Destroy all remaining subpictures */
    SpuHeapClean(&sys->heap);

    vlc_mutex_destroy(&sys->lock);

    vlc_object_release(spu);
}

/**
 * Attach/Detach the SPU from any input
 *
 * \param p_this the object in which to destroy the subpicture unit
 * \param b_attach to select attach or detach
 */
void spu_Attach(spu_t *spu, vlc_object_t *input, bool attach)
{
    if (attach) {
        UpdateSPU(spu, input);
        var_Create(input, "highlight", VLC_VAR_BOOL);
        var_AddCallback(input, "highlight", CropCallback, spu);

        vlc_mutex_lock(&spu->p->lock);
        spu->p->input = input;

        if (spu->p->text)
            FilterRelease(spu->p->text);
        spu->p->text = SpuRenderCreateAndLoadTextIfNeeded(spu);

        vlc_mutex_unlock(&spu->p->lock);
    } else {
        vlc_mutex_lock(&spu->p->lock);
        spu->p->input = NULL;
        vlc_mutex_unlock(&spu->p->lock);

        /* Delete callbacks */
        var_DelCallback(input, "highlight", CropCallback, spu);
        var_Destroy(input, "highlight");
    }
}

/**
 * Inform the SPU filters of mouse event
 */
int spu_ProcessMouse(spu_t *spu,
                     const vlc_mouse_t *mouse,
                     const video_format_t *fmt)
{
    spu_private_t *sys = spu->p;

    vlc_mutex_lock(&sys->source_chain_lock);
    filter_chain_MouseEvent(sys->source_chain, mouse, fmt);
    vlc_mutex_unlock(&sys->source_chain_lock);

    return VLC_SUCCESS;
}

/**
 * Display a subpicture
 *
 * Remove the reservation flag of a subpicture, which will cause it to be
 * ready for display.
 * \param spu the subpicture unit object
 * \param subpic the subpicture to display
 */
void spu_PutSubpicture(spu_t *spu, subpicture_t *subpic)
{
    spu_private_t *sys = spu->p;

    /* Update sub-filter chain */
    vlc_mutex_lock(&sys->lock);
    char *chain_update = sys->filter_chain_update;
    sys->filter_chain_update = NULL;
    vlc_mutex_unlock(&sys->lock);

    bool is_left_empty = false;

    vlc_mutex_lock(&sys->filter_chain_lock);
    if (chain_update) {
        if (*chain_update) {
            if (sys->vout)
                filter_chain_ForEach(sys->filter_chain,
                                     SubFilterDelProxyCallbacks,
                                     sys->vout);
            filter_chain_Reset(sys->filter_chain, NULL, NULL);

            filter_chain_AppendFromString(spu->p->filter_chain, chain_update);
            if (sys->vout)
                filter_chain_ForEach(sys->filter_chain,
                                     SubFilterAddProxyCallbacks,
                                     sys->vout);
        }
        else
            filter_chain_Reset(sys->filter_chain, NULL, NULL);

        /* "sub-source"  was formerly "sub-filter", so now the "sub-filter"
        configuration may contain sub-filters or sub-sources configurations.
        if the filters chain was left empty it may indicate that it's a sub-source configuration */
        is_left_empty = filter_chain_IsEmpty(spu->p->filter_chain);
    }
    vlc_mutex_unlock(&sys->filter_chain_lock);

    if (is_left_empty) {
        /* try to use the configuration as a sub-source configuration,
           but only if there is no 'source_chain_update' value and
           if only if 'chain_update' has a value */
        if (chain_update && *chain_update) {
            vlc_mutex_lock(&sys->lock);
            if (!sys->source_chain_update || !*sys->source_chain_update) {
                if (sys->source_chain_update)
                    free(sys->source_chain_update);
                sys->source_chain_update = chain_update;
                sys->source_chain_current = strdup(chain_update);
                chain_update = NULL;
            }
            vlc_mutex_unlock(&sys->lock);
        }
    }

    free(chain_update);

    /* Run filter chain on the new subpicture */
    vlc_mutex_lock(&sys->filter_chain_lock);
    subpic = filter_chain_SubFilter(spu->p->filter_chain, subpic);
    vlc_mutex_unlock(&sys->filter_chain_lock);
    if (!subpic)
        return;

    /* SPU_DEFAULT_CHANNEL always reset itself */
    if (subpic->i_channel == VOUT_SPU_CHANNEL_OSD)
        spu_ClearChannel(spu, VOUT_SPU_CHANNEL_OSD);

    /* p_private is for spu only and cannot be non NULL here */
    for (subpicture_region_t *r = subpic->p_region; r != NULL; r = r->p_next)
        assert(r->p_private == NULL);

    /* */
    vlc_mutex_lock(&sys->lock);
    if (SpuHeapPush(&sys->heap, subpic)) {
        vlc_mutex_unlock(&sys->lock);
        msg_Err(spu, "subpicture heap full");
        subpicture_Delete(subpic);
        return;
    }
    vlc_mutex_unlock(&sys->lock);
}

subpicture_t *spu_Render(spu_t *spu,
                         const vlc_fourcc_t *chroma_list,
                         const video_format_t *fmt_dst,
                         const video_format_t *fmt_src,
                         vlc_tick_t render_subtitle_date,
                         vlc_tick_t render_osd_date,
                         bool ignore_osd)
{
    spu_private_t *sys = spu->p;

    /* Update sub-source chain */
    vlc_mutex_lock(&sys->lock);
    char *chain_update = sys->source_chain_update;
    sys->source_chain_update = NULL;
    vlc_mutex_unlock(&sys->lock);

    vlc_mutex_lock(&sys->source_chain_lock);
    if (chain_update) {
        filter_chain_ForEach(sys->source_chain, SubSourceClean, spu);
            if (sys->vout)
                filter_chain_ForEach(sys->source_chain,
                                     SubSourceDelProxyCallbacks,
                                     sys->vout);
        filter_chain_Reset(sys->source_chain, NULL, NULL);

        filter_chain_AppendFromString(spu->p->source_chain, chain_update);
        if (sys->vout)
            filter_chain_ForEach(sys->source_chain,
                                 SubSourceAddProxyCallbacks, sys->vout);
        filter_chain_ForEach(sys->source_chain, SubSourceInit, spu);

        free(chain_update);
    }
    /* Run subpicture sources */
    filter_chain_SubSource(sys->source_chain, spu, render_osd_date);
    vlc_mutex_unlock(&sys->source_chain_lock);

    static const vlc_fourcc_t chroma_list_default_yuv[] = {
        VLC_CODEC_YUVA,
        VLC_CODEC_RGBA,
        VLC_CODEC_ARGB,
        VLC_CODEC_BGRA,
        VLC_CODEC_YUVP,
        0,
    };
    static const vlc_fourcc_t chroma_list_default_rgb[] = {
        VLC_CODEC_RGBA,
        VLC_CODEC_ARGB,
        VLC_CODEC_BGRA,
        VLC_CODEC_YUVA,
        VLC_CODEC_YUVP,
        0,
    };

    if (!chroma_list || *chroma_list == 0)
        chroma_list = vlc_fourcc_IsYUV(fmt_dst->i_chroma) ? chroma_list_default_yuv
                                                          : chroma_list_default_rgb;

    vlc_mutex_lock(&sys->lock);

    unsigned int subpicture_count;
    subpicture_t *subpicture_array[VOUT_MAX_SUBPICTURES];

    /* Get an array of subpictures to render */
    SpuSelectSubpictures(spu, &subpicture_count, subpicture_array,
                         render_subtitle_date, render_osd_date, ignore_osd);

    /* 文本字幕桥：把当前应显示的文本回调给外部监听者（无字幕时通知清空） */
#ifdef __ANDROID__
    /* VLCSpuTrace: 跟踪当前被选中显示的字幕 subpicture（指针变化时打印） */
    {
        static subpicture_t *s_last_traced = NULL;
        subpicture_t *p_traced = NULL;
        for ( unsigned i = 0; i < subpicture_count && !p_traced; i++ )
            if ( subpicture_array[i]->b_subtitle )
                p_traced = subpicture_array[i];
        if ( p_traced != s_last_traced )
        {
            s_last_traced = p_traced;
            if ( p_traced )
            {
                int i_regions = 0;
                bool b_text = false;
                for ( subpicture_region_t *r = p_traced->p_region; r; r = r->p_next )
                {
                    i_regions++;
                    if ( r->p_text )
                        b_text = true;
                }
                __android_log_print( ANDROID_LOG_INFO, "VLCSpuTrace",
                                     "select: subpic=%p order=%lld [%lld..%lld] regions=%d text=%d",
                                     (void *)p_traced,
                                     (long long)p_traced->i_order,
                                     (long long)p_traced->i_start,
                                     (long long)p_traced->i_stop,
                                     i_regions, b_text );
            }
            else
            {
                __android_log_print( ANDROID_LOG_INFO, "VLCSpuTrace", "select: none" );
            }
        }
    }
#endif
    {
        vlc_mutex_lock(&spu_text_bridge.lock);
        bool b_bridge = spu_text_bridge.pf_text != NULL;
        vlc_mutex_unlock(&spu_text_bridge.lock);
        if (b_bridge) {
            subpicture_t *p_text_sub = NULL;
            for (unsigned i = 0; i < subpicture_count && !p_text_sub; i++)
                if (subpicture_array[i]->b_subtitle)
                    p_text_sub = subpicture_array[i];
            if (p_text_sub) {
                char *psz = spu_text_bridge_text(p_text_sub);
                spu_text_bridge_update(psz, p_text_sub->i_start, p_text_sub->i_stop);
                free(psz);
            } else {
                spu_text_bridge_update("", 0, 0);
            }
        }
    }

    if (subpicture_count <= 0) {
        vlc_mutex_unlock(&sys->lock);
        /* 队列为空也必须通知接管方清屏：关闭字幕轨 / 空轨 / 新轨首句未到时，
         * 走不到 SpuRenderSubpictures() 里的 SpuBitmapBridgeNotify，外部
         * ImageView 会一直显示上一句字幕（切轨/切集残留的内核侧根因）。
         * b_has_last 保证只补发一次 CLEAR。 */
        SpuBitmapBridgeNotify(spu, NULL);
        return NULL;
    }

    /* Updates the subpictures */
    for (unsigned i = 0; i < subpicture_count; i++) {
        subpicture_t *subpic = subpicture_array[i];
        subpicture_Update(subpic,
                          fmt_src, fmt_dst,
                          subpic->b_subtitle ? render_subtitle_date : render_osd_date);
    }

    /* Now order the subpicture array
     * XXX The order is *really* important for overlap subtitles positionning */
    qsort(subpicture_array, subpicture_count, sizeof(*subpicture_array), SubpictureCmp);

    /* Render the subpictures */
    subpicture_t *render = SpuRenderSubpictures(spu,
                                                subpicture_count, subpicture_array,
                                                chroma_list,
                                                fmt_dst,
                                                fmt_src,
                                                render_subtitle_date,
                                                render_osd_date);
    vlc_mutex_unlock(&sys->lock);

    return render;
}

void spu_OffsetSubtitleDate(spu_t *spu, vlc_tick_t duration)
{
    spu_private_t *sys = spu->p;

    vlc_mutex_lock(&sys->lock);
    for (int i = 0; i < VOUT_MAX_SUBPICTURES; i++) {
        spu_heap_entry_t *entry = &sys->heap.entry[i];
        subpicture_t *current = entry->subpicture;

        if (current && current->b_subtitle) {
            if (current->i_start > 0)
                current->i_start += duration;
            if (current->i_stop > 0)
                current->i_stop  += duration;
        }
    }
    vlc_mutex_unlock(&sys->lock);
}

int spu_RegisterChannel(spu_t *spu)
{
    spu_private_t *sys = spu->p;

    vlc_mutex_lock(&sys->lock);
    int channel = sys->channel++;
    vlc_mutex_unlock(&sys->lock);

    return channel;
}

void spu_ClearChannel(spu_t *spu, int channel)
{
    spu_private_t *sys = spu->p;

    vlc_mutex_lock(&sys->lock);

    for (int i = 0; i < VOUT_MAX_SUBPICTURES; i++) {
        spu_heap_entry_t *entry = &sys->heap.entry[i];
        subpicture_t *subpic = entry->subpicture;

        if (!subpic)
            continue;
        if (subpic->i_channel != channel && (channel != -1 || subpic->i_channel == VOUT_SPU_CHANNEL_OSD))
            continue;

        /* You cannot delete subpicture outside of SpuSelectSubpictures */
        entry->reject = true;
    }

    vlc_mutex_unlock(&sys->lock);
}

void spu_ChangeSources(spu_t *spu, const char *filters)
{
    spu_private_t *sys = spu->p;

    vlc_mutex_lock(&sys->lock);

    free(sys->source_chain_update);
    if (filters)
    {
        sys->source_chain_update = strdup(filters);
        free(sys->source_chain_current);
        sys->source_chain_current = strdup(filters);
    }
    else if (sys->source_chain_current)
        sys->source_chain_update = strdup(sys->source_chain_current);

    vlc_mutex_unlock(&sys->lock);
}

void spu_ChangeFilters(spu_t *spu, const char *filters)
{
    spu_private_t *sys = spu->p;

    vlc_mutex_lock(&sys->lock);

    free(sys->filter_chain_update);
    if (filters)
    {
        sys->filter_chain_update = strdup(filters);
        free(sys->filter_chain_current);
        sys->filter_chain_current = strdup(filters);
    }
    else if (sys->filter_chain_current)
        sys->filter_chain_update = strdup(sys->filter_chain_current);

    vlc_mutex_unlock(&sys->lock);
}

void spu_ChangeMargin(spu_t *spu, int margin)
{
    spu_private_t *sys = spu->p;

    vlc_mutex_lock(&sys->lock);
    sys->margin = margin;
    vlc_mutex_unlock(&sys->lock);
}

