#include "afc.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void afc_reset(AFC *a, double rate, double line_hz)
{
    memset(a,0,sizeof(*a)); a->rate=rate; a->line_hz=line_hz;
    a->settle=(uint64_t)(rate*.15);
}
static double mean(const double *p, int start, int length)
{
    double sum=0;
    for(int j=0;j<length;++j) sum+=p[(start+j+256)%256];
    return sum/length;
}
static int reference(const AFC *a, int half, double *black, int *phase)
{
    double p[256];
    for(int j=0;j<256;++j) {
        if(a->count[half][j]<20) return 0;
        p[j]=a->sum[half][j]/a->count[half][j];
    }
    double best=0;
    for(int j=0;j<256;++j) {
        /* Nominal sync start j; avoid edges and PAL colour burst. */
        double tip=mean(p,j+5,10);
        double front=mean(p,j-4,3), back=mean(p,j+34,5);
        double depth=(front+back)/2-tip;
        double spread=0;
        for(int k=5;k<15;++k) spread=fmax(spread,fabs(p[(j+k)%256]-tip));
        if(depth<.16 || depth>.45 || fabs(front-back)>.045 || spread>.045) continue;
        if(depth>best) { best=depth; *black=back; *phase=j; }
    }
    return best>0;
}
int afc_measure(AFC *a, const float *video, size_t n, double deviation, int invert, double *error)
{
    int result=0;
    uint64_t window=(uint64_t)(a->rate*.04);
    for(size_t j=0;j<n;++j) {
        if(a->settle) { --a->settle; continue; }
        int half=a->tick>=window, bin=(int)a->phase;
        a->sum[half][bin]+=video[j]; ++a->count[half][bin];
        a->phase+=256*a->line_hz/a->rate;
        if(a->phase>=256) a->phase-=256;
        if(++a->tick==2*window) {
            double b[2]={0}; int phase[2]={0};
            int valid=reference(a,0,&b[0],&phase[0]) && reference(a,1,&b[1],&phase[1]);
            int distance=abs(phase[0]-phase[1]); if(distance>128) distance=256-distance;
            valid=valid && distance<=4 && fabs(b[0]-b[1])*deviation<30000;
            if(valid) *error=(b[0]+b[1])*.5*deviation*(invert?-1:1);
            result=valid?1:-1;
            memset(a->sum,0,sizeof(a->sum)); memset(a->count,0,sizeof(a->count)); a->tick=0;
        }
    }
    return result;
}
double afc_target(double anchor, double current, double error, int *limited)
{
    double target=current+error;
    *limited=!isfinite(target) || fabs(target-anchor)>AFC_RANGE_HZ || target<1 || target>6e9;
    if(*limited || fabs(error)<10000) return current;
    return current+fmax(-100000,fmin(100000,error*.7));
}
