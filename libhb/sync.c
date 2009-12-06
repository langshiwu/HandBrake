/* $Id: sync.c,v 1.38 2005/04/14 21:57:58 titer Exp $

   This file is part of the HandBrake source code.
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License. */

#include "hb.h"
#include "hbffmpeg.h"
#include <stdio.h>
#include "samplerate.h"

#ifdef INT64_MIN
#undef INT64_MIN /* Because it isn't defined correctly in Zeta */
#endif
#define INT64_MIN (-9223372036854775807LL-1)

#define AC3_SAMPLES_PER_FRAME 1536

typedef struct
{
    hb_lock_t * mutex;
    int         ref;        /* Reference count to tell us when it's unused */
    int         count_frames;
    int64_t     audio_passthru_slip;
    int64_t     video_pts_slip;
} hb_sync_common_t;

typedef struct
{
    int64_t      next_start;    /* start time of next output frame */
    int64_t      next_pts;     /* start time of next input frame */
    int64_t      first_drop;   /* PTS of first 'went backwards' frame dropped */
    int          drop_count;   /* count of 'time went backwards' drops */

    /* Raw */
    SRC_STATE  * state;
    SRC_DATA     data;

    /* AC-3 */
    int          ac3_size;
    uint8_t    * ac3_buf;
} hb_sync_audio_t;

typedef struct
{
    /* Video */
    int64_t    pts_offset;
    int64_t    pts_skip;
    int64_t    next_start;    /* start time of next output frame */
    int64_t    next_pts;      /* start time of next input frame */
    int64_t    first_drop;    /* PTS of first 'went backwards' frame dropped */
    int        drop_count;    /* count of 'time went backwards' drops */
    int        drops;         /* frames dropped to make a cbr video stream */
    int        dups;          /* frames duplicated to make a cbr video stream */
    int        video_sequence;
    int        count_frames_max;
    int        chap_mark;     /* to propagate chapter mark across a drop */
    hb_buffer_t * cur;        /* The next picture to process */

    /* Statistics */
    uint64_t   st_counts[4];
    uint64_t   st_dates[4];
    uint64_t   st_first;
} hb_sync_video_t;

struct hb_work_private_s
{
    hb_job_t * job;
    hb_sync_common_t * common;
    union
    {
        hb_sync_video_t video;
        hb_sync_audio_t audio;
    } type;
};

/***********************************************************************
 * Local prototypes
 **********************************************************************/
static void InitAudio( hb_job_t * job, hb_sync_common_t * common, int i );
static void InsertSilence( hb_work_object_t * w, int64_t d );
static void UpdateState( hb_work_object_t * w );
static hb_buffer_t * OutputAudioFrame( hb_audio_t *audio, hb_buffer_t *buf,
                                       hb_sync_audio_t *sync );

/***********************************************************************
 * hb_work_sync_init
 ***********************************************************************
 * Initialize the work object
 **********************************************************************/
int hb_sync_init( hb_job_t * job )
{
    hb_title_t        * title = job->title;
    hb_chapter_t      * chapter;
    int                 i;
    uint64_t            duration;
    hb_work_private_t * pv;
    hb_sync_video_t   * sync;
    hb_work_object_t  * w;

    pv = calloc( 1, sizeof( hb_work_private_t ) );
    sync = &pv->type.video;
    pv->common = calloc( 1, sizeof( hb_sync_common_t ) );
    pv->common->ref++;
    pv->common->mutex = hb_lock_init();

    w = hb_get_work( WORK_SYNC_VIDEO );
    w->private_data = pv;
    w->fifo_in = job->fifo_raw;
    w->fifo_out = job->fifo_sync;

    pv->job            = job;
    sync->pts_offset   = INT64_MIN;

    if( job->pass == 2 )
    {
        /* We already have an accurate frame count from pass 1 */
        hb_interjob_t * interjob = hb_interjob_get( job->h );
        sync->count_frames_max = interjob->frame_count;
    }
    else
    {
        /* Calculate how many video frames we are expecting */
        if ( job->pts_to_stop )
        {
            duration = job->pts_to_stop + 90000;
        }
        else if( job->frame_to_stop )
        {
            /* Set the duration to a rough estimate */
            duration = ( job->frame_to_stop / ( title->rate / title->rate_base ) ) * 90000;
        }
        else
        {
            duration = 0;
            for( i = job->chapter_start; i <= job->chapter_end; i++ )
            {
                chapter   = hb_list_item( title->list_chapter, i - 1 );
                duration += chapter->duration;
            }
            duration += 90000;
            /* 1 second safety so we're sure we won't miss anything */
        }
        sync->count_frames_max = duration * title->rate / title->rate_base / 90000;
    }
    hb_list_add( job->list_work, w );

    hb_log( "sync: expecting %d video frames", sync->count_frames_max );

    /* Initialize libsamplerate for every audio track we have */
    if ( ! job->indepth_scan )
    {
        for( i = 0; i < hb_list_count( title->list_audio ) && i < 8; i++ )
        {
            InitAudio( job, pv->common, i );
        }
    }

    return 0;
}

