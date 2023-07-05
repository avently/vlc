/*****************************************************************************
 * duplicate.c: duplicate stream output module
 *****************************************************************************
 * Copyright (C) 2003-2004 the VideoLAN team
 *
 * Author: Laurent Aimar <fenrir@via.ecp.fr>
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

#include <vlc_common.h>
#include <vlc_configuration.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_block.h>
#include <vlc_subpicture.h>
#include <vlc_vector.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

vlc_module_begin ()
    set_description( N_("Duplicate stream output") )
    set_capability( "sout output", 50 )
    add_shortcut( "duplicate", "dup" )
    set_subcategory( SUBCAT_SOUT_STREAM )
    set_callbacks( Open, Close )
    add_submodule()
    set_capability("sout filter", 0)
    add_shortcut("duplicate", "dup")
    set_callbacks(Open, Close)
vlc_module_end ()


/*****************************************************************************
 * Exported prototypes
 *****************************************************************************/
static void *Add( sout_stream_t *, const es_format_t *, const char * );
static void  Del( sout_stream_t *, void * );
static int   Send( sout_stream_t *, void *, block_t * );
static void  SetPCR( sout_stream_t *, vlc_tick_t );

typedef struct {
    sout_stream_t *stream;
    char *select_chain;
} duplicated_stream_t;

typedef struct
{
    struct VLC_VECTOR(duplicated_stream_t) streams;
} sout_stream_sys_t;

typedef struct {
    void *id;
    /* Reference to the duplicated output stream. */
    sout_stream_t *stream_owner;
} duplicated_id_t;

typedef struct
{
    struct VLC_VECTOR(duplicated_id_t) dup_ids;
} sout_stream_id_sys_t;

static bool ESSelected( struct vlc_logger *, const es_format_t *fmt,
                        char *psz_select );

/*****************************************************************************
 * Control
 *****************************************************************************/
static int Control( sout_stream_t *p_stream, int i_query, va_list args )
{
    /* Fanout controls */
    switch( i_query )
    {
        case SOUT_STREAM_ID_SPU_HIGHLIGHT:
        {
            sout_stream_id_sys_t *id = va_arg(args, void *);
            const vlc_spu_highlight_t *spu_hl = va_arg(args, const vlc_spu_highlight_t *);

            duplicated_id_t *dup_id;
            vlc_vector_foreach_ref( dup_id, &id->dup_ids )
            {
                if( dup_id->id != NULL )
                    sout_StreamControl(
                        dup_id->stream_owner, i_query, dup_id->id, spu_hl );
            }
            return VLC_SUCCESS;
        }
    }

    return VLC_EGENERIC;
    (void)p_stream;
}

static const struct sout_stream_operations ops = {
    Add, Del, Send, Control, NULL, SetPCR,
};

/*****************************************************************************
 * Open:
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    sout_stream_t     *p_stream = (sout_stream_t*)p_this;
    sout_stream_sys_t *p_sys;
    config_chain_t        *p_cfg;

    msg_Dbg( p_stream, "creating 'duplicate'" );

    p_sys = malloc( sizeof( sout_stream_sys_t ) );
    if( !p_sys )
        return VLC_ENOMEM;

    vlc_vector_init( &p_sys->streams );

    char **ppsz_select = NULL;

    for( p_cfg = p_stream->p_cfg; p_cfg != NULL; p_cfg = p_cfg->p_next )
    {
        if( !strncmp( p_cfg->psz_name, "dst", strlen( "dst" ) ) )
        {
            duplicated_stream_t dup_stream = {0};

            msg_Dbg( p_stream, " * adding `%s'", p_cfg->psz_value );
            dup_stream.stream = sout_StreamChainNew(
                VLC_OBJECT(p_stream), p_cfg->psz_value, p_stream->p_next );

            if( dup_stream.stream != NULL )
            {
                vlc_vector_push( &p_sys->streams, dup_stream );
                ppsz_select = &vlc_vector_last_ref( &p_sys->streams )->select_chain;
            }
        }
        else if( !strncmp( p_cfg->psz_name, "select", strlen( "select" ) ) )
        {
            char *psz = p_cfg->psz_value;

            if( psz && *psz )
            {
                if( ppsz_select == NULL )
                {
                    msg_Err( p_stream, " * ignore selection `%s'", psz );
                }
                else
                {
                    msg_Dbg( p_stream, " * apply selection `%s'", psz );
                    *ppsz_select = strdup( psz );
                    ppsz_select = NULL;
                }
            }
        }
        else
        {
            msg_Err( p_stream, " * ignore unknown option `%s'", p_cfg->psz_name );
        }
    }

    if( p_sys->streams.size == 0 )
    {
        msg_Err( p_stream, "no destination given" );
        free( p_sys );

        return VLC_EGENERIC;
    }

    p_stream->p_sys = p_sys;
    p_stream->ops = &ops;
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t * p_this )
{
    sout_stream_t     *p_stream = (sout_stream_t*)p_this;
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    msg_Dbg( p_stream, "closing a duplication" );
    duplicated_stream_t *dup_stream;
    vlc_vector_foreach_ref( dup_stream, &p_sys->streams )
    {
        sout_StreamChainDelete( dup_stream->stream, p_stream->p_next );
        free( dup_stream->select_chain );
    }
    vlc_vector_destroy( &p_sys->streams );

    free( p_sys );
}

/*****************************************************************************
 * Add:
 *****************************************************************************/
