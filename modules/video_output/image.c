/*****************************************************************************
 * image.c : image video output
 *****************************************************************************
 * Copyright (C) 2004-2006 the VideoLAN team
 * $Id$
 *
 * Authors: Clément Stenac <zorglub@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc/vlc.h>
#include <vlc_vout.h>
#include <vlc_interface.h>

#include "vlc_image.h"
#include "vlc_strings.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  Create    ( vlc_object_t * );
static void Destroy   ( vlc_object_t * );

static int  Init      ( vout_thread_t * );
static void End       ( vout_thread_t *p_vout );
static void Display   ( vout_thread_t *, picture_t * );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define FORMAT_TEXT N_( "Image format" )
#define FORMAT_LONGTEXT N_( "Format of the output images (png or jpg)." )

#define WIDTH_TEXT N_( "Image width" )
#define WIDTH_LONGTEXT N_( "You can enforce the image width. By default " \
                            "(-1) VLC will adapt to the video " \
                            "characteristics.")

#define HEIGHT_TEXT N_( "Image height" )
#define HEIGHT_LONGTEXT N_( "You can enforce the image height. By default " \
                            "(-1) VLC will adapt to the video " \
                            "characteristics.")

#define RATIO_TEXT N_( "Recording ratio" )
#define RATIO_LONGTEXT N_( "Ratio of images to record. "\
                           "3 means that one image out of three is recorded." )

#define PREFIX_TEXT N_( "Filename prefix" )
#define PREFIX_LONGTEXT N_( "Prefix of the output images filenames. Output " \
                            "filenames will have the \"prefixNUMBER.format\" "\
                            "form." )

#define REPLACE_TEXT N_( "Always write to the same file" )
#define REPLACE_LONGTEXT N_( "Always write to the same file instead of " \
                            "creating one file per image. In this case, " \
                             "the number is not appended to the filename." )

static const char *psz_format_list[] = { "png", "jpeg" };
static const char *psz_format_list_text[] = { "PNG", "JPEG" };

#define CFG_PREFIX "image-out-"

vlc_module_begin( );
    set_shortname( _( "Image file" ) );
    set_description( _( "Image video output" ) );
    set_category( CAT_VIDEO );
    set_subcategory( SUBCAT_VIDEO_VOUT );
    set_capability( "video output", 0 );

    add_string(  CFG_PREFIX "format", "png", NULL,
                 FORMAT_TEXT, FORMAT_LONGTEXT, VLC_FALSE );
    change_string_list( psz_format_list, psz_format_list_text, 0 );
    add_integer( CFG_PREFIX "width", 0, NULL,
                 WIDTH_TEXT, WIDTH_LONGTEXT, VLC_TRUE );
        add_deprecated_alias( "image-width" ); /* since 0.9.0 */
    add_integer( CFG_PREFIX "height", 0, NULL,
                 HEIGHT_TEXT, HEIGHT_LONGTEXT, VLC_TRUE );
        add_deprecated_alias( "image-height" ); /* since 0.9.0 */
    add_integer( CFG_PREFIX "ratio", 3, NULL,
                 RATIO_TEXT, RATIO_LONGTEXT, VLC_FALSE );
    add_string(  CFG_PREFIX "prefix", "img", NULL,
                 PREFIX_TEXT, PREFIX_LONGTEXT, VLC_FALSE );
    add_bool(    CFG_PREFIX "replace", 0, NULL,
                 REPLACE_TEXT, REPLACE_LONGTEXT, VLC_FALSE );
    set_callbacks( Create, Destroy );
vlc_module_end();

static const char *ppsz_vout_options[] = {
    "format", "width", "height", "ratio", "prefix", "replace", NULL
};

/*****************************************************************************
 * vout_sys_t: video output descriptor
 *****************************************************************************/
struct vout_sys_t
{
    char        *psz_prefix;          /* Prefix */
    char        *psz_format;          /* Format */
    int         i_ratio;         /* Image ratio */

