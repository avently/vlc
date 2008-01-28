/*****************************************************************************
 * mono.c : stereo2mono downmixsimple channel mixer plug-in
 *****************************************************************************
 * Copyright (C) 2006 M2X
 * $Id$
 *
 * Authors: Jean-Paul Saman <jpsaman at m2x dot nl>
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
#include <math.h>                                        /* sqrt */

#ifdef HAVE_STDINT_H
#   include <stdint.h>                                         /* int16_t .. */
#elif defined(HAVE_INTTYPES_H)
#   include <inttypes.h>                                       /* int16_t .. */
#endif

#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#endif

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc/vlc.h>
#include <vlc_es.h>
#include <vlc_block.h>
#include <vlc_filter.h>
#include <vlc_aout.h>

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  OpenFilter    ( vlc_object_t * );
static void CloseFilter   ( vlc_object_t * );

static block_t *Convert( filter_t *p_filter, block_t *p_block );

static unsigned int stereo_to_mono( aout_filter_t *, aout_buffer_t *,
                                    aout_buffer_t * );
static unsigned int mono( aout_filter_t *, aout_buffer_t *, aout_buffer_t * );
static void stereo2mono_downmix( aout_filter_t *, aout_buffer_t *,
                                 aout_buffer_t * );

/*****************************************************************************
 * Local structures
 *****************************************************************************/
struct atomic_operation_t
{
    int i_source_channel_offset;
    int i_dest_channel_offset;
    unsigned int i_delay;/* in sample unit */
    double d_amplitude_factor;
};

struct filter_sys_t
{
    vlc_bool_t b_downmix;

    unsigned int i_nb_channels; /* number of int16_t per sample */
    int i_channel_selected;
    int i_bitspersample;

    size_t i_overflow_buffer_size;/* in bytes */
    byte_t * p_overflow_buffer;
    unsigned int i_nb_atomic_operations;
    struct atomic_operation_t * p_atomic_operations;
};

#define MONO_DOWNMIX_TEXT N_("Use downmix algorithm")
#define MONO_DOWNMIX_LONGTEXT N_("This option selects a stereo to mono " \
    "downmix algorithm that is used in the headphone channel mixer. It" \
    "gives the effect of standing in a room full of speakers." )

#define MONO_CHANNEL_TEXT N_("Select channel to keep")
#define MONO_CHANNEL_LONGTEXT N_("This option silences all other channels " \
    "except the selected channel. Choose one from (0=left, 1=right, " \
    "2=rear left, 3=rear right, 4=center, 5=left front)")

static const int pi_pos_values[] = { 0, 1, 2, 4, 8, 5 };
static const char *ppsz_pos_descriptions[] =
{ N_("Left"), N_("Right"), N_("Left rear"), N_("Right rear"), N_("Center"),
  N_("Left front") };

/* our internal channel order (WG-4 order) */
static const uint32_t pi_channels_out[] =
{ AOUT_CHAN_LEFT, AOUT_CHAN_RIGHT, AOUT_CHAN_REARLEFT, AOUT_CHAN_REARRIGHT,
  AOUT_CHAN_CENTER, AOUT_CHAN_LFE, 0 };

#define MONO_CFG "sout-mono-"
/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
vlc_module_begin();
    set_description( _("Audio filter for stereo to mono conversion") );
    set_capability( "audio filter2", 0 );

    add_bool( MONO_CFG "downmix", VLC_FALSE, NULL, MONO_DOWNMIX_TEXT,
              MONO_DOWNMIX_LONGTEXT, VLC_FALSE );
    add_integer( MONO_CFG "channel", -1, NULL, MONO_CHANNEL_TEXT,
        MONO_CHANNEL_LONGTEXT, VLC_FALSE );
        change_integer_list( pi_pos_values, ppsz_pos_descriptions, 0 );

    set_category( CAT_AUDIO );
    set_subcategory( SUBCAT_AUDIO_MISC );
    set_callbacks( OpenFilter, CloseFilter );
    set_shortname( "Mono" );
vlc_module_end();

/* Init() and ComputeChannelOperations() -
 * Code taken from modules/audio_filter/channel_mixer/headphone.c
 * converted from float into int16_t based downmix
 * Written by Boris Dorès <babal@via.ecp.fr>
 */

/*****************************************************************************
 * Init: initialize internal data structures
 * and computes the needed atomic operations
 *****************************************************************************/
