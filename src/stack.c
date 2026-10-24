#include "atv.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define SW (WIDTH/4)
#define SH (HEIGHT/4)

void stack_destroy(FrameStack *s)
{
    free(s->history);
    memset(s,0,sizeof(*s));
}

/* Mode/window changes start a new acquisition. Failed allocation leaves the
 * existing stack intact. Reuse storage for a reset of the same window. */
int stack_configure(FrameStack *s, unsigned target, int rolling)
{
    if(target<1 || target>512) return 0;
    uint16_t *history=NULL;
    if(rolling) {
        history=s->rolling && s->target==target ? s->history :
            malloc((size_t)target*WIDTH*HEIGHT*sizeof(*history));
        if(!history) return 0;
    }
    if(history!=s->history) free(s->history);
    memset(s,0,sizeof(*s));
    s->history=history; s->rolling=!!rolling;
    s->target=target; s->phase=-1;
    s->period_x=WIDTH*64.0/52; s->period_y=312.5;
    return 1;
}
void stack_reset(FrameStack *s, unsigned target)
{
    (void)stack_configure(s,target,0);
}

/* Only the motion clock is discarded across input jumps or manual pauses.
 * The accumulated image remains in its original reference coordinates. */
void stack_discontinuity(FrameStack *s)
{
    s->motion_count=0; s->relock_seen=s->relock_previous_seen=0;
}
static double difference(double value, double period)
{
    return period>0?remainder(value,period):value;
}
/* Fit a short history instead of differentiating two noisy field positions.
 * Free raster phase wraps over the full line/field, including blanking. */
static int predict(const FrameStack *s,double *px,double *py,double *vx,double *vy,
                   double *ex,double *ey)
{
    unsigned n=s->motion_count;
    if(n<6 || s->motion_time[n-1]-s->motion_time[0]<5) return 0;
    double t=0,x=0,y=0,tt=0,tx=0,ty=0;
    for(unsigned i=0;i<n;++i) {
        double dt=(double)(s->motion_time[i]-s->motion_time[0]);
        t+=dt; x+=s->motion_x[i]; y+=s->motion_y[i];
        tt+=dt*dt; tx+=dt*s->motion_x[i]; ty+=dt*s->motion_y[i];
    }
    double spread=tt-t*t/n;
    if(spread<=0) return 0;
    *vx=(tx-t*x/n)/spread; *vy=(ty-t*y/n)/spread;
    double dx=(double)(s->seen-s->motion_time[0])-t/n;
    *px=x/n+*vx*dx; *py=y/n+*vy*dx;
    double xx=0,yy=0;
    for(unsigned i=0;i<n;++i) {
        double dt=(double)(s->motion_time[i]-s->motion_time[0])-t/n;
        xx+=pow(s->motion_x[i]-x/n-*vx*dt,2);
        yy+=pow(s->motion_y[i]-y/n-*vy*dt,2);
    }
    double leverage=(1+1.0/n+dx*dx/spread)/(n-2);
    *ex=fmin(32,fmax(12,3*sqrt(xx*leverage)));
    *ey=fmin(10,fmax(5,3*sqrt(yy*leverage)));
    return 1;
}
static void observe(FrameStack *s)
{
    double px=s->dx,py=s->dy,vx,vy,ex,ey;
    if(s->motion_count) {
        if(!predict(s,&px,&py,&vx,&vy,&ex,&ey)) {
            px=s->motion_x[s->motion_count-1]; py=s->motion_y[s->motion_count-1];
        }
        px+=difference(s->dx-px,s->phase<0?s->period_x:0);
        py+=difference(s->dy-py,s->phase<0?s->period_y:0);
    }
    if(s->motion_count==24) {
        memmove(s->motion_time,s->motion_time+1,23*sizeof(*s->motion_time));
        memmove(s->motion_x,s->motion_x+1,23*sizeof(*s->motion_x));
        memmove(s->motion_y,s->motion_y+1,23*sizeof(*s->motion_y));
        --s->motion_count;
    }
    unsigned i=s->motion_count++;
    s->motion_time[i]=s->seen; s->motion_x[i]=px; s->motion_y[i]=py;
    s->relock_seen=s->relock_previous_seen=0;
}
/* Box reduction suppresses pixel noise and keeps registration inexpensive.
 * The first structured field remains the reference, avoiding cumulative drift. */