/***********************************************************************
 * Close Video
 ***********************************************************************
 *
 **********************************************************************/
void syncVideoClose( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;
    hb_job_t          * job   = pv->job;
    hb_sync_video_t   * sync = &pv->type.video;

    if( sync->cur )
    {
        hb_buffer_close( &sync->cur );
    }

    hb_log( "sync: got %d frames, %d expected",
            pv->common->count_frames, sync->count_frames_max );

    /* save data for second pass */
    if( job->pass == 1 )
    {
        /* Preserve frame count for better accuracy in pass 2 */
        hb_interjob_t * interjob = hb_interjob_get( job->h );
        interjob->frame_count = pv->common->count_frames;
        interjob->last_job = job->sequence_id;
        interjob->total_time = sync->next_start;
    }

    if (sync->drops || sync->dups )
    {
        hb_log( "sync: %d frames dropped, %d duplicated", 
                sync->drops, sync->dups );
    }

    hb_lock( pv->common->mutex );
    if ( --pv->common->ref == 0 )
    {
        hb_unlock( pv->common->mutex );
        hb_lock_close( &pv->common->mutex );
        free( pv->common );
    }
    else
    {
        hb_unlock( pv->common->mutex );
    }

    free( pv );
    w->private_data = NULL;
}

/***********************************************************************
 * syncVideoWork
 ***********************************************************************
 *
 **********************************************************************/
