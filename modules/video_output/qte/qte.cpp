/*****************************************************************************
 * qte.cpp : QT Embedded plugin for vlc
 *****************************************************************************
 * Copyright (C) 1998-2002 VideoLAN
 * $Id: qte.cpp,v 1.1 2002/09/04 21:13:33 jpsaman Exp $
 *
 * Authors: Gerald Hansink <gerald.hansink@ordain.nl>
 *          Jean-Paul Saman <jpsaman@wxs.nl>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

/*****************************************************************************
 * notes:
 * - written for ipaq, so hardcoded assumptions specific for ipaq...
 * - runs full screen
 * - no "mouse events" handling
 * - etc.
 *****************************************************************************/

#include <errno.h>                                                 /* ENOMEM */
#include <stdlib.h>                                                /* free() */
#include <string.h>                                                /* strerror() */

#include <videolan/vlc.h>

#ifdef HAVE_MACHINE_PARAM_H
    /* BSD */
#   include <machine/param.h>
#   include <sys/types.h>                                  /* typedef ushort */
#   include <sys/ipc.h>
#endif

#ifndef WIN32
#   include <netinet/in.h>                            /* BSD: struct in_addr */
#endif

#ifdef HAVE_SYS_SHM_H
#   include <sys/shm.h>                                /* shmget(), shmctl() */
#endif

#include <qapplication.h>
#include <qpainter.h>

#ifdef Q_WS_QWS
# define USE_DIRECT_PAINTER
# include <qdirectpainter_qws.h>
# include <qgfxraster_qws.h>
#endif

#include "video.h"
#include "video_output.h"

#include "interface.h"
#include "netutils.h"                                 /* network_ChannelJoin */

#include "stream_control.h"                 /* needed by input_ext-intf.h... */
#include "input_ext-intf.h"

#include "qte.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define ALT_FS_TEXT N_("alternate fullscreen method")
#define ALT_FS_TEXT N_("alternate fullscreen method")
#define ALT_FS_LONGTEXT N_( \
    "There are two ways to make a fullscreen window, unfortunately each one " \
    "has its drawbacks.\n" \
    "1) Let the window manager handle your fullscreen window (default). But " \
    "things like taskbars will likely show on top of the video.\n" \
    "2) Completly bypass the window manager, but then nothing will be able " \
    "to show on top of the video.")
#define DISPLAY_TEXT N_("QT Embedded display name")
#define DISPLAY_LONGTEXT N_( \
    "Specify the X11 hardware display you want to use. By default vlc will " \
    "use the value of the DISPLAY environment variable.")
#define DRAWABLE_TEXT N_("QT Embedded drawable")
#define DRAWABLE_LONGTEXT N_( \
    "Specify a QT Embedded drawable to use instead of opening a new window. This " \
    "option is DANGEROUS, use with care.")

/*****************************************************************************
* Exported prototypes
*****************************************************************************/

vlc_module_begin();
    add_category_hint( N_("QT Embedded"), NULL );
    add_string( "qte-display", NULL, NULL, NULL, NULL); //DISPLAY_TEXT, DISPLAY_LONGTEXT );
    add_bool( "qte-altfullscreen", 0, NULL, NULL, NULL); //ALT_FS_TEXT, ALT_FS_LONGTEXT );
    add_integer( "qte-drawable", -1, NULL, NULL, NULL); //DRAWABLE_TEXT, DRAWABLE_LONGTEXT );
    set_description( _("QT Embedded module") );
    set_capability( "video output", 20 );
    set_callbacks( Open, Close);
vlc_module_end();

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  Open      ( vlc_object_t * );
static void Close     ( vlc_object_t * );
static void Render    ( vout_thread_t *, picture_t * );
static void Display   ( vout_thread_t *, picture_t * );
static int  Manage    ( vout_thread_t * );
static int  Init      ( vout_thread_t * );
static void End       ( vout_thread_t * );

static int  CreateQtWindow ( vout_thread_t * );
static void DestroyQtWindow( vout_thread_t * );

static int  NewPicture     ( vout_thread_t *, picture_t * );
static void FreePicture    ( vout_thread_t *, picture_t * );

static void ToggleFullScreen      ( vout_thread_t * );

