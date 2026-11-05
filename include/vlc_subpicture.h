/*****************************************************************************
 * vlc_subpicture.h: subpicture definitions
 *****************************************************************************
 * Copyright (C) 1999 - 2009 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Vincent Seguin <seguin@via.ecp.fr>
 *          Samuel Hocevar <sam@via.ecp.fr>
 *          Olivier Aubert <oaubert 47 videolan d07 org>
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

#ifndef VLC_SUBPICTURE_H
#define VLC_SUBPICTURE_H 1

/**
 */

#include <vlc_picture.h>
#include <vlc_text_style.h>

/**
 * \defgroup subpicture Video sub-pictures
 * \ingroup video_output
 * Subpictures are pictures that should be displayed on top of the video, like
 * subtitles and OSD
 * @{
 * \file
 * Subpictures functions
 */

/**
 * Video subtitle region spu core private
 */
typedef struct subpicture_region_private_t subpicture_region_private_t;

/**
 * Video subtitle region
 *
 * A subtitle region is defined by a picture (graphic) and its rendering
 * coordinates.
 * Subtitles contain a list of regions.
 */
struct subpicture_region_t
{
    video_format_t  fmt;                          /**< format of the picture */
    picture_t       *p_picture;          /**< picture comprising this region */

    int             i_x;      /**< position of region, relative to alignment */
    int             i_y;      /**< position of region, relative to alignment */
    int             i_align;                  /**< alignment flags of region */
    int             i_alpha;                               /**< transparency */

    /* Parameters for text regions (p_picture to be rendered) */
    text_segment_t  *p_text;         /**< subtitle text, made of a list of segments */
    int             i_text_align;    /**< alignment flags of region content */
    bool            b_noregionbg;    /**< render background under text only */
    bool            b_gridmode;      /** if the decoder sends row/cols based output */
    bool            b_balanced_text; /** try to balance wrapped text lines */
    int             i_max_width;     /** horizontal rendering/cropping target/limit */
    int             i_max_height;    /** vertical rendering/cropping target/limit */

    subpicture_region_t *p_next;                /**< next region in the list */
    subpicture_region_private_t *p_private;  /**< Private data for spu_t *only* */
};

/* Subpicture region position flags */
#define SUBPICTURE_ALIGN_LEFT       0x1
#define SUBPICTURE_ALIGN_RIGHT      0x2
#define SUBPICTURE_ALIGN_TOP        0x4
#define SUBPICTURE_ALIGN_BOTTOM     0x8
#define SUBPICTURE_ALIGN_MASK ( SUBPICTURE_ALIGN_LEFT|SUBPICTURE_ALIGN_RIGHT| \
                                SUBPICTURE_ALIGN_TOP |SUBPICTURE_ALIGN_BOTTOM )
/**
 * This function will create a new subpicture region.
 *
 * You must use subpicture_region_Delete to destroy it.
 */
VLC_API subpicture_region_t * subpicture_region_New( const video_format_t *p_fmt );

/**
 * This function will destroy a subpicture region allocated by
 * subpicture_region_New.
 *
 * You may give it NULL.
 */
VLC_API void subpicture_region_Delete( subpicture_region_t *p_region );

/**
 * This function will destroy a list of subpicture regions allocated by
 * subpicture_region_New.
 *
 * Provided for convenience.
 */
VLC_API void subpicture_region_ChainDelete( subpicture_region_t *p_head );

/**
 * This function will copy a subpicture region to a new allocated one
 * and transfer all the properties
 *
 * Provided for convenience.
 */
VLC_API subpicture_region_t *subpicture_region_Copy( subpicture_region_t *p_region );

/**
 *
 */
typedef struct subpicture_updater_sys_t subpicture_updater_sys_t;
typedef struct
{
    /** Optional pre update callback, usually useful on video format change.
      * Will skip pf_update on VLC_SUCCESS, or will delete every region before
      * the call to pf_update */
    int  (*pf_validate)( subpicture_t *,
                         bool has_src_changed, const video_format_t *p_fmt_src,
                         bool has_dst_changed, const video_format_t *p_fmt_dst,
                         vlc_tick_t);
    /** Mandatory callback called after pf_validate and doing
      * the main job of creating the subpicture regions for the
      * current video_format */
    void (*pf_update)  ( subpicture_t *,
                         const video_format_t *p_fmt_src,
                         const video_format_t *p_fmt_dst,
                         vlc_tick_t );
    /** Optional callback for subpicture private data cleanup */
    void (*pf_destroy) ( subpicture_t * );
    subpicture_updater_sys_t *p_sys;
} subpicture_updater_t;