int syncVideoWork( hb_work_object_t * w, hb_buffer_t ** buf_in,
              hb_buffer_t ** buf_out )
{
    hb_buffer_t * cur, * next, * sub = NULL;
    hb_work_private_t * pv = w->private_data;
    hb_job_t          * job = pv->job;
    hb_subtitle_t     * subtitle;
    hb_sync_video_t   * sync = &pv->type.video;
    int i;

    *buf_out = NULL;
    if( !sync->cur )
    {
        sync->cur = *buf_in;
        *buf_in = NULL;
        if( sync->cur->size == 0 )
        {
            /* we got an end-of-stream as our first video packet? 
             * Feed it downstream & signal that we're done. 
             */
            *buf_out = hb_buffer_init( 0 );

            /*
             * Push through any subtitle EOFs in case they 
             * were not synced through.
             */
            for( i = 0; i < hb_list_count( job->list_subtitle ); i++)
            {
                subtitle = hb_list_item( job->list_subtitle, i );
                if( subtitle->config.dest == PASSTHRUSUB )
                {
                    hb_fifo_push( subtitle->fifo_out, hb_buffer_init( 0 ) );
                }
            }
            return HB_WORK_DONE;
        }
        return HB_WORK_OK;
    }
    next = *buf_in;
    *buf_in = NULL;
    cur = sync->cur;
    if( job->frame_to_stop && pv->common->count_frames > job->frame_to_stop )
    {
        // Drop an empty buffer into our output to ensure that things
        // get flushed all the way out.
        hb_buffer_close( &sync->cur );
        hb_buffer_close( &next );
        *buf_out = hb_buffer_init( 0 );
        hb_log( "sync: reached %d frames, exiting early",
                pv->common->count_frames );
        return HB_WORK_DONE;
    }

    /* At this point we have a frame to process. Let's check
        1) if we will be able to push into the fifo ahead
        2) if the next frame is there already, since we need it to
           compute the duration of the current frame*/
    if( next->size == 0 )
    {
        hb_buffer_close( &next );

        cur->start = sync->next_start;
        cur->stop = cur->start + 90000. / ((double)job->vrate / (double)job->vrate_base);

        /* Push the frame to the renderer */
        hb_fifo_push( job->fifo_sync, cur );
        sync->cur = NULL;

        /* we got an end-of-stream. Feed it downstream & signal that
         * we're done. Note that this means we drop the final frame of
         * video (we don't know its duration). On DVDs the final frame
         * is often strange and dropping it seems to be a good idea. */
        *buf_out = hb_buffer_init( 0 );

        /*
         * Push through any subtitle EOFs in case they were not synced through.
         */
        for( i = 0; i < hb_list_count( job->list_subtitle ); i++)
        {
            subtitle = hb_list_item( job->list_subtitle, i );
            if( subtitle->config.dest == PASSTHRUSUB )
            {
                if( subtitle->source == VOBSUB ) 
                    hb_fifo_push( subtitle->fifo_sync, hb_buffer_init( 0 ) );
                else
                    hb_fifo_push( subtitle->fifo_out, hb_buffer_init( 0 ) );
            }
        }
        return HB_WORK_DONE;
    }
    if( sync->pts_offset == INT64_MIN )
    {
        /* This is our first frame */
        sync->pts_offset = 0;
        if ( cur->start != 0 )
        {
            /*
             * The first pts from a dvd should always be zero but
             * can be non-zero with a transport or program stream since
             * we're not guaranteed to start on an IDR frame. If we get
             * a non-zero initial PTS extend its duration so it behaves
             * as if it started at zero so that our audio timing will
             * be in sync.
             */
            hb_log( "sync: first pts is %"PRId64, cur->start );
            cur->start = 0;
        }
    }

    /*
     * since the first frame is always 0 and the upstream reader code
     * is taking care of adjusting for pts discontinuities, we just have
     * to deal with the next frame's start being in the past. This can
     * happen when the PTS is adjusted after data loss but video frame
     * reordering causes some frames with the old clock to appear after
     * the clock change. This creates frames that overlap in time which
     * looks to us like time going backward. The downstream muxing code
     * can deal with overlaps of up to a frame time but anything larger
     * we handle by dropping frames here.
     */
    hb_lock( pv->common->mutex );
    if ( (int64_t)( next->start - pv->common->video_pts_slip - cur->start ) <= 0 )
    {
        if ( sync->first_drop == 0 )
        {
            sync->first_drop = next->start;
        }
        ++sync->drop_count;
        if (next->start - cur->start > 0)
        {
            sync->pts_skip += next->start - cur->start;
            pv->common->video_pts_slip -= next->start - cur->start;
        }
        hb_unlock( pv->common->mutex );
        if ( next->new_chap )
        {
            // don't drop a chapter mark when we drop the buffer
            sync->chap_mark = next->new_chap;
        }
        hb_buffer_close( &next );
        return HB_WORK_OK;
    }
    hb_unlock( pv->common->mutex );
    if ( sync->first_drop )
    {
        hb_log( "sync: video time didn't advance - dropped %d frames "
                "(delta %d ms, current %"PRId64", next %"PRId64", dur %d)",
                sync->drop_count, (int)( cur->start - sync->first_drop ) / 90,
                cur->start, next->start, (int)( next->start - cur->start ) );
        sync->first_drop = 0;
        sync->drop_count = 0;
    }

    /*
     * Track the video sequence number localy so that we can sync the audio
     * to it using the sequence number as well as the PTS.
     */
    sync->video_sequence = cur->sequence;

    /*
     * Look for a subtitle for this frame.
     *
     * If found then it will be tagged onto a video buffer of the correct time and 
     * sent in to the render pipeline. This only needs to be done for VOBSUBs which
     * get rendered, other types of subtitles can just sit in their raw_queue until
     * delt with at muxing.
     */
    for( i = 0; i < hb_list_count( job->list_subtitle ); i++)
    {
        subtitle = hb_list_item( job->list_subtitle, i );

        /*
         * Rewrite timestamps on subtitles that need it (on raw queue).
         */
        if( subtitle->source == CC608SUB ||
            subtitle->source == CC708SUB ||
            subtitle->source == SRTSUB )
        {
            /*
             * Rewrite timestamps on subtitles that came from Closed Captions
             * since they are using the MPEG2 timestamps.
             */
            while( ( sub = hb_fifo_see( subtitle->fifo_raw ) ) )
            {
                /*
                 * Rewrite the timestamps as and when the video
                 * (cur->start) reaches the same timestamp as a
                 * closed caption (sub->start).
                 *
                 * What about discontinuity boundaries - not delt
                 * with here - Van?
                 *
                 * Bypass the sync fifo altogether.
                 */
                if( sub->size <= 0 )
                {
                    sub = hb_fifo_get( subtitle->fifo_raw );
                    hb_fifo_push( subtitle->fifo_out, sub );
                    sub = NULL;
                    break;
                } else {
                    /*
                     * Sync the subtitles to the incoming video, and use
                     * the matching converted video timestamp.
                     *
                     * Note that it doesn't appear that we need to convert 
                     * timestamps, I guess that they were already correct,
                     * so just push them through for rendering.
                     *
                     */
                    if( sub->start < cur->start )
                    {
                        sub = hb_fifo_get( subtitle->fifo_raw );
                        hb_fifo_push( subtitle->fifo_out, sub );
                    } else {
                        sub = NULL;
                        break;
                    }
                }
            }
        }

        if( subtitle->source == VOBSUB ) 
        {
            hb_buffer_t * sub2;
            while( ( sub = hb_fifo_see( subtitle->fifo_raw ) ) )
            {
                if( sub->size == 0 )
                {
                    /*
                     * EOF, pass it through immediately.
                     */
                    break;
                }

                /* If two subtitles overlap, make the first one stop
                   when the second one starts */
                sub2 = hb_fifo_see2( subtitle->fifo_raw );
                if( sub2 && sub->stop > sub2->start )
                {
                    sub->stop = sub2->start;
                }
                
                // hb_log("0x%x: video seq: %lld  subtitle sequence: %lld",
                //       sub, cur->sequence, sub->sequence);
                
                if( sub->sequence > cur->sequence )
                {
                    /*
                     * The video is behind where we are, so wait until
                     * it catches up to the same reader point on the
                     * DVD. Then our PTS should be in the same region
                     * as the video.
                     */
                    sub = NULL;
                    break;
                }
                
                if( sub->stop > cur->start ) {
                    /*
                     * The stop time is in the future, so fall through
                     * and we'll deal with it in the next block of
                     * code.
                     */

                    /*
                     * There is a valid subtitle, is it time to display it?
                     */
                    if( sub->stop > sub->start)
                    {
                        /*
                         * Normal subtitle which ends after it starts, 
                         * check to see that the current video is between 
                         * the start and end.
                         */
                        if( cur->start > sub->start &&
                            cur->start < sub->stop )
                        {
                            /*
                            * We should be playing this, so leave the
                            * subtitle in place.
                            *
                            * fall through to display
                            */
                            if( ( sub->stop - sub->start ) < ( 2 * 90000 ) )
                            {
                                /*
                                 * Subtitle is on for less than three 
                                 * seconds, extend the time that it is 
                                 * displayed to make it easier to read. 
                                 * Make it 3 seconds or until the next
                                 * subtitle is displayed.
                                 *
                                 * This is in response to Indochine which 
                                 * only displays subs for 1 second - 
                                 * too fast to read.
                                 */
                                sub->stop = sub->start + ( 2 * 90000 );
                            
                                sub2 = hb_fifo_see2( subtitle->fifo_raw );
                            
                                if( sub2 && sub->stop > sub2->start )
                                {
                                    sub->stop = sub2->start;
                                }
                            }
                        }
                        else
                        {
                            /*
                             * Defer until the play point is within 
                             * the subtitle
                             */
                            sub = NULL;
                        }
                    }
                    else
                    {
                        /*
                         * The end of the subtitle is less than the start, 
                         * this is a sign of a PTS discontinuity.
                         */
                        if( sub->start > cur->start )
                        {
                            /*
                             * we haven't reached the start time yet, or
                             * we have jumped backwards after having
                             * already started this subtitle.
                             */
                            if( cur->start < sub->stop )
                            {
                                /*
                                 * We have jumped backwards and so should
                                 * continue displaying this subtitle.
                                 *
                                 * fall through to display.
                                 */
                            }
                            else
                            {
                                /*
                                 * Defer until the play point is 
                                 * within the subtitle
                                 */
                                sub = NULL;
                            }
                        } else {
                            /*
                            * Play this subtitle as the start is 
                            * greater than our video point.
                            *
                            * fall through to display/
                            */
                        }
                    }
                	break;
                }
                else
                {
                    
                    /*
                     * The subtitle is older than this picture, trash it
                     */
                    sub = hb_fifo_get( subtitle->fifo_raw );
                    hb_buffer_close( &sub );
                }
            }
            
            /* If we have a subtitle for this picture, copy it */
            /* FIXME: we should avoid this memcpy */
            if( sub )
            {
                if( sub->size > 0 )
                {
                    if( subtitle->config.dest == RENDERSUB )
                    {
                        if ( cur->sub == NULL )
                        {
                            /*
                             * Tack onto the video buffer for rendering
                             */
                            cur->sub         = hb_buffer_init( sub->size );
                            cur->sub->x      = sub->x;
                            cur->sub->y      = sub->y;
                            cur->sub->width  = sub->width;
                            cur->sub->height = sub->height;
                            memcpy( cur->sub->data, sub->data, sub->size ); 
                        }
                    } else {
                        /*
                         * Pass-Through, pop it off of the raw queue, 
                         */
                        sub = hb_fifo_get( subtitle->fifo_raw );
                        hb_fifo_push( subtitle->fifo_sync, sub );
                    }
                } else {
                    /*
                    * EOF - consume for rendered, else pass through
                    */
                    if( subtitle->config.dest == RENDERSUB )
                    {
                        sub = hb_fifo_get( subtitle->fifo_raw );
                        hb_buffer_close( &sub );
                    } else {
                        sub = hb_fifo_get( subtitle->fifo_raw );
                        hb_fifo_push( subtitle->fifo_sync, sub );
                    }
                }
            }
        }
    } // end subtitles

    /*
     * Adjust the pts of the current frame so that it's contiguous
     * with the previous frame. The start time of the current frame
     * has to be the end time of the previous frame and the stop
     * time has to be the start of the next frame.  We don't
     * make any adjustments to the source timestamps other than removing
     * the clock offsets (which also removes pts discontinuities).
     * This means we automatically encode at the source's frame rate.
     * MP2 uses an implicit duration (frames end when the next frame
     * starts) but more advanced containers like MP4 use an explicit
     * duration. Since we're looking ahead one frame we set the
     * explicit stop time from the start time of the next frame.
     */
    *buf_out = cur;
    sync->cur = cur = next;
    cur->sub = NULL;
    sync->next_pts = cur->start;
    int64_t duration = cur->start - sync->pts_skip - (*buf_out)->start;
    sync->pts_skip = 0;
    if ( duration <= 0 )
    {
        hb_log( "sync: invalid video duration %"PRId64", start %"PRId64", next %"PRId64"",
                duration, (*buf_out)->start, next->start );
    }

    (*buf_out)->start = sync->next_start;
    sync->next_start += duration;
    (*buf_out)->stop = sync->next_start;

    if ( sync->chap_mark )
    {
        // we have a pending chapter mark from a recent drop - put it on this
        // buffer (this may make it one frame late but we can't do any better).
        (*buf_out)->new_chap = sync->chap_mark;
        sync->chap_mark = 0;
    }

    /* Update UI */
    UpdateState( w );
        
    return HB_WORK_OK;
}