static void *
Add( sout_stream_t *p_stream, const es_format_t *p_fmt, const char *es_id )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    sout_stream_id_sys_t  *id;
    size_t i_valid_streams = 0;

    id = malloc( sizeof( sout_stream_id_sys_t ) );
    if( !id )
        return NULL;

    vlc_vector_init( &id->dup_ids );

    msg_Dbg( p_stream, "duplicated a new stream codec=%4.4s (es=%d group=%d)",
             (char*)&p_fmt->i_codec, p_fmt->i_id, p_fmt->i_group );

    duplicated_stream_t *dup_stream;
    vlc_vector_foreach_ref( dup_stream, &p_sys->streams )
    {
        duplicated_id_t dup_id = {.stream_owner = dup_stream->stream};
        const size_t idx = vlc_vector_idx_dup_stream;

        if( ESSelected(p_stream->obj.logger, p_fmt, dup_stream->select_chain) )
        {
            /* FIXME(Alaric): suffix the string id with the duplicated track
             * count. */
            dup_id.id = sout_StreamIdAdd( dup_id.stream_owner,
                                          p_fmt, es_id );
            if( dup_id.id != NULL )
            {
                msg_Dbg( p_stream, "    - added for output %zu", idx );
                i_valid_streams++;
            }
            else
            {
                msg_Dbg( p_stream, "    - failed for output %zu", idx);
            }
        }
        else
        {
            msg_Dbg( p_stream, "    - ignored for output %zu", idx );
        }

        /* Append failed attempts as well to keep track of which pp_id
         * belongs to which duplicated stream */
        vlc_vector_push( &id->dup_ids, dup_id );
    }

    if( i_valid_streams <= 0 )
    {
        Del( p_stream, id );
        return NULL;
    }

    return id;
}

/*****************************************************************************
 * Del:
 *****************************************************************************/
static void Del( sout_stream_t *p_stream, void *_id )
{
    sout_stream_id_sys_t *id = (sout_stream_id_sys_t *)_id;

    duplicated_id_t *dup_id;
    vlc_vector_foreach_ref( dup_id, &id->dup_ids )
    {
        if( dup_id->id != NULL )
            sout_StreamIdDel( dup_id->stream_owner, dup_id->id );
    }
    vlc_vector_destroy( &id->dup_ids );

    free( id );
    (void)p_stream;
}

/*****************************************************************************
 * Send:
 *****************************************************************************/
static int Send( sout_stream_t *p_stream, void *_id, block_t *p_buffer )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    sout_stream_id_sys_t *id = (sout_stream_id_sys_t *)_id;

    /* Loop through the linked list of buffers */
    while( p_buffer )
    {
        block_t *p_next = p_buffer->p_next;

        p_buffer->p_next = NULL;

        /* Should be ensured in `Add`. */
        assert(id->dup_ids.size > 0);

        for( size_t i = 0; i < id->dup_ids.size - 1; i++ )
        {
            duplicated_id_t *dup_id = &id->dup_ids.data[i];

            if( dup_id->id != NULL )
            {
                block_t *p_dup = block_Duplicate( p_buffer );

                if( p_dup )
                    sout_StreamIdSend( dup_id->stream_owner, dup_id->id, p_dup );
            }
        }

        duplicated_id_t *last_dup_id = vlc_vector_last_ref( &id->dup_ids );
        if( last_dup_id != NULL )
        {
            sout_StreamIdSend(
                last_dup_id->stream_owner, last_dup_id->id, p_buffer );
        }
        else
        {
            block_Release( p_buffer );
        }

        p_buffer = p_next;
    }
    return VLC_SUCCESS;
}

static void SetPCR( sout_stream_t *stream, vlc_tick_t pcr )
{
    sout_stream_sys_t *sys = stream->p_sys;

    duplicated_stream_t *dup_stream;
    vlc_vector_foreach_ref( dup_stream, &sys->streams )
    {
        sout_StreamSetPCR( dup_stream->stream, pcr );
    }
}

/*****************************************************************************
 * Divers
 *****************************************************************************/