    unsigned int i_width;        /* Image width */
    unsigned int i_height;      /* Image height */

    int         i_current;     /* Current image */
    int         i_frames;   /* Number of frames */

    vlc_bool_t  b_replace;

    vlc_bool_t b_time;
    vlc_bool_t b_meta;

    image_handler_t *p_image;
};

/*****************************************************************************
 * Create: allocates video thread
 *****************************************************************************
 * This function allocates and initializes a vout method.
 *****************************************************************************/
static int Create( vlc_object_t *p_this )
{
    vout_thread_t *p_vout = ( vout_thread_t * )p_this;

    /* Allocate instance and initialize some members */
    p_vout->p_sys = malloc( sizeof( vout_sys_t ) );
    if( ! p_vout->p_sys )
        return VLC_ENOMEM;

    config_ChainParse( p_vout, CFG_PREFIX, ppsz_vout_options,
                       p_vout->p_cfg );

    p_vout->p_sys->psz_prefix =
            var_CreateGetString( p_this, CFG_PREFIX "prefix" );
    p_vout->p_sys->b_time = strchr( p_vout->p_sys->psz_prefix, '%' )
                            ? VLC_TRUE : VLC_FALSE;
    p_vout->p_sys->b_meta = strchr( p_vout->p_sys->psz_prefix, '$' )
                            ? VLC_TRUE : VLC_FALSE;
    p_vout->p_sys->psz_format =
            var_CreateGetString( p_this, CFG_PREFIX "format" );
    p_vout->p_sys->i_width =
            var_CreateGetInteger( p_this, CFG_PREFIX "width" );
    p_vout->p_sys->i_height =
            var_CreateGetInteger( p_this, CFG_PREFIX "height" );
    p_vout->p_sys->i_ratio =
            var_CreateGetInteger( p_this, CFG_PREFIX "ratio" );
    p_vout->p_sys->b_replace =
            var_CreateGetBool( p_this, CFG_PREFIX "replace" );
    p_vout->p_sys->i_current = 0;
    p_vout->p_sys->p_image = image_HandlerCreate( p_vout );

    if( !p_vout->p_sys->p_image )
    {
        msg_Err( p_this, "unable to create image handler") ;
        FREENULL( p_vout->p_sys->psz_prefix );
        FREENULL( p_vout->p_sys );
        return VLC_EGENERIC;
    }

    p_vout->pf_init = Init;
    p_vout->pf_end = End;
    p_vout->pf_manage = NULL;
    p_vout->pf_render = Display;
    p_vout->pf_display = NULL;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Init: initialize video thread
 *****************************************************************************/
static int Init( vout_thread_t *p_vout )
{
    int i_index;
    picture_t *p_pic;

    /* Initialize the output structure */
    p_vout->output.i_chroma = p_vout->render.i_chroma;
    p_vout->output.pf_setpalette = NULL;
    p_vout->output.i_width = p_vout->render.i_width;
    p_vout->output.i_height = p_vout->render.i_height;
    p_vout->output.i_aspect = p_vout->output.i_width
                               * VOUT_ASPECT_FACTOR / p_vout->output.i_height;

    p_vout->output.i_rmask = 0xff0000;
    p_vout->output.i_gmask = 0x00ff00;
    p_vout->output.i_bmask = 0x0000ff;

    /* Try to initialize 1 direct buffer */
    p_pic = NULL;

    /* Find an empty picture slot */
    for( i_index = 0 ; i_index < VOUT_MAX_PICTURES ; i_index++ )
    {
        if( p_vout->p_picture[ i_index ].i_status == FREE_PICTURE )
        {
            p_pic = p_vout->p_picture + i_index;
            break;
        }
    }

    /* Allocate the picture */
    if( p_pic == NULL )
    {
        return VLC_EGENERIC;
    }

    vout_AllocatePicture( VLC_OBJECT(p_vout), p_pic, p_vout->output.i_chroma,
                          p_vout->output.i_width, p_vout->output.i_height,
                          p_vout->output.i_aspect );

    if( p_pic->i_planes == 0 )
    {
        return VLC_EGENERIC;
    }

    p_pic->i_status = DESTROYED_PICTURE;
    p_pic->i_type   = DIRECT_PICTURE;

    PP_OUTPUTPICTURE[ I_OUTPUTPICTURES ] = p_pic;
    I_OUTPUTPICTURES++;
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Destroy: destroy video thread
 *****************************************************************************
 * Terminate an output method created by Create
 *****************************************************************************/
static void Destroy( vlc_object_t *p_this )
{
    int i_index;
    vout_thread_t *p_vout = ( vout_thread_t * )p_this;

    for( i_index = I_OUTPUTPICTURES-1; i_index >= 0; i_index-- )
    {
        free( PP_OUTPUTPICTURE[ i_index ]->p_data );
    }

    /* Destroy structure */
    image_HandlerDelete( p_vout->p_sys->p_image );
    FREENULL( p_vout->p_sys->psz_prefix );
    FREENULL( p_vout->p_sys->psz_format );
    FREENULL( p_vout->p_sys );
}

/*****************************************************************************
 * Display: displays previously rendered output
 *****************************************************************************
 * This function copies the rendered picture into our circular buffer.
 *****************************************************************************/
static void Display( vout_thread_t *p_vout, picture_t *p_pic )
{
    video_format_t fmt_in, fmt_out;

    char *psz_filename;
    char *psz_prefix;
    char *psz_tmp;

    memset( &fmt_in, 0, sizeof( fmt_in ) );
    memset( &fmt_out, 0, sizeof( fmt_out ) );

    if( p_vout->p_sys->i_frames % p_vout->p_sys->i_ratio != 0 )
    {
        p_vout->p_sys->i_frames++;
        return;
    }
    p_vout->p_sys->i_frames++;

    fmt_in.i_chroma = p_vout->render.i_chroma;
    fmt_in.i_width = p_vout->render.i_width;
    fmt_in.i_height = p_vout->render.i_height;

    fmt_out.i_width = (p_vout->p_sys->i_width > 0) ? p_vout->p_sys->i_width :
                                                   p_vout->render.i_width;
    fmt_out.i_height = (p_vout->p_sys->i_height > 0) ? p_vout->p_sys->i_height :
                                                     p_vout->render.i_height;

    if( p_vout->p_sys->b_time )
    {
        psz_tmp = str_format_time( p_vout->p_sys->psz_prefix );
        path_sanitize( psz_tmp );
    }
    else
        psz_tmp = p_vout->p_sys->psz_prefix;
    if( p_vout->p_sys->b_meta )
    {
        psz_prefix = str_format_meta( p_vout, psz_tmp );
        path_sanitize( psz_prefix );
        if( p_vout->p_sys->b_time )
            free( psz_tmp );
    }
    else
        psz_prefix = psz_tmp;
    psz_filename = (char *)malloc( 10 + strlen( psz_prefix )
                                      + strlen( p_vout->p_sys->psz_format ) );
    if( p_vout->p_sys->b_replace )
    {
        sprintf( psz_filename, "%s.%s", psz_prefix,
                                        p_vout->p_sys->psz_format );
    }
    else
    {
        sprintf( psz_filename, "%s%.6i.%s", psz_prefix,
                                            p_vout->p_sys->i_current,
                                            p_vout->p_sys->psz_format );
    }
    if( p_vout->p_sys->b_time || p_vout->p_sys->b_meta )
        free( psz_prefix );
    image_WriteUrl( p_vout->p_sys->p_image, p_pic,
                    &fmt_in, &fmt_out, psz_filename ) ;
    free( psz_filename );

    p_vout->p_sys->i_current++;

    return;
}


static void End( vout_thread_t *p_vout )
{
    VLC_UNUSED(p_vout);
}
