#include "atv.h"
#include <math.h>
#include "fm_math.h"
#include <string.h>
#if defined(__SSE2__) && !defined(FM1_NO_SIMD)
#include <emmintrin.h>
#endif


/* Explicit bounds let the compiler inline these hot per-pixel operations.
 * Preserve fmin/fmax's handling of NaN (white sync clamp, black video). */
static inline float sync_clip(float v) { return !(v<1)?1:v < -1?-1:v; }
static inline float video_clip(float v) { return !(v>0)?0:v>1?1:v; }

/* Only prefer an existing path while its timing has recent confirmation.
 * After three missed fields ordinary acquisition can select a new path. */
static int echo_tracking(const Decoder *d)
{
    return d->experimental_sync && d->hsync_updates>=4 &&
        d->fields-d->hsync_field<=3 && d->free_period>0;
}

/* Predict the next line from the recovered oscillator. Once acquired, filter
 * timing innovations instead of copying each noisy field fit into the raster.
 * Keep acquisition and recovery after a fade fast. */
static void horizontal_clock(Decoder *d, double origin, double period)
{
    if(echo_tracking(d) &&
       fabs(remainder(origin-d->free_origin,d->free_period))>=d->rate*2e-6)
        return;
    if(d->hsync_updates>=4 && d->fields-d->hsync_field<=3) {
        double error=remainder(origin-d->free_origin,d->free_period);
        if(fabs(error)<d->rate*2e-6) {
            origin=origin-error+.25*error;
            period=d->free_period+.125*(period-d->free_period);
        }
    }
    d->free_origin=origin;
    d->free_period=period;
    ++d->hsync_updates;
}

/* Weak FM often fragments individual pulses. Fold many lines, independently
 * in three parts of the field, and require the same negative sync trough. */
static void folded_horizontal(Decoder *d, double period)
{
    double phase[3];
    for(int part=0;part<3;++part) {
        double profile[128], score[128];
        for(int k=0;k<128;++k) {
            if(d->hsync_samples[part][k]<8) return;
            profile[k]=d->hsync_sum[part][k]/d->hsync_samples[part][k];
        }
        int best=0;
        for(int k=0;k<128;++k) {
            score[k]=0;
            for(int b=-4;b<=4;++b) score[k]+=profile[(k+b+128)%128]/9;
            if(score[k]<score[best]) best=k;
        }
        if(echo_tracking(d)) {
            int nearby=-1;
            for(int k=0;k<128;++k) {
                double onset=(k+.5)*period/128-2.35e-6*d->rate;
                if(fabs(remainder(onset-d->free_origin,period))<d->rate*2e-6 &&
                   (nearby<0 || score[k]<score[nearby])) nearby=k;
            }
            /* Keep the old path only if it remains a substantial trough.
             * Absolute contrast and independent thirds still apply below. */
            if(nearby>=0 && score[nearby]<=.75*score[best]) best=nearby;
        }
        double sides=0;
        for(int b=9;b<=17;++b)
            sides+=(profile[(best+b)%128]+profile[(best-b+128)%128])/18;
        if(score[best]>-.035 || sides-score[best]<.06) return;
        double left=score[(best+127)%128], right=score[(best+1)%128];
        double curvature=left+right-2*score[best];
        double fraction=curvature>1e-9 ? .5*(left-right)/curvature : 0;
        phase[part]=(best+.5+fraction)*period/128-2.35e-6*d->rate;
        if(part) phase[part]=phase[0]+remainder(phase[part]-phase[0],period);
    }
    /* Reject unrelated minima and nonlinear phase changes in noise. */
    if(fabs(phase[2]-2*phase[1]+phase[0])>period/64) return;
    double span=d->rate*.020/3;
    double slope=(phase[2]-phase[0])/(2*span);
    double next_period=period/(1-slope);
    if(fabs(next_period/(d->rate/d->line_hz)-1)>=.002) return;
    double at_end=(phase[0]+phase[1]+phase[2])/3+slope*d->rate*.010;
    horizontal_clock(d,d->tick+remainder(at_end-d->tick,period)/(1-slope),next_period);
}

/* Recover horizontal phase and rate from a field's consensus of sync pulses.
 * Isolated edges must not move the raster; absent consensus, coast through fades.
 * Work on pulse timing before clipping sync/blanking out of the displayed image. */