/* x and z represent the coordinates of the virtual speaker
 *  relatively to the center of the listener's head, measured in meters :
 *
 *  left              right
 *Z
 *-
 *a          head
 *x
 *i
 *s
 *  rear left    rear right
 *
 *          x-axis
 *  */
static void ComputeChannelOperations( struct filter_sys_t * p_data,
        unsigned int i_rate, unsigned int i_next_atomic_operation,
        int i_source_channel_offset, double d_x, double d_z,
        double d_compensation_length, double d_channel_amplitude_factor )
{
    double d_c = 340; /*sound celerity (unit: m/s)*/
    double d_compensation_delay = (d_compensation_length-0.1) / d_c * i_rate;

    /* Left ear */
    p_data->p_atomic_operations[i_next_atomic_operation]
        .i_source_channel_offset = i_source_channel_offset;
    p_data->p_atomic_operations[i_next_atomic_operation]
        .i_dest_channel_offset = 0;/* left */
    p_data->p_atomic_operations[i_next_atomic_operation]
        .i_delay = (int)( sqrt( (-0.1-d_x)*(-0.1-d_x) + (0-d_z)*(0-d_z) )
                          / d_c * i_rate - d_compensation_delay );
    if( d_x < 0 )
    {
        p_data->p_atomic_operations[i_next_atomic_operation]
            .d_amplitude_factor = d_channel_amplitude_factor * 1.1 / 2;
    }
    else if( d_x > 0 )
    {
        p_data->p_atomic_operations[i_next_atomic_operation]
            .d_amplitude_factor = d_channel_amplitude_factor * 0.9 / 2;
    }
    else
    {
        p_data->p_atomic_operations[i_next_atomic_operation]
            .d_amplitude_factor = d_channel_amplitude_factor / 2;
    }

    /* Right ear */
    p_data->p_atomic_operations[i_next_atomic_operation + 1]
        .i_source_channel_offset = i_source_channel_offset;
    p_data->p_atomic_operations[i_next_atomic_operation + 1]
        .i_dest_channel_offset = 1;/* right */
    p_data->p_atomic_operations[i_next_atomic_operation + 1]
        .i_delay = (int)( sqrt( (0.1-d_x)*(0.1-d_x) + (0-d_z)*(0-d_z) )
                          / d_c * i_rate - d_compensation_delay );
    if( d_x < 0 )
    {
        p_data->p_atomic_operations[i_next_atomic_operation + 1]
            .d_amplitude_factor = d_channel_amplitude_factor * 0.9 / 2;
    }
    else if( d_x > 0 )
    {
        p_data->p_atomic_operations[i_next_atomic_operation + 1]
            .d_amplitude_factor = d_channel_amplitude_factor * 1.1 / 2;
    }
    else
    {
        p_data->p_atomic_operations[i_next_atomic_operation + 1]
            .d_amplitude_factor = d_channel_amplitude_factor / 2;
    }
}

