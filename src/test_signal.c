/* Independent offline transmitter model; does not call the receiver/demo DSP. */
#include "test_signal.h"
#include <math.h>
#include <string.h>

static void word(uint8_t *p, int x, int y, const char *str, int sx, int sy)
{
    static const char *chars="ATV123";
    static const char *bits[]={
        "01110100011000111111100011000110001",
        "11111001000010000100001000010000100",
        "10001100011000110001100010101000100",
        "00100011000010000100001000010001110",
        "01110100010000100010001000100011111",
        "11110000010000101110000010000111110"};
    for(;*str;++str,x+=6*sx) {
        const char *c=strchr(chars,*str);
        if(!c) continue;
        for(int row=0;row<7;++row) for(int col=0;col<5;++col)
            if(bits[c-chars][row*5+col]=='1')
                for(int yy=0;yy<sy;++yy) for(int xx=0;xx<sx;++xx) {
                    int px=x+col*sx+xx, py=y+row*sy+yy;
                    if(px>=0 && px<WIDTH && py>=0 && py<HEIGHT) p[py*WIDTH+px]=255;
                }
    }
}
void test_signal_init(TestSignal *s, TestSignalConfig config)
{
    memset(s,0,sizeof(*s)); s->config=config; s->random=config.seed;
    s->noise_sigma=config.noise ? config.amplitude/sqrt(2*pow(10,config.cnr_db/10)) : 0;
    for(int y=0;y<HEIGHT;++y) for(int x=0;x<WIDTH;++x) {
        int value=16;
        if(y<3 || y>=HEIGHT-3 || x<3 || x>=WIDTH-3) value=255;
        else if(y>=240 && y<276) value=255*(7-x/(WIDTH/8))/7;
        s->picture[y*WIDTH+x]=(uint8_t)value;
    }
    word(s->picture,116,22,"ATV",24,13);
    word(s->picture,116,132,"123",24,13);
}
/* SplitMix64: fixed-width unsigned operations, same sequence for seed and
 * sample index independently of block size. Open interval avoids log(0). */
static double uniform(TestSignal *s)
{
    uint64_t z=(s->random+=UINT64_C(0x9e3779b97f4a7c15));
    z=(z^(z>>30))*UINT64_C(0xbf58476d1ce4e5b9);
    z=(z^(z>>27))*UINT64_C(0x94d049bb133111eb);
    z^=z>>31;
    return ((double)(z>>12)+.5)/4503599627370496.0;
}
void test_signal_generate(TestSignal *s, IQ *out, size_t count)
{
    const TestSignalConfig *c=&s->config;
    double line_us=1e6/(15625+c->line_offset_hz);
    double field_us=1e6/(50+c->field_offset_hz);
    for(size_t j=0;j<count;++j,++s->sample) {
        /* At zero offsets this is a continuous 625-line raster with a
         * half-line second-field offset. Independent clock errors deliberately
         * break that ratio. Field origin is the first broad-sync pulse. */
        double us=(double)s->sample*(1e6/c->rate);
        double field=fmod(us,field_us)*64/line_us;
        double line=fmod(us,line_us)*64/line_us, half=fmod(field,32);
        double video=0;
        double carrier=2*PI*(4433618.75+c->color_offset_hz)*s->sample/c->rate+c->color_phase;
        int sign=((uint64_t)floor(us/line_us)&1)?-1:1;
        if(field<160) video=half<27.3 ? -.3 : 0;
        else if(field<320 || field>=field_us*64/line_us-160) video=half<2.35 ? -.3 : 0;
        else if(line<4.7) video=-.3;
        else if(line>=10.5 && line<62.5) {
            int x=(int)((line-10.5)*WIDTH/52);
            /* Keep the picture row constant through the half-line offset of
             * the second field, including its final active line. */
            int y=(int)floor((field-line)/64+.001)-20;
            if(x>=0 && x<WIDTH && y>=0 && y<HEIGHT) {
                if(c->color) {
                    static const unsigned bars[]={7,6,3,2,5,4,1,0};
                    unsigned bar=bars[x/(WIDTH/8)];
                    double r=(bar&4)?.75:0, g=(bar&2)?.75:0, b=(bar&1)?.75:0;
                    double luma=.299*r+.587*g+.114*b;
                    video=.7*(luma+.493*(b-luma)*cos(carrier)+sign*.877*(r-luma)*sin(carrier));
                } else video=.7*s->picture[y*WIDTH+x]/255;
            }
        }
        /* Burst blanking through the vertical interval. PAL swinging burst
         * is -U +/-V at +/-135 degrees, 0.3 V peak-to-peak. */
        if(c->color && !c->no_burst && field>=320 && field<field_us*64/line_us-160 && line>=5.6 && line<7.85)
            video=.15/sqrt(2)*(-cos(carrier)+sign*sin(carrier));
        double hz=c->offset+c->deviation*video*(c->invert?-1:1);
        s->phase=remainder(s->phase+2*PI*hz/c->rate,2*PI);
        double ni=0,nq=0;
        if(c->noise) {
            double radius=s->noise_sigma*sqrt(-2*log(uniform(s)));
            double angle=2*PI*uniform(s);
            ni=radius*cos(angle); nq=radius*sin(angle);
            s->noise_energy+=ni*ni+nq*nq;
        }
        out[j]=(IQ){c->amplitude*cos(s->phase)+ni,c->amplitude*sin(s->phase)+nq};
    }
}