static void free_horizontal(Decoder *d)
{
    unsigned updates=d->hsync_updates;
    double period = d->free_period ? d->free_period : d->rate/d->line_hz;
    unsigned bins[128] = {0}, best = 0;
    int centre = 0;
    for (unsigned j=0;j<d->hsync_count;++j)
        ++bins[(int)(fmod(d->hsync_pulses[j],period)*128/period)%128];
    for (int k=0;k<128;++k) {
        unsigned count=0;
        for(int b=-4;b<=4;++b) count+=bins[(k+b+128)%128];
        if(count>best) { best=count; centre=k; }
    }
    int tracking=echo_tracking(d);
    if(tracking) {
        unsigned nearby=0;
        int near_centre=centre;
        for(int k=0;k<128;++k) {
            if(fabs(remainder((k+.5)*period/128-d->free_origin,period))>=d->rate*2e-6)
                continue;
            unsigned count=0;
            for(int b=-4;b<=4;++b) count+=bins[(k+b+128)%128];
            if(count>nearby) { nearby=count; near_centre=k; }
        }
        if(nearby>=24 && nearby>=.75*best) { best=nearby; centre=near_centre; }
    }
    if(best>=24 && best>=(tracking?.4:.6)*d->hsync_count) {
        double phase=(centre+.5)*period/128, sx=0, sy=0, sxx=0, sxy=0, syy=0;
        unsigned n=0;
        double first=0,last=0;
        for(unsigned j=0;j<d->hsync_count;++j) {
            double y=remainder(d->hsync_pulses[j]-phase,period);
            if(fabs(y)>4.5*period/128) continue;
            double x=(double)d->hsync_pulses[j]-d->tick;
            if(!n) first=x;
            last=x; ++n; sx+=x; sy+=y; sxx+=x*x; sxy+=x*y; syy+=y*y;
        }
        double variance=n?sxx-sx*sx/n:0;
        if(n>=24 && last-first>.008*d->rate && variance>0) {
            double slope=(sxy-sx*sy/n)/variance;
            double intercept=(sy-slope*sx)/n;
            double error=fmax(0,(syy-intercept*sy-slope*sxy)/n);
            double next_period=period/(1-slope);
            if(sqrt(error)<.8e-6*d->rate &&
               fabs(next_period/(d->rate/d->line_hz)-1)<.002) {
                horizontal_clock(d,d->tick+remainder(phase+intercept-d->tick,period)/(1-slope),next_period);
            }
        }
    }
    if(updates==d->hsync_updates) folded_horizontal(d,period);
    d->hsync_count=0;
    memset(d->hsync_sum,0,sizeof(d->hsync_sum));
    memset(d->hsync_samples,0,sizeof(d->hsync_samples));
}

void decoder_init(Decoder *d, double rate, double deviation, int invert, FrameFn fn, void *opaque)
{
    memset(d, 0, sizeof(*d));
    d->line_hz = 15625; d->rate = rate; d->sample_time=1/rate; d->deviation = deviation; d->invert = invert;
    d->frame = fn; d->opaque = opaque; d->row = -1;
    for (int j = 0; j < WIDTH * HEIGHT; ++j) d->pixels[j] = 0xff000000;
}

static void vertical_candidate(Decoder *d, uint64_t onset)
{
    double nominal=d->rate/50, period=d->vsync_period?d->vsync_period:nominal;
    double interval=(double)onset-d->vsync_last;
    int steps=(int)llround(interval/period);
    /* Acquisition requires three consecutive sync groups. Once acquired,
     * a group after a fade must agree with the predicted phase. */
    int consistent=d->vsync_count && steps>=1 && steps<=100 &&
        fabs(interval-steps*period)<d->rate*80e-6;
    if(!consistent || (!d->vsync_updates && steps!=1)) d->vsync_count=0;
    if(d->vsync_count==8) {
        memmove(d->vsync_times,d->vsync_times+1,7*sizeof(double));
        --d->vsync_count;
    }
    d->vsync_times[d->vsync_count++]=onset;
    d->vsync_last=onset;
    if(d->vsync_count<3) return;
    double sx=0,sy=0,sxx=0,sxy=0;
    for(unsigned k=0;k<d->vsync_count;++k) {
        double y=d->vsync_times[k]-d->vsync_times[0];
        double x=round(y/period);
        sx+=x; sy+=y; sxx+=x*x; sxy+=x*y;
    }
    double n=d->vsync_count, denom=sxx-sx*sx/n;
    if(denom<=0) return;
    double fitted=(sxy-sx*sy/n)/denom;
    if(fitted<d->rate/51 || fitted>d->rate/49) return;
    double origin=d->vsync_times[0]+(sy-fitted*sx)/n+
        round((onset-d->vsync_times[0])/period)*fitted;
    double phase_error=remainder(origin-d->field,period);
    if(!d->vsync_updates || fabs(phase_error)>d->rate*128e-6) {
        ++d->vsync_epoch;
        d->complete_lines=0; d->row=-1; d->have_line=0;
        for(int k=0;k<WIDTH*HEIGHT;++k) d->pixels[k]=d->guide_pixels[k]=0xff000000u;
        d->hsync_count=0;
        memset(d->hsync_sum,0,sizeof(d->hsync_sum));
        memset(d->hsync_samples,0,sizeof(d->hsync_samples));
    }
    else if(d->vsync_count>=8) {
        /* The broad-pulse detector has 8 us bins. Do not turn bin changes
         * into immediate vertical picture movement. */
        origin=origin-phase_error+.25*phase_error;
        fitted=period+.125*(fitted-period);
    }
    d->field=(uint64_t)llround(origin);
    d->vsync_period=fitted;
    d->vsync_confirmed=onset;
    ++d->vsync_updates;
}

/* Average the broad PAL sync group before thresholding, so fragmented pulses
 * can still establish field timing. Compare 160 us with both surrounding
 * porches; horizontal sync alone cannot satisfy this long negative trough. */