static int Init( vlc_object_t *p_this, struct filter_sys_t * p_data,
                 unsigned int i_nb_channels, uint32_t i_physical_channels,
                 unsigned int i_rate )
{
    double d_x = config_GetInt( p_this, "headphone-dim" );
    double d_z = d_x;
    double d_z_rear = -d_x/3;
    double d_min = 0;
    unsigned int i_next_atomic_operation;
    int i_source_channel_offset;
    unsigned int i;

    if( p_data == NULL )
    {
        msg_Dbg( p_this, "passing a null pointer as argument" );
        return 0;
    }

    if( config_GetInt( p_this, "headphone-compensate" ) )
    {
        /* minimal distance to any speaker */
        if( i_physical_channels & AOUT_CHAN_REARCENTER )
        {
            d_min = d_z_rear;
        }
        else
        {
            d_min = d_z;
        }
    }

    /* Number of elementary operations */
    p_data->i_nb_atomic_operations = i_nb_channels * 2;
    if( i_physical_channels & AOUT_CHAN_CENTER )
    {
        p_data->i_nb_atomic_operations += 2;
    }
    p_data->p_atomic_operations = malloc( sizeof(struct atomic_operation_t)
            * p_data->i_nb_atomic_operations );
    if( p_data->p_atomic_operations == NULL )
    {
        msg_Err( p_this, "out of memory" );
        return -1;
    }

    /* For each virtual speaker, computes elementary wave propagation time
     * to each ear */
    i_next_atomic_operation = 0;
    i_source_channel_offset = 0;
    if( i_physical_channels & AOUT_CHAN_LEFT )
    {
        ComputeChannelOperations( p_data , i_rate
                , i_next_atomic_operation , i_source_channel_offset
                , -d_x , d_z , d_min , 2.0 / i_nb_channels );
        i_next_atomic_operation += 2;
        i_source_channel_offset++;
    }
    if( i_physical_channels & AOUT_CHAN_RIGHT )
    {
        ComputeChannelOperations( p_data , i_rate
                , i_next_atomic_operation , i_source_channel_offset
                , d_x , d_z , d_min , 2.0 / i_nb_channels );
        i_next_atomic_operation += 2;
        i_source_channel_offset++;
    }
    if( i_physical_channels & AOUT_CHAN_MIDDLELEFT )
    {
        ComputeChannelOperations( p_data , i_rate
                , i_next_atomic_operation , i_source_channel_offset
                , -d_x , 0 , d_min , 1.5 / i_nb_channels );
        i_next_atomic_operation += 2;
        i_source_channel_offset++;
    }
    if( i_physical_channels & AOUT_CHAN_MIDDLERIGHT )
    {
        ComputeChannelOperations( p_data , i_rate
                , i_next_atomic_operation , i_source_channel_offset
                , d_x , 0 , d_min , 1.5 / i_nb_channels );
        i_next_atomic_operation += 2;
        i_source_channel_offset++;
    }
    if( i_physical_channels & AOUT_CHAN_REARLEFT )
    {
        ComputeChannelOperations( p_data , i_rate
                , i_next_atomic_operation , i_source_channel_offset
                , -d_x , d_z_rear , d_min , 1.5 / i_nb_channels );
        i_next_atomic_operation += 2;
        i_source_channel_offset++;
    }
    if( i_physical_channels & AOUT_CHAN_REARRIGHT )
    {
        ComputeChannelOperations( p_data , i_rate
                , i_next_atomic_operation , i_source_channel_offset
                , d_x , d_z_rear , d_min , 1.5 / i_nb_channels );
        i_next_atomic_operation += 2;
        i_source_channel_offset++;
    }
    if( i_physical_channels & AOUT_CHAN_REARCENTER )
    {
        ComputeChannelOperations( p_data , i_rate
                , i_next_atomic_operation , i_source_channel_offset
                , 0 , -d_z , d_min , 1.5 / i_nb_channels );
        i_next_atomic_operation += 2;
        i_source_channel_offset++;
    }
    if( i_physical_channels & AOUT_CHAN_CENTER )
    {
        /* having two center channels increases the spatialization effect */
        ComputeChannelOperations( p_data , i_rate
                , i_next_atomic_operation , i_source_channel_offset
                , d_x / 5.0 , d_z , d_min , 0.75 / i_nb_channels );
        i_next_atomic_operation += 2;
        ComputeChannelOperations( p_data , i_rate
                , i_next_atomic_operation , i_source_channel_offset
                , -d_x / 5.0 , d_z , d_min , 0.75 / i_nb_channels );
        i_next_atomic_operation += 2;
        i_source_channel_offset++;
    }
    if( i_physical_channels & AOUT_CHAN_LFE )
    {
        ComputeChannelOperations( p_data , i_rate
                , i_next_atomic_operation , i_source_channel_offset
                , 0 , d_z_rear , d_min , 5.0 / i_nb_channels );
        i_next_atomic_operation += 2;
        i_source_channel_offset++;
    }

    /* Initialize the overflow buffer
     * we need it because the process induce a delay in the samples */
    p_data->i_overflow_buffer_size = 0;
    for( i = 0 ; i < p_data->i_nb_atomic_operations ; i++ )
    {
        if( p_data->i_overflow_buffer_size
                < p_data->p_atomic_operations[i].i_delay * 2 * sizeof (int16_t) )
        {
            p_data->i_overflow_buffer_size
                = p_data->p_atomic_operations[i].i_delay * 2 * sizeof (int16_t);
        }
    }
    p_data->p_overflow_buffer = malloc( p_data->i_overflow_buffer_size );
    if( p_data->p_atomic_operations == NULL )
    {
        msg_Err( p_this, "out of memory" );
        return -1;
    }
    memset( p_data->p_overflow_buffer, 0, p_data->i_overflow_buffer_size );

    /* end */
    return 0;
}

/*****************************************************************************
 * OpenFilter
 *****************************************************************************/