// sync*Init does nothing because sync has a special initializer
// that takes care of initializing video and all audio tracks
int syncVideoInit( hb_work_object_t * w, hb_job_t * job)
{
    return 0;
}

hb_work_object_t hb_sync_video =
{
    WORK_SYNC_VIDEO,
    "Video Synchronization",
    syncVideoInit,
    syncVideoWork,
    syncVideoClose
};

/***********************************************************************
 * Close Audio
 ***********************************************************************
 *
 **********************************************************************/
void syncAudioClose( hb_work_object_t * w )
{
    hb_work_private_t * pv    = w->private_data;
    hb_sync_audio_t   * sync  = &pv->type.audio;

    if( w->audio->config.out.codec == HB_ACODEC_AC3 )
    {
        free( sync->ac3_buf );
    }
    else
    {
        src_delete( sync->state );
    }

    hb_lock( pv->common->mutex );
    if ( --pv->common->ref == 0 )
    {
        hb_unlock( pv->common->mutex );
        hb_lock_close( &pv->common->mutex );
        free( pv->common );
    }
    else
    {
        hb_unlock( pv->common->mutex );
    }

    free( pv );
    w->private_data = NULL;
}

int syncAudioInit( hb_work_object_t * w, hb_job_t * job)
{
    return 0;
}

