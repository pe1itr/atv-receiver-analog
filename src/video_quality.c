#include "video_quality.h"
#include <math.h>
#include <string.h>

static void invalidate(VideoQualityReading *r)
{
    r->valid=0; r->lines=0;
    r->video_snr_db=r->noise_rms=r->sync_depth=r->sync_depth_error=NAN;
}
void video_quality_init(VideoQuality *q,double rate)
{
    memset(q,0,sizeof(*q)); q->rate=rate;
    invalidate(&q->reading); q->reading.status=VQ_COLLECTING;
}
void video_quality_reset(VideoQuality *q)
{
    unsigned sequence=q->reading.sequence;
    double rate=q->rate;
    video_quality_init(q,rate);
    q->reading.sequence=sequence+1;
}
const char *video_quality_status(const VideoQualityReading *r)
{
    switch(r->status) {
    case VQ_OK: return "ok";
    case VQ_NO_SYNC: return "no_sync";
    case VQ_FEW_LINES: return "insufficient_lines";
    case VQ_NO_NOISE: return "noise_below_measurement_floor";
    case VQ_BAD_DEPTH: return "invalid_sync_depth";
    case VQ_UNCERTAIN: return "uncertain_sync_depth";
    default: return "collecting";
    }
}
static void finish_line(VideoQuality *q)
{
    unsigned count=q->counts[1];
    if(count<4 || count<q->rate*.8e-6) return;
    if(q->window_sync && q->counts[0]>=q->rate*1.6e-6) {
        /* Pool the quiet samples before removing the mean. Per-line mean
         * removal would hide low-frequency noise and inflate video SNR. */
        double delta=q->means[1]-q->porch_mean;
        uint64_t combined=q->samples+count;
        q->noise_m2+=q->m2[1]+delta*delta*q->samples*count/combined;
        q->porch_mean+=delta*count/combined; q->samples=combined;
        ++q->lines;
        double depth=q->means[1]-q->means[0];
        double depth_delta=depth-q->depth_mean;
        q->depth_mean+=depth_delta/q->lines;
        q->depth_m2+=depth_delta*(depth-q->depth_mean);
    }
}
static void publish(VideoQuality *q,int sync_valid)
{
    VideoQualityReading *r=&q->reading;
    invalidate(r); ++r->sequence;
    r->lines=q->lines;
    double variance=q->samples>1?fmax(0,q->noise_m2/(q->samples-1)):0;
    if(q->samples>1) r->noise_rms=sqrt(variance);
    if(q->lines) r->sync_depth=q->depth_mean;
    if(q->lines>1) r->sync_depth_error=sqrt(fmax(0,q->depth_m2/(q->lines-1)/q->lines));
    if(!sync_valid) r->status=VQ_NO_SYNC;
    else if(q->lines<128) r->status=VQ_FEW_LINES;
    else if(!(variance>1e-20)) r->status=VQ_NO_NOISE;
    else if(!(r->sync_depth>0)) r->status=VQ_BAD_DEPTH;
    /* Validate the uncertainty of the mean across independent lines, not
     * the instantaneous noise amplitude. The former depth > 2*noise gate
     * silently discarded every valid low-SNR result below 13.38 dB. */
    else if(r->sync_depth<=5*r->sync_depth_error) r->status=VQ_UNCERTAIN;
    else {
        r->video_snr_db=20*log10((7.0/3)*r->sync_depth/r->noise_rms);
        r->valid=1; r->status=VQ_OK;
    }
    q->noise_m2=q->porch_mean=q->depth_mean=q->depth_m2=0;
    q->samples=0; q->lines=0; q->elapsed=0;
}
void video_quality_process(VideoQuality *q,const float *video,size_t n,
                           uint64_t first_tick,int sync_valid,
                           double origin,double period,double field_origin,double field_period)
{
    if(!n || !(q->rate>0) || !(period>0)) return;
    if(!sync_valid) {
        /* Reject this block and the partial line, but do not throw away
         * already accepted lines on every brief loss of clock confidence.
         * The stricter quality gate can flicker while display sync coasts. */
        q->have_line=0;
        if(q->reading.status!=VQ_NO_SYNC) {
            invalidate(&q->reading); q->reading.status=VQ_NO_SYNC; ++q->reading.sequence;
        }
        q->unsynced+=n; q->elapsed+=n;
        /* Never carry old lines through a long loss. A measurement may
         * wait for the next good block at 250 ms, but lasts at most 500 ms. */
        if(q->unsynced>=q->rate*.25 || q->elapsed>=q->rate*.5) {
            q->noise_m2=q->porch_mean=q->depth_mean=q->depth_m2=0;
            q->samples=0; q->lines=0;
            publish(q,0); q->unsynced=0;
        }
        return;
    }
    q->unsynced=0;
    if(q->reading.status==VQ_NO_SYNC) {
        /* On reacquisition, report a sufficiently populated recent window
         * instead of hiding it behind another full collection interval.
         * publish still enforces positive depth and its confidence bound. */
        if(q->lines>=128) publish(q,1);
        else { q->reading.status=VQ_COLLECTING; ++q->reading.sequence; }
    }
    double begin=(double)first_tick, end=begin+n;
    double line=origin+floor((begin-origin)/period)*period;
    while(line<end) {
        if(!q->have_line || fabs(line-q->line_start)>.25*period) {
            if(q->have_line) finish_line(q);
            memset(q->means,0,sizeof(q->means)); memset(q->m2,0,sizeof(q->m2));
            memset(q->counts,0,sizeof(q->counts));
            q->line_start=line; q->have_line=1; q->window_sync=sync_valid;
        }
        /* Omit the vertical blanking interval. */
        double field_phase=field_period>0?fmod(line-field_origin,field_period):q->rate*.01;
        if(field_phase<0) field_phase+=field_period;
        if(field_phase>=q->rate*.0016 && field_phase<q->rate*.0185) {
            const double starts[]={1.5e-6,9e-6},ends[]={3.5e-6,10e-6};
            for(unsigned gate=0;gate<2;++gate) {
                double lo=ceil(line+starts[gate]*q->rate), hi=ceil(line+ends[gate]*q->rate);
                size_t from=(size_t)fmax(0,fmin(n,lo-begin));
                size_t to=(size_t)fmax(0,fmin(n,hi-begin));
                for(size_t j=from;j<to;++j) {
                    double v=video[j];
                    if(!isfinite(v)) { q->window_sync=0; continue; }
                    ++q->counts[gate];
                    double delta=v-q->means[gate];
                    q->means[gate]+=delta/q->counts[gate];
                    q->m2[gate]+=delta*(v-q->means[gate]);
                }
            }
        }
        line+=period;
    }
    q->elapsed+=n;
    if(q->elapsed>=q->rate*.25) publish(q,sync_valid);
}