static int OpenFilter( vlc_object_t *p_this )
{
    filter_t * p_filter = (filter_t *)p_this;
    filter_sys_t *p_sys = NULL;

    if( aout_FormatNbChannels( &(p_filter->fmt_in.audio) ) == 1 )
    {
        msg_Dbg( p_filter, "filter discarded (incompatible format)" );
        return VLC_EGENERIC;
    }

    if( (p_filter->fmt_in.i_codec != AOUT_FMT_S16_NE) ||
        (p_filter->fmt_out.i_codec != AOUT_FMT_S16_NE) )
    {
        msg_Err( p_this, "filter discarded (invalid format)" );
        return -1;
    }

    if( (p_filter->fmt_in.audio.i_format != p_filter->fmt_out.audio.i_format) &&
        (p_filter->fmt_in.audio.i_rate != p_filter->fmt_out.audio.i_rate) &&
        (p_filter->fmt_in.audio.i_format != AOUT_FMT_S16_NE) &&
        (p_filter->fmt_out.audio.i_format != AOUT_FMT_S16_NE) &&
        (p_filter->fmt_in.audio.i_bitspersample !=
                                    p_filter->fmt_out.audio.i_bitspersample))
    {
        msg_Err( p_this, "couldn't load mono filter" );
        return VLC_EGENERIC;
    }

    /* Allocate the memory needed to store the module's structure */
    p_sys = p_filter->p_sys = malloc( sizeof(filter_sys_t) );
    if( p_sys == NULL )
    {
        msg_Err( p_filter, "out of memory" );
        return VLC_EGENERIC;
    }

    var_Create( p_this, MONO_CFG "downmix",
                VLC_VAR_BOOL | VLC_VAR_DOINHERIT );
    p_sys->b_downmix = var_GetBool( p_this, MONO_CFG "downmix" );

    var_Create( p_this, MONO_CFG "channel",
                VLC_VAR_INTEGER | VLC_VAR_DOINHERIT );
    p_sys->i_channel_selected =
            (unsigned int) var_GetInteger( p_this, MONO_CFG "channel" );

    if( p_sys->b_downmix )
    {
        msg_Dbg( p_this, "using stereo to mono downmix" );
        p_filter->fmt_out.audio.i_physical_channels = AOUT_CHAN_CENTER;
        p_filter->fmt_out.audio.i_channels = 1;
    }
    else
    {
        msg_Dbg( p_this, "using pseudo mono" );
        p_filter->fmt_out.audio.i_physical_channels =
                            (AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT);
        p_filter->fmt_out.audio.i_channels = 2;
    }

    p_filter->fmt_out.audio.i_rate = p_filter->fmt_in.audio.i_rate;
    p_filter->fmt_out.audio.i_format = p_filter->fmt_out.i_codec;

    p_sys->i_nb_channels = aout_FormatNbChannels( &(p_filter->fmt_in.audio) );
    p_sys->i_bitspersample = p_filter->fmt_out.audio.i_bitspersample;

    p_sys->i_overflow_buffer_size = 0;
    p_sys->p_overflow_buffer = NULL;
    p_sys->i_nb_atomic_operations = 0;
    p_sys->p_atomic_operations = NULL;

    if( Init( VLC_OBJECT(p_filter), p_filter->p_sys,
              aout_FormatNbChannels( &p_filter->fmt_in.audio ),
              p_filter->fmt_in.audio.i_physical_channels,
              p_filter->fmt_in.audio.i_rate ) < 0 )
    {
        return VLC_EGENERIC;
    }

    p_filter->pf_audio_filter = Convert;

    msg_Dbg( p_this, "%4.4s->%4.4s, channels %d->%d, bits per sample: %i->%i",
             (char *)&p_filter->fmt_in.i_codec,
             (char *)&p_filter->fmt_out.i_codec,
             p_filter->fmt_in.audio.i_physical_channels,
             p_filter->fmt_out.audio.i_physical_channels,
             p_filter->fmt_in.audio.i_bitspersample,
             p_filter->fmt_out.audio.i_bitspersample );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * CloseFilter
 *****************************************************************************/
static void CloseFilter( vlc_object_t *p_this)
{
    filter_t *p_filter = (filter_t *) p_this;
    filter_sys_t *p_sys = p_filter->p_sys;

    var_Destroy( p_this, MONO_CFG "channel" );
    var_Destroy( p_this, MONO_CFG "downmix" );
    free( p_sys );
}

/*****************************************************************************
 * Convert
 *****************************************************************************/
static block_t *Convert( filter_t *p_filter, block_t *p_block )
{
    aout_filter_t aout_filter;
    aout_buffer_t in_buf, out_buf;
    block_t *p_out = NULL;
    unsigned int i_samples;
    int i_out_size;

    if( !p_block || !p_block->i_samples )
    {
        if( p_block )
            p_block->pf_release( p_block );
        return NULL;
    }

    i_out_size = p_block->i_samples * p_filter->p_sys->i_bitspersample/8 *
                 aout_FormatNbChannels( &(p_filter->fmt_out.audio) );

    p_out = p_filter->pf_audio_buffer_new( p_filter, i_out_size );
    if( !p_out )
    {
        msg_Warn( p_filter, "can't get output buffer" );
        p_block->pf_release( p_block );
        return NULL;
    }
    p_out->i_samples = (p_block->i_samples / p_filter->p_sys->i_nb_channels) *
                       aout_FormatNbChannels( &(p_filter->fmt_out.audio) );
    p_out->i_dts = p_block->i_dts;
    p_out->i_pts = p_block->i_pts;
    p_out->i_length = p_block->i_length;

    aout_filter.p_sys = (struct aout_filter_sys_t *)p_filter->p_sys;
    aout_filter.input = p_filter->fmt_in.audio;
    aout_filter.input.i_format = p_filter->fmt_in.i_codec;
    aout_filter.output = p_filter->fmt_out.audio;
    aout_filter.output.i_format = p_filter->fmt_out.i_codec;

    in_buf.p_buffer = p_block->p_buffer;
    in_buf.i_nb_bytes = p_block->i_buffer;
    in_buf.i_nb_samples = p_block->i_samples;

#if 0
    unsigned int i_in_size = in_buf.i_nb_samples  * (p_filter->p_sys->i_bitspersample/8) *
                             aout_FormatNbChannels( &(p_filter->fmt_in.audio) );
    if( (in_buf.i_nb_bytes != i_in_size) && ((i_in_size % 32) != 0) ) /* is it word aligned?? */
    {
        msg_Err( p_filter, "input buffer is not word aligned" );
        /* Fix output buffer to be word aligned */
    }
#endif

    out_buf.p_buffer = p_out->p_buffer;
    out_buf.i_nb_bytes = p_out->i_buffer;
    out_buf.i_nb_samples = p_out->i_samples;

    memset( p_out->p_buffer, 0, i_out_size );
    if( p_filter->p_sys->b_downmix )
    {
        stereo2mono_downmix( &aout_filter, &in_buf, &out_buf );
        i_samples = mono( &aout_filter, &out_buf, &in_buf );
    }
    else
    {
        i_samples = stereo_to_mono( &aout_filter, &out_buf, &in_buf );
    }

    p_out->i_buffer = out_buf.i_nb_bytes;
    p_out->i_samples = out_buf.i_nb_samples;

    p_block->pf_release( p_block );
    return p_out;
}

/* stereo2mono_downmix - stereo channels into one mono channel.
 * Code taken from modules/audio_filter/channel_mixer/headphone.c
 * converted from float into int16_t based downmix
 * Written by Boris Dorès <babal@via.ecp.fr>
 */
static void stereo2mono_downmix( aout_filter_t * p_filter,
                            aout_buffer_t * p_in_buf, aout_buffer_t * p_out_buf )
{
    filter_sys_t *p_sys = (filter_sys_t *)p_filter->p_sys;

    int i_input_nb = aout_FormatNbChannels( &p_filter->input );
    int i_output_nb = aout_FormatNbChannels( &p_filter->output );

    int16_t * p_in = (int16_t*) p_in_buf->p_buffer;
    byte_t * p_out;
    byte_t * p_overflow;
    byte_t * p_slide;

    size_t i_overflow_size;     /* in bytes */
    size_t i_out_size;          /* in bytes */

    unsigned int i, j;

    int i_source_channel_offset;
    int i_dest_channel_offset;
    unsigned int i_delay;
    double d_amplitude_factor;

    /* out buffer characterisitcs */
    p_out_buf->i_nb_samples = p_in_buf->i_nb_samples;
    p_out_buf->i_nb_bytes = p_in_buf->i_nb_bytes * i_output_nb / i_input_nb;
    p_out = p_out_buf->p_buffer;
    i_out_size = p_out_buf->i_nb_bytes;

    if( p_sys != NULL )
    {
        /* Slide the overflow buffer */
        p_overflow = p_sys->p_overflow_buffer;
        i_overflow_size = p_sys->i_overflow_buffer_size;

        if ( i_out_size > i_overflow_size )
            memcpy( p_out, p_overflow, i_overflow_size );
        else
            memcpy( p_out, p_overflow, i_out_size );

        p_slide = p_sys->p_overflow_buffer;
        while( p_slide < p_overflow + i_overflow_size )
        {
            if( p_slide + i_out_size < p_overflow + i_overflow_size )
            {
                memset( p_slide, 0, i_out_size );
                if( p_slide + 2 * i_out_size < p_overflow + i_overflow_size )
                    memcpy( p_slide, p_slide + i_out_size, i_out_size );
                else
                    memcpy( p_slide, p_slide + i_out_size,
                            p_overflow + i_overflow_size - ( p_slide + i_out_size ) );
            }
            else
            {
                memset( p_slide, 0, p_overflow + i_overflow_size - p_slide );
            }
            p_slide += i_out_size;
        }

        /* apply the atomic operations */
        for( i = 0; i < p_sys->i_nb_atomic_operations; i++ )
        {
            /* shorter variable names */
            i_source_channel_offset
                = p_sys->p_atomic_operations[i].i_source_channel_offset;
            i_dest_channel_offset
                = p_sys->p_atomic_operations[i].i_dest_channel_offset;
            i_delay = p_sys->p_atomic_operations[i].i_delay;
            d_amplitude_factor
                = p_sys->p_atomic_operations[i].d_amplitude_factor;

            if( p_out_buf->i_nb_samples > i_delay )
            {
                /* current buffer coefficients */
                for( j = 0; j < p_out_buf->i_nb_samples - i_delay; j++ )
                {
                    ((int16_t*)p_out)[ (i_delay+j)*i_output_nb + i_dest_channel_offset ]
                        += p_in[ j * i_input_nb + i_source_channel_offset ]
                           * d_amplitude_factor;
                }

                /* overflow buffer coefficients */
                for( j = 0; j < i_delay; j++ )
                {
                    ((int16_t*)p_overflow)[ j*i_output_nb + i_dest_channel_offset ]
                        += p_in[ (p_out_buf->i_nb_samples - i_delay + j)
                           * i_input_nb + i_source_channel_offset ]
                           * d_amplitude_factor;
                }
            }
            else
            {
                /* overflow buffer coefficients only */
                for( j = 0; j < p_out_buf->i_nb_samples; j++ )
                {
                    ((int16_t*)p_overflow)[ (i_delay - p_out_buf->i_nb_samples + j)
                        * i_output_nb + i_dest_channel_offset ]
                        += p_in[ j * i_input_nb + i_source_channel_offset ]
                           * d_amplitude_factor;
                }
            }
        }
    }
    else
    {
        memset( p_out, 0, i_out_size );
    }
}

/* Simple stereo to mono mixing. */
static unsigned int mono( aout_filter_t *p_filter,
                          aout_buffer_t *p_output, aout_buffer_t *p_input )
{
    filter_sys_t *p_sys = (filter_sys_t *)p_filter->p_sys;
    int16_t *p_in, *p_out;
    unsigned int n = 0, r = 0;

    p_in = (int16_t *) p_input->p_buffer;
    p_out = (int16_t *) p_output->p_buffer;

    while( n < (p_input->i_nb_samples * p_sys->i_nb_channels) )
    {
        p_out[r] = (p_in[n] + p_in[n+1]) >> 1;
        r++;
        n += 2;
    }
    return r;
}

/* Simple stereo to mono mixing. */
static unsigned int stereo_to_mono( aout_filter_t *p_filter,
                                    aout_buffer_t *p_output, aout_buffer_t *p_input )
{
    filter_sys_t *p_sys = (filter_sys_t *)p_filter->p_sys;
    int16_t *p_in, *p_out;
    unsigned int n;

    p_in = (int16_t *) p_input->p_buffer;
    p_out = (int16_t *) p_output->p_buffer;

    for( n = 0; n < (p_input->i_nb_samples * p_sys->i_nb_channels); n++ )
    {
        /* Fake real mono. */
        if( p_sys->i_channel_selected == -1)
        {
            p_out[n] = p_out[n+1] = (p_in[n] + p_in[n+1]) >> 1;
            n++;
        }
        else if( (n % p_sys->i_nb_channels) == (unsigned int) p_sys->i_channel_selected )
        {
            p_out[n] = p_out[n+1] = p_in[n];
        }
    }
    return n;
}