/***********************************************************************
 * SyncAudio
 ***********************************************************************
 *
 **********************************************************************/
static int syncAudioWork( hb_work_object_t * w, hb_buffer_t ** buf_in,
                       hb_buffer_t ** buf_out )
{
    hb_work_private_t * pv = w->private_data;
    hb_job_t        * job = pv->job;
    hb_sync_audio_t * sync = &pv->type.audio;
    hb_buffer_t     * buf;
    //hb_fifo_t       * fifo;
    int64_t start;

    *buf_out = NULL;
    buf = *buf_in;
    *buf_in = NULL;
    hb_lock( pv->common->mutex );
    start = buf->start - pv->common->audio_passthru_slip;
    hb_unlock( pv->common->mutex );
    /* if the next buffer is an eof send it downstream */
    if ( buf->size <= 0 )
    {
        hb_buffer_close( &buf );
        *buf_out = hb_buffer_init( 0 );
        return HB_WORK_DONE;
    }
    if( job->frame_to_stop && pv->common->count_frames >= job->frame_to_stop )
    {
        hb_buffer_close( &buf );
        *buf_out = hb_buffer_init( 0 );
        return HB_WORK_DONE;
    }
    if ( (int64_t)( start - sync->next_pts ) < 0 )
    {
        // audio time went backwards.
        // If our output clock is more than a half frame ahead of the
        // input clock drop this frame to move closer to sync.
        // Otherwise drop frames until the input clock matches the output clock.
        if ( sync->first_drop || sync->next_start - start > 90*15 )
        {
            // Discard data that's in the past.
            if ( sync->first_drop == 0 )
            {
                sync->first_drop = sync->next_pts;
            }
            ++sync->drop_count;
            hb_buffer_close( &buf );
            return HB_WORK_OK;
        }
        sync->next_pts = start;
    }
    if ( sync->first_drop )
    {
        // we were dropping old data but input buf time is now current
        hb_log( "sync: audio %d time went backwards %d ms, dropped %d frames "
                "(next %"PRId64", current %"PRId64")", w->audio->id,
                (int)( sync->next_pts - sync->first_drop ) / 90,
                sync->drop_count, sync->first_drop, sync->next_pts );
        sync->first_drop = 0;
        sync->drop_count = 0;
        sync->next_pts = start;
    }
    if ( start - sync->next_pts >= (90 * 70) )
    {
        if ( start - sync->next_pts > (90000LL * 60) )
        {
            // there's a gap of more than a minute between the last
            // frame and this. assume we got a corrupted timestamp
            // and just drop the next buf.
            hb_log( "sync: %d minute time gap in audio %d - dropping buf"
                    "  start %"PRId64", next %"PRId64,
                    (int)((start - sync->next_pts) / (90000*60)),
                    w->audio->id, start, sync->next_pts );
            hb_buffer_close( &buf );
            return HB_WORK_OK;
        }
        /*
         * there's a gap of at least 70ms between the last
         * frame we processed & the next. Fill it with silence.
         * Or in the case of DCA, skip some frames from the
         * other streams.
         */
        if( w->audio->config.out.codec == HB_ACODEC_DCA )
        {
            hb_log( "sync: audio gap %d ms. Skipping frames. Audio %d"
                    "  start %"PRId64", next %"PRId64,
                    (int)((start - sync->next_pts) / 90),
                    w->audio->id, start, sync->next_pts );
            hb_lock( pv->common->mutex );
            pv->common->audio_passthru_slip += (start - sync->next_pts);
            pv->common->video_pts_slip += (start - sync->next_pts);
            hb_unlock( pv->common->mutex );
            *buf_out = buf;
            return HB_WORK_OK;
        }
        hb_log( "sync: adding %d ms of silence to audio %d"
                "  start %"PRId64", next %"PRId64,
                (int)((start - sync->next_pts) / 90),
                w->audio->id, start, sync->next_pts );
        InsertSilence( w, start - sync->next_pts );
        *buf_out = buf;
        return HB_WORK_OK;
    }

    /*
     * When we get here we've taken care of all the dups and gaps in the
     * audio stream and are ready to inject the next input frame into
     * the output stream.
     */
    *buf_out = OutputAudioFrame( w->audio, buf, sync );
    return HB_WORK_OK;
}