static void vertical_sample(Decoder *d, float v)
{
    if(d->experimental_sync) {
        double period=d->vsync_period?d->vsync_period:d->rate/50;
        if(d->tick-d->vsync_scan_tick>=period) {
            /* Observe a whole field before choosing: an early dark picture
             * feature must not hide the actual broad sync in a blind window.
             * Keep the original three-field timing confirmation afterwards. */
            if(d->vsync_scan_best) vertical_candidate(d,d->vsync_scan_peak);
            d->vsync_scan_tick=d->tick;
            d->vsync_scan_best=0;
        }
    }
    d->vsync_sum+=sync_clip(v); ++d->vsync_samples;
    if(d->tick-d->vsync_bin_tick<d->vsync_bin_samples) return;
    d->vsync_bin_tick=d->tick;
    d->vsync_bins[d->vsync_pos]=d->vsync_sum/d->vsync_samples;
    d->vsync_pos=(d->vsync_pos+1)%64;
    d->vsync_sum=0; d->vsync_samples=0;
    if(d->vsync_filled<64) ++d->vsync_filled;
    if(d->vsync_filled<36) return;
    double middle=0,sides=0;
    for(int k=0;k<36;++k) {
        double value=d->vsync_bins[(d->vsync_pos+63-k)%64];
        if(k>=8 && k<28) middle+=value/20;
        else sides+=value/16;
    }
    double score=sides-middle;
    /* A narrow sync IF attenuates the broad pulse relative to its porches.
     * Once line timing is independently confirmed, accept a smaller contrast;
     * retain the negative-level test and three-field timing confirmation. */
    double threshold=d->hsync_updates && d->fields-d->hsync_field<=2 ? .035 : .06;
    if(middle<-.06 && score>threshold && score>d->vsync_best &&
       (d->experimental_sync || !d->vsync_event || d->tick-d->vsync_event>d->rate*.015)) {
        d->vsync_best=score;
        d->vsync_peak=d->tick-(uint64_t)llround(d->rate*224e-6);
    }
    if(d->vsync_best && d->tick-d->vsync_peak>d->rate*400e-6) {
        if(d->experimental_sync) {
            if(d->vsync_best>d->vsync_scan_best) {
                d->vsync_scan_best=d->vsync_best;
                d->vsync_scan_peak=d->vsync_peak;
            }
        } else vertical_candidate(d,d->vsync_peak);
        d->vsync_event=d->tick;
        d->vsync_best=0;
    }
}

/* Separate raster clocks share one demodulated video stream. The preview
 * deliberately never follows received sync, even while stacking acquires it. */
static void free_raster_clock(Decoder *d, int recover_sync)
{
    double field_period=recover_sync && d->vsync_period ? d->vsync_period : d->rate/50;
    if(field_period!=d->raster_field_period) {
        d->raster_field_period=field_period;
        d->raster_field_ticks=(uint64_t)llround(field_period);
    }
    if (d->tick - d->field >= d->raster_field_ticks) {
        /* Free-running fields can be averaged without received sync.
         * This indicates raster completeness, not signal quality.
         * The stack separately checks structure and image alignment. */
        d->stack_valid = d->complete_lines >= 280;
        /* Local parity is not transmitter interlace parity. Registration
         * must determine alignment from image content in free sync. */
        d->stack_phase = -1;
        ++d->fields;
        unsigned updates=d->hsync_updates;
        uint64_t last_sync=d->hsync_field;
        if(recover_sync) free_horizontal(d);
        /* This raster predates a phase correction after acquisition
         * or a fade. It must not seed a new horizontal reference. */
        if(d->hsync_updates!=updates) {
            if(!updates || d->fields-last_sync>3) d->stack_valid=0;
            d->hsync_field=d->fields;
        }
        if (d->frame) d->frame(d->opaque,d->pixels);
        d->field += d->raster_field_ticks;
        d->complete_lines = 0;
        if(!d->no_video_raster) for(int k=0;k<WIDTH*HEIGHT;++k) d->pixels[k]=0xff000000;
        if(d->guided) for(int k=0;k<WIDTH*HEIGHT;++k) d->guide_pixels[k]=0xff000000;
    }
    if(d->no_video_raster && !d->color) return;
    double lp=d->free_period ? d->free_period : d->nominal_line_period;
    if(d->have_line && d->tick<d->raster_next_line &&
       lp==d->raster_period && d->free_origin==d->raster_origin) return;
    double start=d->free_origin+floor((d->tick-d->free_origin)/lp)*lp;
    d->raster_period=lp; d->raster_origin=d->free_origin;
    d->raster_next_line=(uint64_t)fmax(0,ceil(start+lp));
    uint64_t line=(uint64_t)fmax(0,start);
    if (!d->have_line || line != d->line) {
        d->line=line; d->have_line=1; d->column=0;
        d->porch_sum=0; d->porch_count=0;
        d->row=(int)floor((double)(d->tick-d->field)/lp)-20+d->vertical_shift;
    }
}