static void reduce(const uint32_t *pixels, float *small)
{
    for (int y=0;y<SH;++y) for (int x=0;x<SW;++x) {
        unsigned sum=0;
        for (int j=0;j<4;++j) for (int i=0;i<4;++i)
            sum+=pixels[(4*y+j)*WIDTH+4*x+i]&255;
        small[y*SW+x]=sum/16.f;
    }
}
static double correlation(double n, double a, double b, double aa, double bb, double ab)
{
    double va=aa-a*a/n, vb=bb-b*b/n;
    if (va<n*16 || vb<n*16) return -1;
    return (ab-a*b/n)/sqrt(va*vb);
}
static int structured(const float *p)
{
    double n=0,a=0,b=0,aa=0,bb=0,ab=0;
    for (int y=2;y<SH-2;++y) for (int x=2;x<SW-2;++x) {
        double u=p[y*SW+x], v=(p[y*SW+x+1]+p[(y+1)*SW+x])*.5;
        ++n; a+=u; b+=v; aa+=u*u; bb+=v*v; ab+=u*v;
    }
    return correlation(n,a,b,aa,bb,ab)>.65;
}
/* Registration uses broad lettering rather than individual noisy pixels.
 * Test coherence beyond the smoothing support so filtered noise does not
 * qualify merely because neighbouring filtered samples share inputs. */
static int smooth_structure(float *p)
{
    float blurred[SW*SH];
    for(int y=0;y<SH;++y) for(int x=0;x<SW;++x) {
        double sum=0; int n=0;
        for(int j=-2;j<=2;++j) for(int i=-2;i<=2;++i) {
            int yy=y+j,xx=x+i;
            if(xx>=0 && xx<SW && yy>=0 && yy<SH) { sum+=p[yy*SW+xx]; ++n; }
        }
        blurred[y*SW+x]=sum/n;
    }
    memcpy(p,blurred,sizeof(blurred));
    double n=0,a=0,b=0,aa=0,bb=0,ab=0;
    for(int y=6;y<SH-12;++y) for(int x=6;x<SW-12;++x) {
        double u=p[y*SW+x],v=(p[y*SW+x+6]+p[(y+6)*SW+x])*.5;
        ++n; a+=u; b+=v; aa+=u*u; bb+=v*v; ab+=u*v;
    }
    return correlation(n,a,b,aa,bb,ab)>.20;
}
static double sample(const float *p, double x, double y)
{
    int ix=(int)x, iy=(int)y;
    double fx=x-ix, fy=y-iy;
    return (1-fy)*((1-fx)*p[iy*SW+ix]+fx*p[iy*SW+ix+1])+
        fy*((1-fx)*p[(iy+1)*SW+ix]+fx*p[(iy+1)*SW+ix+1]);
}
/* Positive offsets sample to the right/below the reference. Shear is the
 * difference in horizontal displacement between the top and bottom rows.
 * Missing borders are excluded, never wrapped or treated as black data. */