hb_work_object_t hb_sync_audio =
{
    WORK_SYNC_AUDIO,
    "AudioSynchronization",
    syncAudioInit,
    syncAudioWork,
    syncAudioClose
};

static void InitAudio( hb_job_t * job, hb_sync_common_t * common, int i )
{
    hb_work_object_t  * w;
    hb_work_private_t * pv;
    hb_title_t        * title = job->title;
    hb_sync_audio_t   * sync;

    pv = calloc( 1, sizeof( hb_work_private_t ) );
    sync = &pv->type.audio;
    pv->job    = job;
    pv->common = common;
    pv->common->ref++;

    w = hb_get_work( WORK_SYNC_AUDIO );
    w->private_data = pv;
    w->audio = hb_list_item( title->list_audio, i );
    w->fifo_in = w->audio->priv.fifo_raw;

    if( w->audio->config.out.codec == HB_ACODEC_AC3 ||
        w->audio->config.out.codec == HB_ACODEC_DCA )
    {
        w->fifo_out = w->audio->priv.fifo_out;
    }
    else
    {
        w->fifo_out = w->audio->priv.fifo_sync;
    }

    if( w->audio->config.out.codec == HB_ACODEC_AC3 )
    {
        /* Have a silent AC-3 frame ready in case we have to fill a
           gap */
        AVCodec        * codec;
        AVCodecContext * c;
        short          * zeros;

        codec = avcodec_find_encoder( CODEC_ID_AC3 );
        c     = avcodec_alloc_context();

        c->bit_rate    = w->audio->config.in.bitrate;
        c->sample_rate = w->audio->config.in.samplerate;
        c->channels    = HB_INPUT_CH_LAYOUT_GET_DISCRETE_COUNT( w->audio->config.in.channel_layout );

        if( hb_avcodec_open( c, codec ) < 0 )
        {
            hb_log( "sync: avcodec_open failed" );
            return;
        }

        zeros          = calloc( AC3_SAMPLES_PER_FRAME *
                                 sizeof( short ) * c->channels, 1 );
        sync->ac3_size = w->audio->config.in.bitrate * AC3_SAMPLES_PER_FRAME /
                             w->audio->config.in.samplerate / 8;
        sync->ac3_buf  = malloc( sync->ac3_size );

        if( avcodec_encode_audio( c, sync->ac3_buf, sync->ac3_size,
                                  zeros ) != sync->ac3_size )
        {
            hb_log( "sync: avcodec_encode_audio failed" );
        }

        free( zeros );
        hb_avcodec_close( c );
        av_free( c );
    }
    else
    {
        /* Initialize libsamplerate */
        int error;
        sync->state = src_new( SRC_SINC_MEDIUM_QUALITY, 
            HB_AMIXDOWN_GET_DISCRETE_CHANNEL_COUNT(
                w->audio->config.out.mixdown), &error );
        sync->data.end_of_input = 0;
    }
    hb_list_add( job->list_work, w );
}