typedef struct subpicture_private_t subpicture_private_t;

/**
 * Video subtitle
 *
 * Any subtitle destined to be displayed by a video output thread should
 * be stored in this structure from it's creation to it's effective display.
 * Subtitle type and flags should only be modified by the output thread. Note
 * that an empty subtitle MUST have its flags set to 0.
 */
struct subpicture_t
{
    /** \name Channel ID */
    /**@{*/
    int             i_channel;                    /**< subpicture channel ID */
    /**@}*/

    /** \name Type and flags
       Should NOT be modified except by the vout thread */
    /**@{*/
    int64_t         i_order;                 /** an increasing unique number */
    subpicture_t *  p_next;               /**< next subtitle to be displayed */
    /**@}*/

    subpicture_region_t *p_region;  /**< region list composing this subtitle */

    /** \name Date properties */
    /**@{*/
    vlc_tick_t      i_start;                  /**< beginning of display date */
    vlc_tick_t      i_stop;                         /**< end of display date */
    bool            b_ephemer;    /**< If this flag is set to true the subtitle
                                will be displayed until the next one appear */
    bool            b_fade;                               /**< enable fading */
    /**@}*/

    /** \name Display properties
     * These properties are only indicative and may be
     * changed by the video output thread, or simply ignored depending of the
     * subtitle type. */
    /**@{*/
    bool         b_subtitle;            /**< the picture is a movie subtitle */
    bool         b_absolute;                       /**< position is absolute */
    int          i_original_picture_width;  /**< original width of the movie */
    int          i_original_picture_height;/**< original height of the movie */
    int          i_alpha;                                  /**< transparency */
     /**@}*/

    subpicture_updater_t updater;

    subpicture_private_t *p_private;    /* Reserved to the core */
};

/**
 * This function create a new empty subpicture.
 *
 * You must use subpicture_Delete to destroy it.
 */
VLC_API subpicture_t * subpicture_New( const subpicture_updater_t * );

/**
 * This function delete a subpicture created by subpicture_New.
 * You may give it NULL.
 */
VLC_API void subpicture_Delete( subpicture_t *p_subpic );

/**
 * This function will create a subpicture having one region in the requested
 * chroma showing the given picture.
 *
 * The picture_t given is not released nor used inside the
 * returned subpicture_t.
 */
VLC_API subpicture_t * subpicture_NewFromPicture( vlc_object_t *, picture_t *, vlc_fourcc_t i_chroma );

/**
 * This function will update the content of a subpicture created with
 * a non NULL subpicture_updater_t.
 */
VLC_API void subpicture_Update( subpicture_t *, const video_format_t *src, const video_format_t *, vlc_tick_t );

/**
 * This function will blend a given subpicture onto a picture.
 *
 * The subpicture and all its region must:
 *  - be absolute.
 *  - not be ephemere.
 *  - not have the fade flag.
 *  - contains only picture (no text rendering).
 * \return the number of region(s) successfully blent
 */
VLC_API unsigned picture_BlendSubpicture( picture_t *, filter_t *p_blend, subpicture_t * );

/**
 * 文本字幕外部渲染回调（Android 自绘场景）。
 *
 * 由 spu 渲染线程调用：每当"当前应显示的文本字幕"发生变化（含清空，psz_text 为 ""）
 * 时触发一次，与显示时机完全同步。
 *
 * @param p_data  注册时传入的用户指针
 * @param psz_text UTF-8 字幕文本（可能含换行）；"" 表示当前无字幕应显示
 * @param i_start 当前字幕的显示起始时间（vlc_tick_t，微秒）
 * @param i_stop  当前字幕的显示结束时间（0 表示未知/延续）
 */
typedef void (*vlc_spu_text_cb)( void *p_data, const char *psz_text,
                                 int64_t i_start, int64_t i_stop );

/**
 * 注册/注销文本字幕监听。
 *
 * @param pf_text 回调函数；传 NULL 注销
 * @param p_data  回调用户指针
 * @param b_listener_only true 时，文本字幕只回调不上屏（由外部自绘）；
 *                图形字幕（PGS 等，无文本）不受影响、照常上屏。
 *                false 时仅回调，内核照常渲染。
 *
 * 线程安全。进程级生效（对之后创建的所有 vout/spu 生效）。
 */
VLC_API void vlc_spu_set_text_listener( vlc_spu_text_cb pf_text, void *p_data,
                                        bool b_listener_only );

/**@}*/

#endif /* _VLC_VIDEO_H */