static void* run_qtapp_exec (void* pVoid);

/*****************************************************************************
 * Seeking function TODO: put this in a generic location !
 *****************************************************************************/
static inline void vout_Seek( off_t i_seek )
{
}

/*****************************************************************************
 * Open: allocate video thread output method
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    //msg_Err( "+vout_Create::qte" );
    vout_thread_t * p_vout = (vout_thread_t *)p_this;

    /* Allocate structure */
    p_vout->p_sys = (vout_sys_s*) malloc( sizeof( vout_sys_t ) );

    if( p_vout->p_sys == NULL )
    {
        msg_Err( p_vout, "out of memory" );
        return( 1 );
    }

//    memset(p_vout->p_sys, 0, sizeof( vout_sys_t ));

    p_vout->pf_init    = Init;
    p_vout->pf_end     = End;
    p_vout->pf_manage  = NULL; //Manage;
    p_vout->pf_render  = NULL; //Render;
    p_vout->pf_display = Display;

    CreateQtWindow(p_vout);

    //msg_Err( "-vout_Create::qte\n" );
    return( 0 );
}

/*****************************************************************************
 * CloseVideo: destroy Sys video thread output method
 *****************************************************************************
 * Terminate an output method created by Open
 *****************************************************************************/
static void Close ( vlc_object_t *p_this )
{
    vout_thread_t * p_vout = (vout_thread_t *)p_this;

    //msg_Err( "+vout_Destroy::qte\n" );
    DestroyQtWindow(p_vout);
    free(p_vout->p_sys);
}

/*****************************************************************************
 * Init: initialize video thread output method
 *****************************************************************************
 * This function create the buffers needed by the output thread. It is called
 * at the beginning of the thread, but also each time the window is resized.
 *****************************************************************************/
static int Init( vout_thread_t *p_vout )
{
    int         i_index;
    picture_t*  p_pic;

    int         dd = QPixmap::defaultDepth();

    //msg_Err( "+vout_Init::qte\n" );

    I_OUTPUTPICTURES = 0;

    p_vout->output.i_chroma = (dd == 16) ? FOURCC_RV16 : FOURCC_RV32;
    p_vout->output.i_rmask  = 0xf800;
    p_vout->output.i_gmask  = 0x07e0;
    p_vout->output.i_bmask  = 0x001f;
    //p_vout->output.i_width  = p_vout->render.i_width;
    //p_vout->output.i_height = p_vout->render.i_height;
    p_vout->output.i_width  = p_vout->p_sys->i_width;
    p_vout->output.i_height = p_vout->p_sys->i_height;
    p_vout->output.i_aspect = p_vout->render.i_aspect;

    /* Try to initialize MAX_DIRECTBUFFERS direct buffers */
    while( I_OUTPUTPICTURES < QTE_MAX_DIRECTBUFFERS )
    {
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
        if( p_pic == NULL ||  NewPicture( p_vout, p_pic ) )
        {
            break;
        }

        p_pic->i_status = DESTROYED_PICTURE;
        p_pic->i_type   = DIRECT_PICTURE;

        PP_OUTPUTPICTURE[ I_OUTPUTPICTURES ] = p_pic;

        I_OUTPUTPICTURES++;
    }

    //msg_Err( "-vout_Init::qte %d output pictures\n", I_OUTPUTPICTURES);

    return( 0 );
}


/*****************************************************************************
 * Render: render previously calculated output
 *****************************************************************************/
static void Render( vout_thread_t *p_vout, picture_t *p_pic )
{
    //msg_Err( "+vout_Render::qte\n" );
    ;
}

/*****************************************************************************
 * Display: displays previously rendered output
 *****************************************************************************
 * This function sends the currently rendered image to screen.
 *****************************************************************************/