static hb_buffer_t * OutputAudioFrame( hb_audio_t *audio, hb_buffer_t *buf,
                                       hb_sync_audio_t *sync )
{
    int64_t start = sync->next_start;
    int64_t duration = buf->stop - buf->start;

    sync->next_pts += duration;

    if( audio->config.in.samplerate == audio->config.out.samplerate ||
        audio->config.out.codec == HB_ACODEC_AC3 ||
        audio->config.out.codec == HB_ACODEC_DCA )
    {
        /*
         * If we don't have to do sample rate conversion or this audio is 
         * pass-thru just send the input buffer downstream after adjusting
         * its timestamps to make the output stream continuous.
         */
    }
    else
    {
        /* Not pass-thru - do sample rate conversion */
        int count_in, count_out;
        hb_buffer_t * buf_raw = buf;
        int channel_count = HB_AMIXDOWN_GET_DISCRETE_CHANNEL_COUNT(audio->config.out.mixdown) *
                            sizeof( float );

        count_in  = buf_raw->size / channel_count;
        /*
         * When using stupid rates like 44.1 there will always be some
         * truncation error. E.g., a 1536 sample AC3 frame will turn into a
         * 1536*44.1/48.0 = 1411.2 sample frame. If we just truncate the .2
         * the error will build up over time and eventually the audio will
         * substantially lag the video. libsamplerate will keep track of the
         * fractional sample & give it to us when appropriate if we give it
         * an extra sample of space in the output buffer.
         */
        count_out = ( duration * audio->config.out.samplerate ) / 90000 + 1;

        sync->data.input_frames = count_in;
        sync->data.output_frames = count_out;
        sync->data.src_ratio = (double)audio->config.out.samplerate /
                               (double)audio->config.in.samplerate;

        buf = hb_buffer_init( count_out * channel_count );
        sync->data.data_in  = (float *) buf_raw->data;
        sync->data.data_out = (float *) buf->data;
        if( src_process( sync->state, &sync->data ) )
        {
            /* XXX If this happens, we're screwed */
            hb_log( "sync: audio %d src_process failed", audio->id );
        }
        hb_buffer_close( &buf_raw );

        buf->size = sync->data.output_frames_gen * channel_count;
        duration = ( sync->data.output_frames_gen * 90000 ) /
                   audio->config.out.samplerate;
    }
    buf->frametype = HB_FRAME_AUDIO;
    buf->start = start;
    buf->stop  = start + duration;
    sync->next_start = start + duration;
    return buf;
}