static double match(const float *ref, const float *p, double dx, double dy, double shear, int step)
{
    double n=0,a=0,b=0,aa=0,bb=0,ab=0;
    int total=0;
    for (int y=2;y<SH-2;y+=step) {
        double yy=y+dy, shift=dx+shear*(y-(SH-1)*.5)/SH;
        for (int x=2;x<SW-2;x+=step) {
            ++total;
            double xx=x+shift;
            if (xx<0 || xx>=SW-1 || yy<0 || yy>=SH-1) continue;
            double u=ref[y*SW+x], v=sample(p,xx,yy);
            ++n; a+=u; b+=v; aa+=u*u; bb+=v*v; ab+=u*v;
        }
    }
    if (n<total*.45) return -1;
    return correlation(n,a,b,aa,bb,ab);
}
/* Final fit at native resolution removes the box-reduction phase error. */
static double native_match(const FrameStack *s, const uint32_t *p, double dx, double dy, double shear)
{
    double n=0,a=0,b=0,aa=0,bb=0,ab=0;
    for (int y=8;y<HEIGHT-8;y+=3) {
        double yy=y+dy, shift=dx+shear*(y-(HEIGHT-1)*.5)/HEIGHT;
        if (yy<0 || yy>=HEIGHT-1) continue;
        int iy=(int)yy; double fy=yy-iy;
        for (int x=8;x<WIDTH-8;x+=3) {
            double xx=x+shift;
            if (xx<0 || xx>=WIDTH-1) continue;
            int ix=(int)xx; double fx=xx-ix;
            double u=s->reference_pixels[y*WIDTH+x];
            double v=(1-fy)*((1-fx)*(p[iy*WIDTH+ix]&255)+fx*(p[iy*WIDTH+ix+1]&255))+
                fy*((1-fx)*(p[(iy+1)*WIDTH+ix]&255)+fx*(p[(iy+1)*WIDTH+ix+1]&255));
            ++n; a+=u; b+=v; aa+=u*u; bb+=v*v; ab+=u*v;
        }
    }
    if (n<8000) return -1;
    return correlation(n,a,b,aa,bb,ab);
}
static int align(FrameStack *s, const float *p, const uint32_t *pixels, int weak)
{
    double dx=s->dx/4,dy=s->dy/4,shear=s->shear/4;
    double best=match(s->reference,p,dx,dy,shear,2);
    double score_sum=0,score_squared=0,score_count=0;
    /* Slow drift normally stays near the previous fit. Only trust this fast
     * path at high correlation; reacquire globally after fades or phase jumps. */
    if (weak || best<.94) {
        best=-1; shear=0;
        for (int y=-SH/2;y<=SH/2;y+=2) for (int x=-SW/2;x<=SW/2;x+=2) {
            double score=match(s->reference,p,x,y,0,3);
            if(score> -1) { score_sum+=score; score_squared+=score*score; ++score_count; }
            if (score>best) { best=score; dx=x; dy=y; }
        }
    }
    /* Refine translation and differential line skew together. */
    for (double step=2;step>=.125;step*=.5) {
        for (int pass=0;pass<4;++pass) {
            double bx=dx,by=dy,bs=shear;
            best=match(s->reference,p,dx,dy,shear,2)-(weak?.0005*shear*shear:0);
            for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x) for (int h=-1;h<=1;++h) {
                double nx=dx+x*step,ny=dy+y*step,ns=shear+h*step*2;
                if (fabs(ns)>16 || fabs(nx)>SW/2 || fabs(ny)>SH/2) continue;
                double score=match(s->reference,p,nx,ny,ns,2)-(weak?.0005*ns*ns:0);
                if (score>best) { best=score; bx=nx; by=ny; bs=ns; }
            }
            if (bx==dx && by==dy && bs==shear) break;
            dx=bx; dy=by; shear=bs;
        }
    }
    s->confidence=match(s->reference,p,dx,dy,shear,1);
    /* A fixed high NCC threshold rejects real low-C/N images. For weak
     * fields require both minimum correlation and separation from the whole
     * search's background of competing alignments (not a calibrated p-value). */
    if (s->confidence<(weak?.30:.80)) return 0;
    if (weak) {
        if(score_count<2) return 0;
        double mean=score_sum/score_count;
        double sd=sqrt(fmax(0,score_squared/score_count-mean*mean));
        if(s->confidence<mean+4*sd) return 0;
    }
    dx*=4; dy*=4; shear*=4;
    for (double step=weak?0:1;step>=.25;step*=.5) {
        for (int pass=0;pass<4;++pass) {
            double bx=dx,by=dy,bs=shear;
            best=native_match(s,pixels,dx,dy,shear);
            for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x) for (int h=-1;h<=1;++h) {
                double nx=dx+x*step, ny=dy+y*step, ns=shear+h*step*2;
                if (fabs(ns)>64 || fabs(nx)>WIDTH/2 || fabs(ny)>HEIGHT/2) continue;
                double score=native_match(s,pixels,nx,ny,ns);
                if (score>best) { best=score; bx=nx; by=ny; bs=ns; }
            }
            if (bx==dx && by==dy && bs==shear) break;
            dx=bx; dy=by; shear=bs;
        }
    }
    double px,py,vx,vy,ex,ey;
    int tracking=predict(s,&px,&py,&vx,&vy,&ex,&ey);
    if(tracking &&
       (fabs(difference(dx-px,s->phase<0?s->period_x:0))>ex ||
        fabs(difference(dy-py,s->phase<0?s->period_y:0))>ey)) {
        /* A weak isolated peak must not drag a rolling reference onto noise.
         * Allow a genuine phase jump after two nearby independent matches. */
        double dt=(double)(s->seen-s->relock_seen);
        int confirmed=s->relock_seen && dt<=4 &&
            fabs(difference(dx-s->relock_x-vx*dt,s->phase<0?s->period_x:0))<12 &&
            fabs(difference(dy-s->relock_y-vy*dt,s->phase<0?s->period_y:0))<5;
        /* If the old velocity itself is wrong, three fresh matches establish
         * a new trajectory. Requiring the old speed forever would deadlock. */
        if(!confirmed && s->relock_previous_seen && dt<=4 &&
           s->relock_seen-s->relock_previous_seen<=4) {
            double interval=(double)(s->relock_seen-s->relock_previous_seen);
            double nvx=difference(s->relock_x-s->relock_previous_x,s->phase<0?s->period_x:0)/interval;
            double nvy=difference(s->relock_y-s->relock_previous_y,s->phase<0?s->period_y:0)/interval;
            confirmed=fabs(difference(dx-s->relock_x-nvx*dt,s->phase<0?s->period_x:0))<12 &&
                fabs(difference(dy-s->relock_y-nvy*dt,s->phase<0?s->period_y:0))<5;
        }
        if(s->confidence<.95 && !confirmed) {
            s->relock_previous_seen=s->relock_seen;
            s->relock_previous_x=s->relock_x; s->relock_previous_y=s->relock_y;
            s->relock_seen=s->seen; s->relock_x=dx; s->relock_y=dy;
            return 0;
        }
        stack_discontinuity(s);
    } else if(tracking && weak) {
        /* On weak fields the photometric maximum jitters on residual noise.
         * Prefer the observed trajectory only when its image match is nearly
         * as good; do not force a prediction through a fade or real change. */
        double w=fmin(1,fmax(.2,(s->confidence-.30)/.50));
        double nx=dx-(1-w)*difference(dx-px,s->phase<0?s->period_x:0);
        double ny=dy-(1-w)*difference(dy-py,s->phase<0?s->period_y:0);
        double ns=s->shear+w*(shear-s->shear);
        double score=match(s->reference,p,nx/4,ny/4,ns/4,1);
        double mean=score_sum/score_count;
        double sd=sqrt(fmax(0,score_squared/score_count-mean*mean));
        if(fabs(nx)<=WIDTH/2 && fabs(ny)<=HEIGHT/2 &&
           score>=s->confidence-.01 && score>=.30 && score>=mean+4*sd) {
            dx=nx; dy=ny; shear=ns; s->confidence=score;
        }
    }
    s->dx=dx; s->dy=dy; s->shear=shear;
    return 1;
}