static void Display( vout_thread_t *p_vout, picture_t *p_pic )
{
    int x, y, w, h;

    vout_PlacePicture( p_vout, p_vout->p_sys->i_width, p_vout->p_sys->i_height,
                       &x, &y, &w, &h );

    if(p_vout->p_sys->pcVoutWidget)
    {
// shameless borrowed from opie mediaplayer....
#ifndef USE_DIRECT_PAINTER
        QPainter p(p_vout->p_sys->pcVoutWidget);

        /* rotate frame */
        int dd      = QPixmap::defaultDepth();
        int bytes   = ( dd == 16 ) ? 2 : 4;
        int rw = h, rh = w;

        QImage rotatedFrame( rw, rh, bytes << 3 );

        ushort* in  = (ushort*)p_pic->p_sys->pQImage->bits();
        ushort* out = (ushort*)rotatedFrame.bits();

        int spl = rotatedFrame.bytesPerLine() / bytes;
        for (int x=0; x<h; x++)
        {
            if ( bytes == 2 )
            {
                ushort* lout = out++ + (w - 1)*spl;
                for (int y=0; y<w; y++)
                {
                    *lout=*in++;
                    lout-=spl;
                }
            }
            else
            {
                ulong* lout = ((ulong *)out)++ + (w - 1)*spl;
                for (int y=0; y<w; y++)
                {
                    *lout=*((ulong*)in)++;
                    lout-=spl;
                }
            }
        }

        p.drawImage( x, y, rotatedFrame, 0, 0, rw, rh );
#else
        QDirectPainter p(p_vout->p_sys->pcVoutWidget);

        // just copy the image to the frame buffer...
        memcpy(p.frameBuffer(), (p_pic->p_sys->pQImage->jumpTable())[0], h * p.lineStep());
#endif
    }
}

/*****************************************************************************
 * Manage: handle X11 events
 *****************************************************************************
 * This function should be called regularly by video output thread. It manages
 * X11 events and allows window resizing. It returns a non null value on
 * error.
 *****************************************************************************/
static int Manage( vout_thread_t *p_vout )
{
    //msg_Err( "+vout_Manage::qte\n" );
    return 0;
}

/*****************************************************************************
 * End: terminate video thread output method
 *****************************************************************************
 * Destroy the buffers created by vout_Init. It is called at the end of
 * the thread, but also each time the window is resized.
 *****************************************************************************/
static void End( vout_thread_t *p_vout )
{
    int i_index;

    //msg_Err( "+vout_End::qte\n" );

    /* Free the direct buffers we allocated */
    for( i_index = I_OUTPUTPICTURES ; i_index ; )
    {
        i_index--;
        FreePicture( p_vout, PP_OUTPUTPICTURE[ i_index ] );
    }
}


/*****************************************************************************
 * NewPicture: allocate a picture
 *****************************************************************************
 * Returns 0 on success, -1 otherwise
 *****************************************************************************/
static int NewPicture( vout_thread_t *p_vout, picture_t *p_pic )
{
    int dd = QPixmap::defaultDepth();

    //msg_Err( "+NewPicture::dd = %d\n",dd );

    p_pic->p_sys = (picture_sys_t*) malloc( sizeof( picture_sys_t ) );

    if( p_pic->p_sys == NULL )
    {
        return -1;
    }

    switch(p_vout->output.i_chroma)
    {
    case FOURCC_RV16:
        if(dd == 16)
        {
            p_pic->p_sys->pQImage = new QImage(p_vout->output.i_width,
                                               p_vout->output.i_height,
                                               dd );

            if(p_pic->p_sys->pQImage == NULL)
            {
                return -1;
            }

            p_pic->p->p_pixels = (p_pic->p_sys->pQImage->jumpTable())[0];

            p_pic->p->i_pitch = p_pic->p_sys->pQImage->bytesPerLine();

            p_pic->p->i_lines = p_vout->output.i_height;
            p_pic->p->i_pixel_bytes = 2;
            p_pic->p->b_margin      = 0;
            p_pic->i_planes         = 1;
        }
        else
        {
            return -1;
        }
        break;
    case FOURCC_RV32:
        if(dd == 32)
        {
            p_pic->p_sys->pQImage = new QImage(p_vout->output.i_width,
                                               p_vout->output.i_height,
                                               dd );

            if(p_pic->p_sys->pQImage == NULL)
            {
                return -1;
            }

            p_pic->p->p_pixels = (p_pic->p_sys->pQImage->jumpTable())[0];

            p_pic->p->i_pitch = p_pic->p_sys->pQImage->bytesPerLine();

            p_pic->p->i_lines = p_vout->output.i_height;
            p_pic->p->i_pixel_bytes = 4;
            p_pic->p->b_margin      = 0;
            p_pic->i_planes         = 1;
        }
        else
        {
            return -1;
        }
        break;
    default:
        return -1;
        break;
    }

/*
    msg_Err( "NewPicture: %d %d %d\n",p_vout->output.i_width,
                                 p_vout->output.i_height,
                                 p_vout->output.i_chroma );
*/
    return 0;
}