static void raster_sample(Decoder *d, float v, float guide, const PalDecoder *color)
{
    if (!d->have_line) return;
    double t = (d->tick - d->line) * d->sample_time;
    if (t >= 7e-6 && t < 9e-6) { d->porch_sum += v; ++d->porch_count; }
    /* At lower sample rates the last sample may precede the final pixel
     * centre. Complete its held sample interval at the active-video edge. */
    if (t>=62.5e-6 && d->row>=0 && d->row<HEIGHT && d->column>0 && d->column<WIDTH) {
        uint32_t last=d->pixels[d->row*WIDTH+d->column-1];
        uint32_t last_guide=d->guide_pixels[d->row*WIDTH+d->column-1];
        while(d->column<WIDTH) {
            int k=d->row*WIDTH+d->column++;
            d->pixels[k]=last;
            if(d->guided) d->guide_pixels[k]=last_guide;
        }
        ++d->complete_lines;
    }
    if (t < 10.5e-6 || t >= 62.5e-6 || d->row < 0 || d->row >= HEIGHT) return;
    /* A free-running porch can lie inside white lettering. Clamping to
     * it erases that line's content before registration can recover it.
     * Use the tuned FM black reference when no received sync is used. */
    d->blank = !d->free_sync && d->porch_count ? d->porch_sum / d->porch_count : 0;
    int x = (int)((t - 10.5e-6) * (WIDTH / 52e-6));
    if(d->column>x || d->column>=WIDTH) return;
    int g = color?0:(int)lrintf(video_clip((v-d->blank)*(1.f/.7f))*255);
    /* Fill all crossed pixels, including when the input rate is low. */
    unsigned gg=d->guided?(unsigned)lrintf(video_clip(guide*(1.f/.7f))*255):0;
    uint32_t rgb=color?pal_rgb(color):0;
    int old_column = d->column;
    while (d->column <= x && d->column < WIDTH) {
        int k=d->row*WIDTH+d->column++;
        d->pixels[k]=d->color?rgb:0xff000000u | (unsigned)g*0x010101u;
        if(d->guided) d->guide_pixels[k]=0xff000000u | gg*0x010101u;
    }
    if (old_column < WIDTH && d->column == WIDTH) ++d->complete_lines;
}

/* Initial monochrome decoder: fixed sync threshold relative to tuned carrier,
 * back-porch black clamp, broad-pulse field sync, per-line horizontal retrigger.
 * A field is displayed as 288 lines (bob); no interlace weaving or chroma yet. */
void fm_demod_init(FMDemod *d, double rate, double deviation, int invert, unsigned stride)
{
    memset(d,0,sizeof(*d));
    d->stride=stride?stride:1;
    d->scale=(float)(rate/(2*PI*deviation)*(invert?-1:1))/d->stride;
    d->alpha=1-exp(-2*PI*2500000*d->stride/rate);
}
/* Eight-point windowed FFT phase detector following Linrad fmfix().
 * The common positive power multiplier in Linrad's atan2 cancels.
 * Unnormalised sin^2 window: its common gain likewise cancels.
 * Both receiver paths run FM1 at full rate, with identical window delay. */
static inline IQ iq_add(IQ a, IQ b) { return (IQ){a.i+b.i,a.q+b.q}; }
static inline IQ iq_sub(IQ a, IQ b) { return (IQ){a.i-b.i,a.q-b.q}; }
static inline IQ iq_scale(IQ a, float w) { return (IQ){a.i*w,a.q*w}; }
static inline IQ iq_j(IQ a) { return (IQ){-a.q,a.i}; }
static IQ fm1_phase(const FMDemod *d)
{
    /* Fixed radix-2 FFT: omit the zero window tap, bit-reversal table,
     * loop/index arithmetic, and multiplications by zero/one. Keep the
     * butterfly arithmetic order and Linrad's positive FFT convention. */
    const float lo=.1464466094f,hi=.8535533906f,h=.7071067812f;
    unsigned p=d->history_pos;
    IQ a=d->history[(p+4)&7];
    IQ b=iq_scale(d->history[(p+2)&7],.5f);
    IQ c=iq_scale(d->history[(p+6)&7],.5f);
    IQ e=iq_scale(d->history[(p+1)&7],lo);
    IQ f=iq_scale(d->history[(p+5)&7],hi);
    IQ g=iq_scale(d->history[(p+3)&7],hi);
    IQ k=iq_scale(d->history[(p+7)&7],lo);
    IQ bc=iq_add(b,c),bj=iq_j(iq_sub(b,c));
    IQ neg_a={-a.i,-a.q};
    IQ even[4]={iq_add(a,bc),iq_add(neg_a,bj),iq_sub(a,bc),iq_sub(neg_a,bj)};
    IQ ef=iq_add(e,f),ed=iq_sub(e,f),gk=iq_add(g,k),gj=iq_j(iq_sub(g,k));
    IQ odd0=iq_add(ef,gk),odd1=iq_add(ed,gj),odd2=iq_j(iq_sub(ef,gk)),odd3=iq_sub(ed,gj);
    IQ rot1={h*odd1.i-h*odd1.q,h*odd1.i+h*odd1.q};
    IQ rot3={-h*odd3.i-h*odd3.q,h*odd3.i-h*odd3.q};
    IQ x[8]={iq_add(even[0],odd0),iq_add(even[1],rot1),iq_add(even[2],odd2),iq_add(even[3],rot3),
             iq_sub(even[0],odd0),iq_sub(even[1],rot1),iq_sub(even[2],odd2),iq_sub(even[3],rot3)};
    float power[8];
    for(unsigned j=0;j<8;++j) power[j]=x[j].i*x[j].i+x[j].q*x[j].q;
    /* Tournament selection avoids a series of unpredictable branches in
     * noise. Ties select the first bin, as in the generic FFT detector. */
    unsigned b0=power[1]>power[0]?1:0,b1=power[3]>power[2]?3:2;
    unsigned b2=power[5]>power[4]?5:4,b3=power[7]>power[6]?7:6;
    b0=power[b1]>power[b0]?b1:b0;
    b2=power[b3]>power[b2]?b3:b2;
    unsigned best=power[b2]>power[b0]?b2:b0;
    unsigned left=(best+7)&7,right=(best+1)&7;
    float sign=(left&1)?-1:1;
    return (IQ){sign*(x[best].i-x[left].i-x[right].i),
                sign*(x[best].q-x[left].q-x[right].q)};
}
#if defined(__SSE2__) && !defined(FM1_NO_SIMD)
/* Four independent time positions per SIMD vector. No horizontal sums or
 * reassociation: each lane follows exactly the scalar FFT arithmetic. */
