#include "pal.h"
#include <math.h>
#include <string.h>
#define TAU 6.28318530717958647692
#define FSC 4433618.75

void pal_init(PalDecoder *p, double rate)
{
    memset(p,0,sizeof(*p)); p->rate=rate;
    double w=TAU*FSC/rate;
    p->step_c=cos(w); p->step_s=sin(w);
    p->alpha=1-exp(-TAU*900000/rate);
    p->y_alpha=1-exp(-TAU*2500000/rate);
    /* Keep luminance detail separate from the narrower colour differences.
     * Match their low-frequency delays to limit coloured edge fringes. */
    double r=.75;
    p->notch_b=-2*cos(w); p->notch_a=-2*r*cos(w);
    p->notch_gain=(1-2*r*cos(w)+r*r)/(2-2*cos(w));
    double notch_delay=1-(p->notch_a+2*r*r)/(1+p->notch_a+r*r);
    double delay=2*(1-p->alpha)/p->alpha-2*(1-p->y_alpha)/p->y_alpha-notch_delay;
    p->delay_samples=(unsigned)fmax(0,fmin(31,round(delay)));
}
static void burst(PalDecoder *p)
{
    p->gate_done=1;
    if(p->count<8) goto missing;
    /* Fit DC + C*cos + S*sin. Removing DC and solving the finite window
     * avoids hue bias from a non-integral number of carrier cycles. */
    double n=p->count, cc=p->sum_cc-p->sum_c*p->sum_c/n;
    double ss=n-p->sum_cc-p->sum_s*p->sum_s/n;
    double cs=p->sum_cs-p->sum_c*p->sum_s/n;
    double vc=p->sum_vc-p->sum_v*p->sum_c/n;
    double vs=p->sum_vs-p->sum_v*p->sum_s/n;
    double det=cc*ss-cs*cs;
    if(det<=1e-6) goto missing;
    double c=(vc*ss-vs*cs)/det, s=(vs*cc-vc*cs)/det;
    double amplitude=hypot(c,s);
    double energy=p->sum_vv-p->sum_v*p->sum_v/n;
    if(!isfinite(amplitude) || (amplitude<.015 && energy<.0001*n)) goto missing;
    if(amplitude>.8) goto seeking;
    /* In wide FM reception the burst can be coherent while most energy
     * within this short gate is broadband noise. Average alternating lines
     * separately, then test coherence and the PAL +/-135 degree relation.
     * A fixed line-edge artefact rotates with the local oscillator instead
     * of maintaining the same phase two lines later, and averages away. */
    unsigned k=p->parity;
    if(p->burst_count[k]<8) ++p->burst_count[k];
    double alpha=1.0/p->burst_count[k];
    p->burst_c[k]+=alpha*(c-p->burst_c[k]);
    p->burst_s[k]+=alpha*(s-p->burst_s[k]);
    p->burst_power[k]+=alpha*(c*c+s*s-p->burst_power[k]);
    if(p->burst_count[0]<8 || p->burst_count[1]<8) goto seeking;
    double length[2];
    for(unsigned j=0;j<2;++j) {
        length[j]=hypot(p->burst_c[j],p->burst_s[j]);
        if(length[j]<.035 || length[j]>.3 ||
           length[j]*length[j]<.3*p->burst_power[j]) goto seeking;
    }
    c=p->burst_c[k]/length[k]; s=p->burst_s[k]/length[k];
    double other_c=p->burst_c[k^1]/length[k^1], other_s=p->burst_s[k^1]/length[k^1];
    if(fabs(c*other_c+s*other_s)>.45) goto seeking;
    double ac=-(c+other_c), as=-(s+other_s), norm=hypot(ac,as);
    double phase=atan2(as,ac);
    if(p->tracked_tick && p->tick>p->tracked_tick) {
        double dt=p->tick-p->tracked_tick;
        double error=remainder(phase-p->tracked_phase-p->burst_frequency*dt,TAU);
        /* Second-order burst loop: phase is smoothed by the coherent line
         * averages; its innovation steers the predicted carrier frequency. */
        if(dt<p->rate*.002 && fabs(error)<.4) {
            p->burst_frequency+=.05*error/dt;
            double limit=TAU*500/p->rate;
            p->burst_frequency=fmax(-limit,fmin(limit,p->burst_frequency));
        }
    }
    p->tracked_phase=phase; p->tracked_tick=p->tick;
    p->axis_c=ac/norm; p->axis_s=as/norm;
    p->polarity=p->axis_c*s-p->axis_s*c>0?1:-1;
    if(p->good<8) ++p->good;
    p->locked=p->locked || p->good>=3;
    if(p->locked) p->last_lock=p->tick;
    ++p->burst_updates;
    p->have_burst=1;
    return;
missing:
    p->locked=0;
    memset(p->burst_count,0,sizeof(p->burst_count));
    memset(p->burst_c,0,sizeof(p->burst_c));
    memset(p->burst_s,0,sizeof(p->burst_s));
    memset(p->burst_power,0,sizeof(p->burst_power));
seeking:
    p->have_burst=p->good=0;
    /* Colour killer with hold time: a few noisy bursts should not create
     * monochrome stripes. Keep predicting PAL phase for at most 0.8 ms. */
    if(!p->last_lock || p->tick-p->last_lock>p->rate*.0008) p->locked=0;
}

