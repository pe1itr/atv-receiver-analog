#include "video_scope.h"
#include <float.h>
#include <math.h>
#include <string.h>

void video_scope_init(VideoScope *s,double rate,double line_hz)
{
    memset(s,0,sizeof(*s)); s->rate=rate;
    video_scope_reset(s,line_hz);
}
void video_scope_reset(VideoScope *s,double line_hz)
{
    s->count=s->skip=s->partial=0;
    s->partial_sum=0;
    s->line_hz=line_hz;
    /* Average about 2 us per sample to fit a full PAL field plus trigger margins. */
    s->stride=s->field?(size_t)fmax(1,round(s->rate*2e-6)):1;
    s->wanted=(size_t)ceil((s->field?.022:3/line_hz)*s->rate/s->stride);
    if(s->wanted>VIDEO_SCOPE_CAPACITY) s->wanted=VIDEO_SCOPE_CAPACITY;
    memset(&s->reading,0,sizeof(s->reading));
}
static void acquire(VideoScope *s)
{
    /* Smooth only the trigger detector; keep the displayed samples intact.
     * A pulse trigger indicates a plausible horizontal pulse, not PAL lock. */
    float smooth[VIDEO_SCOPE_CAPACITY], lo=FLT_MAX, hi=-FLT_MAX;
    double rate=s->rate/s->stride;
    size_t window=(size_t)fmax(1,round(rate*.3e-6));
    double sum=0;
    for(size_t k=0;k<s->wanted;++k) {
        sum+=s->samples[k];
        if(k>=window) sum-=s->samples[k-window];
        smooth[k]=(float)(sum/(k<window?k+1:window));
        lo=fminf(lo,smooth[k]); hi=fmaxf(hi,smooth[k]);
    }
    size_t span=(size_t)llround((s->field?16:2)*rate/s->line_hz);
    size_t pre=(size_t)llround(rate*(s->field?.00032:4e-6));
    size_t start=0,fall=0;
    int low=0,triggered=0;
    float threshold=lo+.25f*(hi-lo);
    if(hi-lo>1e-6f) for(size_t k=1;k<s->wanted;++k) {
        if(!low && smooth[k]<threshold && smooth[k-1]>=threshold) { fall=k; low=1; }
        if(low && smooth[k]>=threshold) {
            double width=(k-fall)/rate;
            if(width>=(s->field?20e-6:3e-6) && width<=(s->field?31e-6:7e-6) && fall>=pre && fall-pre+span<=s->wanted) {
                start=fall-pre; triggered=1; break;
            }
            low=0;
        }
    }
    if(s->field && !triggered) span=s->wanted;
    if(span>s->wanted) span=s->wanted;
    VideoScopeReading *r=&s->reading;
    r->triggered=triggered;
    r->start_us=triggered?-(double)pre/rate*1e6:0;
    r->duration_us=span/rate*1e6;
    r->minimum=FLT_MAX; r->maximum=-FLT_MAX;
    for(size_t x=0;x<VIDEO_SCOPE_POINTS;++x) {
        size_t begin=start+x*span/VIDEO_SCOPE_POINTS;
        size_t end=start+(x+1)*span/VIDEO_SCOPE_POINTS;
        if(end<=begin) end=begin+1;
        r->low[x]=FLT_MAX; r->high[x]=-FLT_MAX;
        for(size_t k=begin;k<end;++k) {
            r->low[x]=fminf(r->low[x],s->samples[k]);
            r->high[x]=fmaxf(r->high[x],s->samples[k]);
        }
        r->minimum=fminf(r->minimum,r->low[x]);
        r->maximum=fmaxf(r->maximum,r->high[x]);
    }
    ++r->sequence;
}
int video_scope_process(VideoScope *s,const float *video,size_t count)
{
    if(s->skip) {
        size_t skip=s->skip<count?s->skip:count;
        s->skip-=skip; video+=skip; count-=skip;
    }
    size_t needed=(s->wanted-s->count)*s->stride-s->partial;
    size_t take=count<needed?count:needed;
    if(!s->field) {
        for(size_t k=0;k<take;++k)
            s->samples[s->count++]=isfinite(video[k])?video[k]:0;
    } else {
        for(size_t k=0;k<take;) {
            size_t n=s->stride-s->partial;
            if(n>take-k) n=take-k;
            for(size_t end=k+n;k<end;++k)
                s->partial_sum+=isfinite(video[k])?video[k]:0;
            s->partial+=n;
            if(s->partial==s->stride) {
                s->samples[s->count++]=(float)(s->partial_sum/s->stride);
                s->partial=0;
                s->partial_sum=0;
            }
        }
    }
    if(s->count<s->wanted) return 0;
    acquire(s);
    s->count=0;
    /* Include acquisition time in the 40 ms update interval. */
    size_t interval=(size_t)(s->rate*.04);
    size_t consumed=s->wanted*s->stride+count-take;
    s->skip=interval>consumed?interval-consumed:0;
    return 1;
}