/*****************************************************************************
 * FreePicture: destroy a picture allocated with NewPicture
 *****************************************************************************/
static void FreePicture( vout_thread_t *p_vout, picture_t *p_pic )
{
    delete p_pic->p_sys->pQImage;
}

/*****************************************************************************
 * ToggleFullScreen: Enable or disable full screen mode
 *****************************************************************************
 * This function will switch between fullscreen and window mode.
 *
 *****************************************************************************/
static void ToggleFullScreen ( vout_thread_t *p_vout )
{
}


/*****************************************************************************
 * CreateQtWindow: create qte applicaton / window
 *****************************************************************************
 * Create a window according to video output given size, and set other
 * properties according to the display properties.
 *****************************************************************************/
static int CreateQtWindow( vout_thread_t *p_vout )
{
    //msg_Err( "vout_qt: +init qt window");

    /* for displaying the vout in a qt window we need the QtApplication */
    vlc_thread_t    thread_id;
    //msg_Err( "vout_qt: +init qt window, creating qpe application");

    p_vout->p_sys->pcVoutWidget = NULL;

    /* create thread to exec the qpe application */
    if ( vlc_thread_create( &thread_id, "vout qte",
                            (vlc_thread_func_t)vout_run_qtapp_exec,
                            (void *)p_vout) )
    {
        msg_Err( "input error: can't spawn vout thread");
        return( -1 );
    }

    p_vout->p_sys->i_width  = 320;
    p_vout->p_sys->i_height = 240;

    // just wait until the crew is complete...
    while(p_vout->p_sys->pcVoutWidget == NULL)
    {
        msleep(1);
    }

    //msg_Err( "vout_qt: -init qt window");

    return( 0 );
}


/*****************************************************************************
 * DestroyQtWindow: destroy the window
 *****************************************************************************/
static void DestroyQtWindow( vout_thread_t *p_vout )
{
    // quit qt application loop
    if(p_vout->p_sys->pcQApplication)
    {
        if(p_vout->p_sys->bOwnsQApp)
        {
            p_vout->p_sys->pcQApplication->quit();
        }
        else
        {
            p_vout->p_sys->bRunning = FALSE;
        }

        while(p_vout->p_sys->pcVoutWidget)
        {
            msleep(1);
        }
    }
}

/*****************************************************************************
 * main loop of qtapplication
 *****************************************************************************/
static void*
run_qtapp_exec(void* pVoid)
{
    int     argc    = 0;
    char    arg0[]  = "vout qte";

    vout_thread_t* p_vout = (vout_thread_t*) pVoid;

    if(qApp == NULL)
    {
        QApplication* pApp = new QApplication(argc, NULL);
        if(pApp)
        {
            p_vout->p_sys->pcQApplication = pApp;
            p_vout->p_sys->bOwnsQApp = TRUE;
        }
        else
        {
            return NULL;
        }
    }
    else
    {
        p_vout->p_sys->pcQApplication = qApp;
    }

    {
        QWidget vo(0, "vout");
        vo.showFullScreen();
        vo.show();
        p_vout->p_sys->pcVoutWidget = &vo;

        p_vout->p_sys->bRunning = TRUE;

        if(p_vout->p_sys->bOwnsQApp)
        {
            // run the main loop of qtapplication until someone says: 'quit'
            p_vout->p_sys->pcQApplication->exec();
        }
        else
        {
            while(p_vout->p_sys->bRunning) msleep(100);
        }
    }

    p_vout->p_sys->pcVoutWidget = NULL;

    if(p_vout->p_sys->bOwnsQApp)
    {
        delete p_vout->p_sys->pcQApplication;
        p_vout->p_sys->pcQApplication = NULL;
    }

    return 0;
}