typedef struct { __m128 i,q; } FmIQ4;
static inline FmIQ4 fm4_add(FmIQ4 a,FmIQ4 b) { return (FmIQ4){_mm_add_ps(a.i,b.i),_mm_add_ps(a.q,b.q)}; }
static inline FmIQ4 fm4_sub(FmIQ4 a,FmIQ4 b) { return (FmIQ4){_mm_sub_ps(a.i,b.i),_mm_sub_ps(a.q,b.q)}; }
static inline FmIQ4 fm4_scale(FmIQ4 a,float k) { __m128 w=_mm_set1_ps(k);return (FmIQ4){_mm_mul_ps(a.i,w),_mm_mul_ps(a.q,w)}; }
static inline __m128 fm4_neg(__m128 x) { return _mm_xor_ps(x,_mm_set1_ps(-0.0f)); }
static inline FmIQ4 fm4_j(FmIQ4 a) { return (FmIQ4){fm4_neg(a.q),a.i}; }
static inline FmIQ4 fm4_load(const IQ *p)
{
    __m128 a=_mm_loadu_ps((const float *)p),b=_mm_loadu_ps((const float *)(p+2));
    return (FmIQ4){_mm_shuffle_ps(a,b,_MM_SHUFFLE(2,0,2,0)),_mm_shuffle_ps(a,b,_MM_SHUFFLE(3,1,3,1))};
}
static void fm1_phase4(const IQ *history,IQ *phase)
{
    const float lo=.1464466094f,hi=.8535533906f,h=.7071067812f;
    FmIQ4 a=fm4_load(history+4),b=fm4_scale(fm4_load(history+2),.5f),c=fm4_scale(fm4_load(history+6),.5f);
    FmIQ4 e=fm4_scale(fm4_load(history+1),lo),f=fm4_scale(fm4_load(history+5),hi);
    FmIQ4 g=fm4_scale(fm4_load(history+3),hi),k=fm4_scale(fm4_load(history+7),lo);
    FmIQ4 bc=fm4_add(b,c),bj=fm4_j(fm4_sub(b,c)),neg_a={fm4_neg(a.i),fm4_neg(a.q)};
    FmIQ4 even[4]={fm4_add(a,bc),fm4_add(neg_a,bj),fm4_sub(a,bc),fm4_sub(neg_a,bj)};
    FmIQ4 ef=fm4_add(e,f),ed=fm4_sub(e,f),gk=fm4_add(g,k),gj=fm4_j(fm4_sub(g,k));
    FmIQ4 odd0=fm4_add(ef,gk),odd1=fm4_add(ed,gj),odd2=fm4_j(fm4_sub(ef,gk)),odd3=fm4_sub(ed,gj);
    __m128 h4=_mm_set1_ps(h),nh4=_mm_set1_ps(-h);
    FmIQ4 rot1={_mm_sub_ps(_mm_mul_ps(h4,odd1.i),_mm_mul_ps(h4,odd1.q)),
                _mm_add_ps(_mm_mul_ps(h4,odd1.i),_mm_mul_ps(h4,odd1.q))};
    FmIQ4 rot3={_mm_sub_ps(_mm_mul_ps(nh4,odd3.i),_mm_mul_ps(h4,odd3.q)),
                _mm_sub_ps(_mm_mul_ps(h4,odd3.i),_mm_mul_ps(h4,odd3.q))};
    FmIQ4 x[8]={fm4_add(even[0],odd0),fm4_add(even[1],rot1),fm4_add(even[2],odd2),fm4_add(even[3],rot3),
                fm4_sub(even[0],odd0),fm4_sub(even[1],rot1),fm4_sub(even[2],odd2),fm4_sub(even[3],rot3)};
    float re[8][4],im[8][4];
    __m128 best_power=_mm_set1_ps(-1);__m128i best=_mm_setzero_si128();
    for(unsigned j=0;j<8;++j) {
        _mm_storeu_ps(re[j],x[j].i);_mm_storeu_ps(im[j],x[j].q);
        __m128 power=_mm_add_ps(_mm_mul_ps(x[j].i,x[j].i),_mm_mul_ps(x[j].q,x[j].q));
        __m128 mask=_mm_cmpgt_ps(power,best_power);
        best_power=_mm_max_ps(best_power,power);
        best=_mm_or_si128(_mm_and_si128(_mm_castps_si128(mask),_mm_set1_epi32((int)j)),
                          _mm_andnot_si128(_mm_castps_si128(mask),best));
    }
    int bins[4];_mm_storeu_si128((__m128i *)bins,best);
    for(unsigned j=0;j<4;++j) {
        unsigned bin=(unsigned)bins[j],left=(bin+7)&7,right=(bin+1)&7;
        float sign=(left&1)?-1:1;
        phase[j]=(IQ){sign*(re[bin][j]-re[left][j]-re[right][j]),
                      sign*(im[bin][j]-im[left][j]-im[right][j])};
    }
}
#endif
/* FM1 receiver paths always use stride one. Handle startup once per block
 * and keep discriminator/filter state in locals throughout the hot loop. */