static bool NumInRange( const char *psz_range, int i_num )
{
    int beginRange, endRange;
    int res = sscanf(psz_range, "%d-%d", &beginRange, &endRange);
    if (res == 0)
        return false;
    else if (res == 1)
        return beginRange == i_num;
    return (i_num >= beginRange && i_num <= endRange)
        || (beginRange > endRange && (i_num <= beginRange && i_num >= endRange));
}

static bool ESSelected( struct vlc_logger *logger, const es_format_t *fmt,
                        char *psz_select )
{
    char  *psz_dup;
    char  *psz;

    /* We have tri-state variable : no tested (-1), failed(0), succeed(1) */
    int i_cat = -1;
    int i_es  = -1;
    int i_prgm= -1;

    /* If empty all es are selected */
    if( psz_select == NULL || *psz_select == '\0' )
    {
        return true;
    }
    psz_dup = strdup( psz_select );
    if( !psz_dup )
        return false;
    psz = psz_dup;

    /* If non empty, parse the selection:
     * We have selection[,selection[,..]] where following selection are recognized:
     *      (no(-))audio
     *      (no(-))spu
     *      (no(-))video
     *      (no(-))es=[start]-[end] or es=num
     *      (no(-))prgm=[start]-[end] or prgm=num (program works too)
     *      if a negative test failed we exit directly
     */
    while( psz && *psz )
    {
        char *p;

        /* Skip space */
        while( *psz == ' ' || *psz == '\t' ) psz++;

        /* Search end */
        p = strchr( psz, ',' );
        if( p == psz )
        {
            /* Empty */
            psz = p + 1;
            continue;
        }
        if( p )
        {
            *p++ = '\0';
        }

        if( !strncmp( psz, "no-audio", strlen( "no-audio" ) ) ||
            !strncmp( psz, "noaudio", strlen( "noaudio" ) ) )
        {
            if( i_cat == -1 )
            {
                i_cat = fmt->i_cat != AUDIO_ES ? 1 : 0;
            }
        }
        else if( !strncmp( psz, "no-video", strlen( "no-video" ) ) ||
                 !strncmp( psz, "novideo", strlen( "novideo" ) ) )
        {
            if( i_cat == -1 )
            {
                i_cat = fmt->i_cat != VIDEO_ES ? 1 : 0;
            }
        }
        else if( !strncmp( psz, "no-spu", strlen( "no-spu" ) ) ||
                 !strncmp( psz, "nospu", strlen( "nospu" ) ) )
        {
            if( i_cat == -1 )
            {
                i_cat = fmt->i_cat != SPU_ES ? 1 : 0;
            }
        }
        else if( !strncmp( psz, "audio", strlen( "audio" ) ) )
        {
            if( i_cat == -1 )
            {
                i_cat = fmt->i_cat == AUDIO_ES ? 1 : 0;
            }
        }
        else if( !strncmp( psz, "video", strlen( "video" ) ) )
        {
            if( i_cat == -1 )
            {
                i_cat = fmt->i_cat == VIDEO_ES ? 1 : 0;
            }
        }
        else if( !strncmp( psz, "spu", strlen( "spu" ) ) )
        {
            if( i_cat == -1 )
            {
                i_cat = fmt->i_cat == SPU_ES ? 1 : 0;
            }
        }
        else if( strchr( psz, '=' ) != NULL )
        {
            char *psz_arg = strchr( psz, '=' );
            *psz_arg++ = '\0';

            if( !strcmp( psz, "no-es" ) || !strcmp( psz, "noes" ) )
            {
                if( i_es == -1 )
                {
                    i_es = NumInRange( psz_arg, fmt->i_id ) ? 0 : -1;
                }
            }
            else if( !strcmp( psz, "es" ) )
            {
                if( i_es == -1 )
                {
                    i_es = NumInRange( psz_arg, fmt->i_id) ? 1 : -1;
                }
            }
            else if( !strcmp( psz, "no-prgm" ) || !strcmp( psz, "noprgm" ) ||
                      !strcmp( psz, "no-program" ) || !strcmp( psz, "noprogram" ) )
            {
                if( fmt->i_group >= 0 && i_prgm == -1 )
                {
                    i_prgm = NumInRange( psz_arg, fmt->i_group ) ? 0 : -1;
                }
            }
            else if( !strcmp( psz, "prgm" ) || !strcmp( psz, "program" ) )
            {
                if( fmt->i_group >= 0 && i_prgm == -1 )
                {
                    i_prgm = NumInRange( psz_arg, fmt->i_group ) ? 1 : -1;
                }
            }
        }
        else
        {
            vlc_error( logger, "unknown args (%s)", psz );
        }
        /* Next */
        psz = p;
    }

    free( psz_dup );

    if( i_cat == 1 || i_es == 1 || i_prgm == 1 )
    {
        return true;
    }
    return false;
}