static void InsertSilence( hb_work_object_t * w, int64_t duration )
{
    hb_work_private_t * pv = w->private_data;
    hb_sync_audio_t *sync = &pv->type.audio;
    hb_buffer_t     *buf;
    hb_fifo_t       *fifo;

    // to keep pass-thru and regular audio in sync we generate silence in
    // AC3 frame-sized units. If the silence duration isn't an integer multiple
    // of the AC3 frame duration we will truncate or round up depending on
    // which minimizes the timing error.
    const int frame_dur = ( 90000 * AC3_SAMPLES_PER_FRAME ) /
                          w->audio->config.in.samplerate;
    int frame_count = ( duration + (frame_dur >> 1) ) / frame_dur;

    while ( --frame_count >= 0 )
    {
        if( w->audio->config.out.codec == HB_ACODEC_AC3 )
        {
            buf        = hb_buffer_init( sync->ac3_size );
            buf->start = sync->next_pts;
            buf->stop  = buf->start + frame_dur;
            memcpy( buf->data, sync->ac3_buf, buf->size );
            fifo = w->audio->priv.fifo_out;
        }
        else
        {
            buf = hb_buffer_init( AC3_SAMPLES_PER_FRAME * sizeof( float ) *
                                     HB_AMIXDOWN_GET_DISCRETE_CHANNEL_COUNT(
                                         w->audio->config.out.mixdown) );
            buf->start = sync->next_pts;
            buf->stop  = buf->start + frame_dur;
            memset( buf->data, 0, buf->size );
            fifo = w->audio->priv.fifo_sync;
        }
        buf = OutputAudioFrame( w->audio, buf, sync );
        hb_fifo_push( fifo, buf );
    }
}

static void UpdateState( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;
    hb_sync_video_t   * sync = &pv->type.video;
    hb_state_t state;

    if( !pv->common->count_frames )
    {
        sync->st_first = hb_get_date();
        pv->job->st_pause_date = -1;
        pv->job->st_paused = 0;
    }
    pv->common->count_frames++;

    if( hb_get_date() > sync->st_dates[3] + 1000 )
    {
        memmove( &sync->st_dates[0], &sync->st_dates[1],
                 3 * sizeof( uint64_t ) );
        memmove( &sync->st_counts[0], &sync->st_counts[1],
                 3 * sizeof( uint64_t ) );
        sync->st_dates[3]  = hb_get_date();
        sync->st_counts[3] = pv->common->count_frames;
    }

#define p state.param.working
    state.state = HB_STATE_WORKING;
    p.progress  = (float) pv->common->count_frames / (float) sync->count_frames_max;
    if( p.progress > 1.0 )
    {
        p.progress = 1.0;
    }
    p.rate_cur   = 1000.0 *
        (float) ( sync->st_counts[3] - sync->st_counts[0] ) /
        (float) ( sync->st_dates[3] - sync->st_dates[0] );
    if( hb_get_date() > sync->st_first + 4000 )
    {
        int eta;
        p.rate_avg = 1000.0 * (float) sync->st_counts[3] /
            (float) ( sync->st_dates[3] - sync->st_first - pv->job->st_paused);
        eta = (float) ( sync->count_frames_max - sync->st_counts[3] ) /
            p.rate_avg;
        p.hours   = eta / 3600;
        p.minutes = ( eta % 3600 ) / 60;
        p.seconds = eta % 60;
    }
    else
    {
        p.rate_avg = 0.0;
        p.hours    = -1;
        p.minutes  = -1;
        p.seconds  = -1;
    }
#undef p

    hb_set_state( pv->job->h, &state );
}