static void fm1_process_full_rate(FMDemod *d, const IQ *iq, float *video, size_t n)
{
    size_t j=0;
    while(j<n && d->history_count<8) {
        d->history[d->history_pos]=iq[j];
        d->history_pos=(d->history_pos+1)&7;
        ++d->history_count; ++d->tick; video[j++]=0;
    }
    if(j<n && !d->valid) {
        IQ phase=fm1_phase(d);
        d->history[d->history_pos]=iq[j];
        d->history_pos=(d->history_pos+1)&7;
        d->pi=phase.i; d->pq=phase.q; d->valid=1;
        d->previous_tick=d->tick++;
        video[j++]=d->filtered;
    }
    float pi=d->pi,pq=d->pq,filtered=d->filtered;
    const float scale=d->scale,alpha=d->alpha;
    if(j<n) { d->tick+=n-j; d->previous_tick=d->tick-1; }
#if defined(__SSE2__) && !defined(FM1_NO_SIMD)
    /* Only the first windows can cross the input block boundary. Assemble
     * that prefix once; all later windows read directly from the input. */
    if(j<8 && n-j>=4) {
        IQ prefix[16];
        for(unsigned k=0;k<8;++k) prefix[k]=d->history[(d->history_pos+k)&7];
        size_t take=n-j<8?n-j:8;
        memcpy(prefix+8,iq+j,take*sizeof(IQ));
        size_t first=j;
        for(;j<8 && n-j>=4;j+=4) {
            IQ phase[4];fm1_phase4(prefix+j-first,phase);
            for(unsigned k=0;k<4;++k) {
                d->history[d->history_pos]=iq[j+k];
                d->history_pos=(d->history_pos+1)&7;
                float v=fm_atan2f(phase[k].q*pi-phase[k].i*pq,phase[k].i*pi+phase[k].q*pq)*scale;
                pi=phase[k].i;pq=phase[k].q;
                filtered+=alpha*(v-filtered);video[j+k]=filtered;
            }
        }
    }
    size_t first=j;
    for(;n-j>=4;j+=4) {
        IQ phase[4];fm1_phase4(iq+j-8,phase);
        for(unsigned k=0;k<4;++k) {
            float v=fm_atan2f(phase[k].q*pi-phase[k].i*pq,phase[k].i*pi+phase[k].q*pq)*scale;
            pi=phase[k].i;pq=phase[k].q;
            filtered+=alpha*(v-filtered);video[j+k]=filtered;
        }
    }
    if(j>first) {
        /* Restore exactly the old ring layout for the scalar tail and the
         * next call, without eight-sample bookkeeping on every SIMD group. */
        d->history_pos=(d->history_pos+(unsigned)((j-first)&7))&7;
        for(unsigned k=0;k<8;++k) d->history[(d->history_pos+k)&7]=iq[j-8+k];
    }
#endif
    for(;j<n;++j) {
        IQ phase=fm1_phase(d);
        d->history[d->history_pos]=iq[j];
        d->history_pos=(d->history_pos+1)&7;
        float v=fm_atan2f(phase.q*pi-phase.i*pq,phase.i*pi+phase.q*pq)*scale;
        pi=phase.i; pq=phase.q;
        filtered+=alpha*(v-filtered);
        video[j]=filtered;
    }
    d->pi=pi; d->pq=pq; d->filtered=filtered;
}
void fm_demod_process(FMDemod *d, const IQ *iq, float *video, size_t n)
{
    if(d->mode && d->stride==1) { fm1_process_full_rate(d,iq,video,n); return; }
    for(size_t j=0;j<n;++j,++d->tick) {
        IQ sample=iq[j];
        if(d->mode) {
            /* Linrad uses the eight preceding samples, excluding this one. */
            IQ phase=fm1_phase(d);
            d->history[d->history_pos]=sample;
            d->history_pos=(d->history_pos+1)&7;
            if(d->history_count<8) { ++d->history_count; video[j]=0; continue; }
            sample=phase;
        }
        if(!d->valid) {
            d->pi=sample.i; d->pq=sample.q;
            d->previous_tick=d->tick; d->valid=1;
        } else if(d->tick-d->previous_tick>=d->stride) {
            float i=sample.i,q=sample.q;
            float v=fm_atan2f(q*d->pi-i*d->pq,i*d->pi+q*d->pq)*d->scale;
            d->pi=i; d->pq=q; d->previous_tick=d->tick;
            d->filtered+=d->alpha*(v-d->filtered);
        }
        video[j]=d->filtered;
    }
}
static void process(Decoder *d, const IQ *iq, const IQ *guide,
                    const float *video, const float *guide_video, size_t n)
{
    d->guided=(guide!=NULL || guide_video!=NULL) && !d->no_guide_raster;
    d->nominal_line_period=d->rate/d->line_hz;
    d->vsync_bin_samples=(uint64_t)llround(d->rate*8e-6);
    if(d->preview) d->preview->nominal_line_period=d->preview->rate/d->preview->line_hz;
    const float scale = d->rate / (2 * PI * d->deviation) * (d->invert ? -1 : 1);
    const float alpha = 1 - exp(-2 * PI * 2500000 / d->rate);
    const unsigned guide_stride=d->guide_stride?d->guide_stride:1;
    const float guide_alpha=1-exp(-2*PI*2500000*guide_stride/d->rate);
    const float sync_alpha = 1 - exp(-2 * PI * 500000 / d->rate);
    const unsigned sync_stride=(unsigned)fmax(1,floor(d->rate/4e6));
    /* Keep the same absolute sample grid across arbitrary block boundaries
     * without an integer division for every input sample. */
    uint64_t first_sync=d->tick+(!d->previous_valid?1:0);
    uint64_t next_sync=first_sync+(sync_stride-first_sync%sync_stride)%sync_stride;
    for (size_t j = 0; j < n; ++j, ++d->tick) {
        float v,gv;
        if(video) {
            if(!d->previous_valid) { d->previous_valid=1; continue; }
            v=video[j]; gv=guide_video?guide_video[j]:v;
            if(d->video_deemphasis && guide_video) {
                float delayed=d->sync_delay[d->sync_delay_pos];
                d->sync_delay[d->sync_delay_pos]=gv;
                d->sync_delay_pos=(d->sync_delay_pos+1)%VIDEO_EQ_DELAY;
                gv=delayed;
            }
        } else {
            float i = iq[j].i, q = iq[j].q;
            if (!d->previous_valid) { d->pi = i; d->pq = q; if(guide) { d->guide_pi=guide[j].i; d->guide_pq=guide[j].q; d->guide_tick=d->tick; } d->previous_valid = 1; continue; }
            v = fm_atan2f(q*d->pi - i*d->pq, i*d->pi + q*d->pq) * scale;
            d->pi = i; d->pq = q;
            d->filtered += alpha * (v - d->filtered);
            v = d->color?v:d->filtered;
            gv=v;
            if(guide) {
                /* The guide has already been anti-alias filtered. Its lower
                 * sample clock reduces expensive phase calculations, while
                 * timestamps and both rasters stay on the original IQ clock. */
                if(d->tick-d->guide_tick>=guide_stride) {
                    float gi=guide[j].i,gq=guide[j].q;
                    float g=fm_atan2f(gq*d->guide_pi-gi*d->guide_pq,gi*d->guide_pi+gq*d->guide_pq)*(scale/guide_stride);
                    d->guide_pi=gi; d->guide_pq=gq; d->guide_tick=d->tick;
                    d->guide_filtered+=guide_alpha*(g-d->guide_filtered);
                }
                gv=d->guide_filtered;
            }
        }
        if(d->preview) {
            d->preview->tick=d->tick;
            free_raster_clock(d->preview,0);

        }
        if(d->free_sync) free_raster_clock(d,1);
        if(d->color) pal_sample(&d->pal,v,d->tick,d->line,d->have_line);
        const PalDecoder *color=d->color?&d->pal:NULL;
        if(d->preview) raster_sample(d->preview,v,v,color);
        /* A separate narrow sync path and hysteresis keep video detail and
         * high-frequency noise from repeatedly triggering the sync detector. */
        d->sync_filtered += sync_alpha * (gv - d->sync_filtered);
        if(d->free_sync && d->tick==next_sync) {
            next_sync+=sync_stride;
            vertical_sample(d,d->sync_filtered);
            double period=d->free_period ? d->free_period : d->rate/d->line_hz;
            /* Advance the folding phase on its sample clock; fmod is only
             * needed when sync changes the recovered period. */
            if(period!=d->fold_period) {
                d->fold_period=period;
                d->fold_phase=fmod(d->tick,period);
            } else {
                d->fold_phase+=d->tick-d->fold_tick;
                if(d->fold_phase>=period) d->fold_phase-=period;
            }
            d->fold_tick=d->tick;
            int bin=(int)(d->fold_phase*128/period)%128;
            int part=(int)((d->tick-d->field)*150/d->rate);
            if(part<3) {
                d->hsync_sum[part][bin]+=sync_clip(d->sync_filtered);
                ++d->hsync_samples[part][bin];
            }
        }
        int low = d->sync_filtered < (d->low ? -0.13f : -0.17f);
        if (d->free_sync) {
            if(low && !d->low) d->pulse=d->tick;
            if(!low && d->low) {
                double duration=(d->tick-d->pulse)/d->rate;
                if(duration>3.5e-6 && duration<6e-6 && d->hsync_count<512)
                    d->hsync_pulses[d->hsync_count++]=d->pulse;
            }
            d->low=low;
        }
        if (!d->free_sync) {
            if (low && !d->low) d->pulse = d->tick;
            if (!low && d->low) {
                double duration = (d->tick - d->pulse) / d->rate;
                if (duration > 20e-6 && duration < 31e-6 &&
                    (!d->have_field || (d->pulse - d->field) / d->rate > .015)) {
                    if (d->have_field && d->lines > 200) {
                        double period = (d->pulse - d->field) / d->rate;
                        d->stack_valid = period > .0195 && period < .0205 &&
                            d->complete_lines >= 280 && d->stable_lines >= 270;
                        double phase = fmod((d->line-d->field)/(d->rate*64e-6), 1.0);
                        d->stack_phase = phase > .25 && phase < .75;
                        ++d->fields;
                        if (d->frame) d->frame(d->opaque, d->pixels);
                    }
                    d->field = d->pulse; d->have_field = 1; d->lines = 0;
                    d->have_line = 0; d->row = -1;
                    d->complete_lines = d->stable_lines = 0;
                    for (int k = 0; k < WIDTH * HEIGHT; ++k) d->pixels[k] = 0xff000000;
                } else if (duration > 3.5e-6 && duration < 6e-6 &&
                    (!d->have_line || (d->pulse-d->line)/d->rate > 100e-6 ||
                     ((d->pulse-d->line)/d->rate > 58e-6 && (d->pulse-d->line)/d->rate < 70e-6))) {
                    if (d->have_line && d->row >= 0 && d->row < HEIGHT &&
                        fabs((d->pulse-d->line)/d->rate - 64e-6) < 1.5e-6)
                        ++d->stable_lines;
                    d->line = d->pulse; d->have_line = 1;
                    d->porch_sum = 0; d->porch_count = 0; d->column = 0;
                    /* Broad-sync onset precedes the visible field by ~20 lines. */
                    d->row = d->have_field ? (int)floor((d->line-d->field) / (d->rate*64e-6) + .25) - 20 : -1;
                    if (d->row >= 0 && d->row < HEIGHT) ++d->lines;
                }
            }
            d->low = low;
            if (d->have_field && (d->tick - d->field) / d->rate > .030) {
                d->have_field = 0; d->have_line = 0;
            }
        }
        if(!d->no_video_raster) raster_sample(d,v,gv,color);
    }
    if(d->preview) d->preview->tick=d->tick;
}