static unsigned channel(float v)
{
    if(!(v>0)) return 0;
    if(v>=1) return 255;
    return (unsigned)lrintf(v*255);
}
void pal_sample(PalDecoder *p, float composite, uint64_t tick, uint64_t line, int have_line)
{
    if(!p->initialized || tick!=p->tick+1) {
        double phase=remainder(TAU*FSC*(double)tick/p->rate,TAU);
        p->osc_c=cos(phase); p->osc_s=sin(phase); p->initialized=1;
    } else {
        double c=p->osc_c;
        p->osc_c=c*p->step_c-p->osc_s*p->step_s;
        p->osc_s=p->osc_s*p->step_c+c*p->step_s;
    }
    p->tick=tick;
    if(line!=p->line) {
        double distance=(double)line-(double)p->line;
        if(distance<p->rate*48e-6 || distance>p->rate*80e-6)
            memset(p->burst_count,0,sizeof(p->burst_count));
        /* Advance both line memories with the burst PLL. Without this,
         * averaging would introduce a hue error for transmitter clock drift. */
        double angle=p->burst_frequency*distance, rc=cos(angle), rs=sin(angle);
        for(unsigned j=0;j<2;++j) {
            double c=p->burst_c[j], s=p->burst_s[j];
            p->burst_c[j]=c*rc-s*rs; p->burst_s[j]=s*rc+c*rs;
        }
        double ac=p->axis_c, as=p->axis_s;
        p->axis_c=ac*rc-as*rs; p->axis_s=as*rc+ac*rs;
        p->polarity=-p->polarity;
        p->parity^=1;
        if(!p->gate_done) { p->have_burst=p->good=p->locked=0; }
        p->line=line; p->gate_done=0; p->count=0;
        p->sum_c=p->sum_s=p->sum_cc=p->sum_cs=p->sum_v=p->sum_vc=p->sum_vs=p->sum_vv=0;
    }
    double elapsed=(double)(tick-line);
    if(have_line && elapsed>=p->rate*5.8e-6 && elapsed<p->rate*7.8e-6) {
        double c=p->osc_c,s=p->osc_s;
        ++p->count; p->sum_c+=c; p->sum_s+=s; p->sum_cc+=c*c; p->sum_cs+=c*s;
        p->sum_v+=composite; p->sum_vc+=composite*c; p->sum_vs+=composite*s; p->sum_vv+=composite*composite;
    }
    if(have_line && elapsed>=p->rate*7.8e-6 && !p->gate_done) burst(p);
    if(!have_line) p->locked=0;
    float y=p->notch_gain*(composite+p->notch_b*p->x1+p->x2)-p->notch_a*p->z1-.5625f*p->z2;
    p->x2=p->x1; p->x1=composite; p->z2=p->z1; p->z1=y;
    /* The complementary band-pass rejects luminance/DC before quadrature
     * mixing, avoiding false colour ripple on neutral white and grey. */
    float chroma=composite-y;
    float u=2*chroma*p->osc_c, v=2*chroma*p->osc_s;
    p->u1+=p->alpha*(u-p->u1); p->u2+=p->alpha*(p->u1-p->u2);
    p->v1+=p->alpha*(v-p->v1); p->v2+=p->alpha*(p->v1-p->v2);
    p->y1+=p->y_alpha*(y-p->y1); p->y2+=p->y_alpha*(p->y1-p->y2);
    p->y_delay[p->delay_pos]=p->y2;
    y=p->y_delay[(p->delay_pos+32-p->delay_samples)&31];
    p->delay_pos=(p->delay_pos+1)&31;
    /* Rotate/scale only when the raster emits a pixel, not at every IQ sample. */
    p->out_y=y; p->out_u=p->u2; p->out_v=p->v2;
}
uint32_t pal_rgb(const PalDecoder *p)
{
    /* Constant reciprocals avoid serial divides for every output pixel.
     * Keep normal IEEE semantics elsewhere (no global fast-math). */
    float y=p->out_y*(1.f/.7f);
    float u=p->locked?(p->out_u*p->axis_c+p->out_v*p->axis_s)*(1.f/.7f):0;
    float v=p->locked?(-p->out_u*p->axis_s+p->out_v*p->axis_c)*p->polarity*(1.f/.7f):0;
    return 0xff000000u | channel(y+v*(1.f/.877f))<<16 |
        channel(y-u*(.114f/(.587f*.493f))-v*(.299f/(.587f*.877f)))<<8 |
        channel(y+u*(1.f/.493f));
}