static int stack_add_internal(FrameStack *s, const uint32_t *pixels, const uint32_t *guide, int valid, int phase, uint32_t *out, int synced)
{
    ++s->seen;
    s->status="INCOMPLETE FRAME";
    if (!valid) return 0;
    s->status="DONE";
    if (!s->target || (!s->rolling && s->count>=s->target)) return 0;
    s->status="OTHER FIELD PARITY";
    if ((s->count || s->pending) && s->phase>=0 && phase>=0 && s->phase!=phase) return 0;
    float small[SW*SH];
    reduce(guide,small);
    s->confidence=0;
    int strong=structured(small);
    int coherent=smooth_structure(small);
    /* After acquisition the reference match supplies temporal evidence;
     * individual noisy fields need not pass the acquisition structure gate. */
    s->status="SEEK IMAGE DETAIL";
    if (!synced && !s->count && !s->pending && !strong && !coherent) return 0;
    if (!s->count && !s->pending) {
        memcpy(s->reference,small,sizeof(small));
        for(int k=0;k<WIDTH*HEIGHT;++k) s->reference_pixels[k]=guide[k]&255;
        s->phase=phase; s->dx=s->dy=s->shear=0; s->confidence=1;
        s->weak=!synced && !strong;
        s->pending_misses=0;
        /* A weak reference requires confirmation by an independent field. */
        if(s->weak) { s->pending=1; s->status="CONFIRM WEAK IMAGE"; return 0; }
    } else if(!synced) {
        if (!align(s,small,guide,s->weak || !strong)) {
            s->status="NO IMAGE MATCH";
            /* An isolated fade must not discard a usable weak reference.
             * Try several independent fields, but retire an unconfirmable
             * candidate so acquisition cannot stay on it indefinitely. */
            if(!s->count && ++s->pending_misses>=8) s->pending=0;
            return 0;
        }
        s->pending=0;
    }
    if(synced) {
        s->dx=s->dy=s->shear=0;
        s->confidence=0; s->pending=0;
    }
    observe(s);
    s->status=synced?"SYNC ADDING":"ADDING";
    uint16_t *slot=s->rolling?s->history+(size_t)s->next*WIDTH*HEIGHT:NULL;
    int evict=s->rolling && s->count==s->target;
    if(s->count<s->target) ++s->count;
    ++s->accepted;
    for (int y=0;y<HEIGHT;++y) for (int x=0;x<WIDTH;++x) {
        int k=y*WIDTH+x;
        if(slot) {
            if(evict && slot[k]<256) { s->sum[k]-=slot[k]; --s->weight[k]; }
            slot[k]=256;
        }
        double xx=x+s->dx+s->shear*(y-(HEIGHT-1)*.5)/HEIGHT, yy=y+s->dy;
        if (xx>=0 && xx<=WIDTH-1 && yy>=0 && yy<=HEIGHT-1) {
            int ix=(int)xx,iy=(int)yy, ix1=ix+1<WIDTH?ix+1:ix, iy1=iy+1<HEIGHT?iy+1:iy;
            double fx=xx-ix,fy=yy-iy;
            double g=(1-fy)*((1-fx)*(pixels[iy*WIDTH+ix]&255)+fx*(pixels[iy*WIDTH+ix1]&255))+
                fy*((1-fx)*(pixels[iy1*WIDTH+ix]&255)+fx*(pixels[iy1*WIDTH+ix1]&255));
            unsigned value=(unsigned)lround(g);
            s->sum[k]+=value; ++s->weight[k];
            if(slot) slot[k]=(uint16_t)value;
        }
        unsigned g=s->weight[k]?(s->sum[k]+s->weight[k]/2)/s->weight[k]:0;
        out[k]=0xff000000u | g*0x010101u;
    }
    if(slot) s->next=(s->next+1)%s->target;
    /* Improve a noisy reference in the original coordinate system. Only use
     * patches whose entire smoothing support has at least two observations;
     * unknown/cropped borders must never become reference features. */
    if(guide==pixels && ((s->weak && s->count>=3) || s->rolling)) {
        unsigned minimum=s->target==1?1:2;
        unsigned char covered[SW*SH];
        reduce(out,small); smooth_structure(small);
        if(s->rolling) for(int k=0;k<WIDTH*HEIGHT;++k)
            if(s->weight[k]>=minimum) s->reference_pixels[k]=out[k]&255;
        for(int y=0;y<SH;++y) for(int x=0;x<SW;++x) {
            int valid_patch=1;
            for(int j=0;j<4;++j) for(int i=0;i<4;++i)
                if(s->weight[(y*4+j)*WIDTH+x*4+i]<minimum) valid_patch=0;
            covered[y*SW+x]=valid_patch;
        }
        for(int y=2;y<SH-2;++y) for(int x=2;x<SW-2;++x) {
            int valid_patch=1;
            for(int j=-2;j<=2;++j) for(int i=-2;i<=2;++i)
                if(!covered[(y+j)*SW+x+i]) valid_patch=0;
            if(valid_patch) s->reference[y*SW+x]=small[y*SW+x];
        }
    }
    return 1;
}

/* The guide supplies coordinates only; always accumulate the video pixels.
 * A separate guide keeps its original reference: the wideband average must
 * not replace a reference formed in a different IF passband. */
int stack_add(FrameStack *s, const uint32_t *pixels, int valid, int phase, uint32_t *out)
{
    return stack_add_internal(s,pixels,pixels,valid,phase,out,0);
}
int stack_add_guided(FrameStack *s, const uint32_t *pixels, const uint32_t *guide, int valid, int phase, uint32_t *out)
{
    return stack_add_internal(s,pixels,guide,valid,phase,out,0);
}
int stack_add_synced(FrameStack *s, const uint32_t *pixels, const uint32_t *guide, int valid, int phase, uint32_t *out)
{
    return stack_add_internal(s,pixels,guide,valid,phase,out,1);
}