void decoder_process_video(Decoder *d, const float *video, const float *guide, size_t n)
{
    process(d,NULL,NULL,video,guide,n);
}
void decoder_process_guided(Decoder *d, const IQ *iq, const IQ *guide, size_t n)
{
    process(d,iq,guide,NULL,NULL,n);
}

void decoder_process(Decoder *d, const IQ *iq, size_t n)
{
    decoder_process_guided(d,iq,NULL,n);
}

/* Synthetic 625-line/50-field monochrome FM source with half-line field offset,
 * equalising pulses and serrated broad sync; never sent to any RF hardware. */
void demo_generate(IQ *iq, size_t n, double rate, double deviation, uint64_t *tick, double *phase)
{
    for (size_t j = 0; j < n; ++j, ++*tick) {
        double t = *tick / rate;
        double field = fmod(t, .020), line = fmod(t, 64e-6), half = fmod(field, 32e-6);
        float v = 0;
        if (field < 160e-6) v = half < 27.3e-6 ? -.3f : 0;
        else if (field < 320e-6 || field >= .019840) v = half < 2.35e-6 ? -.3f : 0;
        else if (line < 4.7e-6) v = -.3f;
        else if (field >= 20*64e-6 && field < 308*64e-6 && line >= 10.5e-6 && line < 62.5e-6) {
            double x = (line - 10.5e-6) / 52e-6;
            int row = (int)(field / 64e-6) - 20;
            v = row < 180 ? .7f * (7 - (int)(x * 8)) / 7 :
                (((int)(x*16) + row/16) % 2 ? .7f : 0);
        }
        *phase = remainder(*phase + 2*PI*deviation*v/rate, 2*PI);
        iq[j].i = .7f * cos(*phase); iq[j].q = .7f * sin(*phase);
    }
}
