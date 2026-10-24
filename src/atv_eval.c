/* Offline evaluator uses exactly the receiver's IF and decoder implementations. */
#include "atv.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint8_t reference[WIDTH*HEIGHT], last[WIDTH*HEIGHT];
    int have_reference;
    uint64_t fields;
    double squared_error, absolute_error;
} Measurement;
static void frame(void *opaque,const uint32_t *pixels)
{
    Measurement *m=opaque;
    for(int k=0;k<WIDTH*HEIGHT;++k) {
        m->last[k]=(uint8_t)pixels[k];
        if(m->have_reference) {
            double error=(double)m->last[k]-m->reference[k];
            m->squared_error+=error*error; m->absolute_error+=fabs(error);
        }
    }
    ++m->fields;
}
static double number(const char *s)
{
    char *end; errno=0; double v=strtod(s,&end);
    if(errno || end==s || *end || !isfinite(v)) { fprintf(stderr,"Invalid number: %s\n",s); exit(2); }
    return v;
}
int main(int argc,char **argv)
{
    const char *path=NULL,*reference=NULL,*output=NULL;
    double rate=20e6,deviation=4e6,bw=18e6,line_hz=15625;
    int invert=0,free_sync=0,fm_mode=0;
    double alignment_bw=0;
    for(int j=1;j<argc;++j) {
        const char *o=argv[j];
        if(!strcmp(o,"--help")) {
            puts("atv-eval --file IQ [--rate SPS] [--deviation HZ] [--if-bandwidth HZ]\n"
                 "         [--free-sync] [--line-hz HZ] [--invert] [--fm-mode 0|1] [--alignment-bandwidth HZ]\n"
                 "         [--reference clean.pgm] [--output last-field.pgm]\n"
                 "Outputs JSON metrics. Reference is a 640x288 8-bit P5 PGM written by this tool.\n"
                 "No fields = null image metrics, no output image; process still succeeds."); return 0;
        }
        if(!strcmp(o,"--free-sync")) { free_sync=1; continue; }
        if(!strcmp(o,"--invert")) { invert=1; continue; }
        if(++j>=argc) { fprintf(stderr,"Missing value for %s\n",o); return 2; }
        const char *v=argv[j];
        if(!strcmp(o,"--file")) path=v;
        else if(!strcmp(o,"--reference")) reference=v;
        else if(!strcmp(o,"--output")) output=v;
        else if(!strcmp(o,"--rate")) rate=number(v);
        else if(!strcmp(o,"--deviation")) deviation=number(v);
        else if(!strcmp(o,"--if-bandwidth")) bw=number(v);
        else if(!strcmp(o,"--alignment-bandwidth")) alignment_bw=number(v);
        else if(!strcmp(o,"--fm-mode")) {
            if(strcmp(v,"0") && strcmp(v,"1")) { fprintf(stderr,"Invalid FM mode\n"); return 2; }
            fm_mode=atoi(v);
        }
        else if(!strcmp(o,"--line-hz")) line_hz=number(v);
        else { fprintf(stderr,"Unknown option: %s\n",o); return 2; }
    }
    if(alignment_bw<0 || alignment_bw>rate || (alignment_bw>0 && alignment_bw<250000) || !path || rate<8e6 || rate>64e6 || deviation<=0 || deviation>=rate/2 || bw<250000 || bw>rate || line_hz<15000 || line_hz>16250) {
        fprintf(stderr,"Invalid settings; see --help\n"); return 2;
    }
    Measurement *m=calloc(1,sizeof(*m)); Decoder *d=malloc(sizeof(*d)); IQ *iq=malloc(BLOCK*sizeof(*iq));
    IFFilter *filter=if_filter_create(rate,bw);
    IQ *guide_iq=malloc(BLOCK*sizeof(*guide_iq));
    float *video=malloc(BLOCK*sizeof(*video)), *guide=malloc(BLOCK*sizeof(*guide));
    IFFilter *guide_filter=alignment_bw?if_filter_create(rate,alignment_bw):NULL;
    FMDemod vf,gf;
    fm_demod_init(&vf,rate,deviation,invert,1); vf.mode=fm_mode;
    unsigned stride=fm_mode?1:(unsigned)fmax(1,floor(rate/fmax(4e6,2*alignment_bw)));
    fm_demod_init(&gf,rate,deviation,invert,stride); gf.mode=fm_mode;
    double dsp_seconds=0;
    FILE *in=NULL; int rc=1;
    if(!m || !d || !iq || !filter || !guide_iq || !video || !guide || (alignment_bw && !guide_filter)) goto done;
    if(reference) {
        FILE *ref=fopen(reference,"rb");
        if(!ref) { perror(reference); goto done; }
        char header[64];
        int valid=fgets(header,sizeof(header),ref) && !strcmp(header,"P5\n") &&
            fgets(header,sizeof(header),ref) && !strcmp(header,"640 288\n") &&
            fgets(header,sizeof(header),ref) && !strcmp(header,"255\n") &&
            fread(m->reference,1,sizeof(m->reference),ref)==sizeof(m->reference);
        fclose(ref);
        if(!valid) { fprintf(stderr,"Reference must be a PGM produced by atv-eval\n"); goto done; }
        m->have_reference=1;
    }
    in=fopen(path,"rb"); if(!in) { perror(path); goto done; }
    decoder_init(d,rate,deviation,invert,frame,m); d->free_sync=free_sync; d->line_hz=line_hz;
    uint64_t samples=0;
    for(;;) {
        size_t bytes=fread(iq,1,BLOCK*sizeof(*iq),in);
        if(bytes%sizeof(*iq)) { fprintf(stderr,"Truncated IQ pair\n"); goto done; }
        size_t n=bytes/sizeof(*iq);
        if(!n) break;
        for(size_t j=0;j<n;++j) if(!isfinite(iq[j].i) || !isfinite(iq[j].q) || fabsf(iq[j].i)>1e6 || fabsf(iq[j].q)>1e6) {
            fprintf(stderr,"Invalid IQ sample\n"); goto done;
        }
        clock_t start=clock();
        samples+=n;
        if(guide_filter) {
            memcpy(guide_iq,iq,n*sizeof(*iq)); if_filter_process(guide_filter,guide_iq,n);
            fm_demod_process(&gf,guide_iq,guide,n);
        }
        if_filter_process(filter,iq,n); fm_demod_process(&vf,iq,video,n);
        decoder_process_video(d,video,guide_filter?guide:NULL,n);
        dsp_seconds+=(double)(clock()-start)/CLOCKS_PER_SEC;
    }
    if(ferror(in)) { perror("IQ read"); goto done; }
    if(!samples) { fprintf(stderr,"Empty IQ file\n"); goto done; }
    if(output && m->fields) {
        FILE *out=fopen(output,"wbx");
        if(!out) { perror(output); goto done; }
        int ok=fprintf(out,"P5\n640 288\n255\n")>0 && fwrite(m->last,1,sizeof(m->last),out)==sizeof(m->last);
        if(fclose(out)) ok=0;
        if(!ok) { fprintf(stderr,"Image write failed\n"); goto done; }
    }
    uint64_t expected=(uint64_t)floor((samples-1)*50.0/rate);
    printf("{\"samples\":%llu,\"seconds\":%.9g,\"if_bandwidth\":%.9g,\"free_sync\":%s,"
        "\"fields\":%llu,\"expected_fields\":%llu,\"field_fraction\":",
        (unsigned long long)samples,samples/rate,bw,free_sync?"true":"false",
        (unsigned long long)m->fields,(unsigned long long)expected);
    if(expected) printf("%.9g",(double)m->fields/expected); else printf("null");
    if(m->have_reference && m->fields) {
        double n=(double)m->fields*WIDTH*HEIGHT, mse=m->squared_error/n;
        printf(",\"mae\":%.9g,\"mse\":%.9g,\"psnr_db\":",m->absolute_error/n,mse);
        if(mse>0) printf("%.9g",10*log10(255.0*255/mse)); else printf("null");
        printf(",\"perfect_match\":%s",mse==0?"true":"false");
    } else printf(",\"mae\":null,\"mse\":null,\"psnr_db\":null,\"perfect_match\":false");
    printf(",\"fm_mode\":%d,\"alignment_bandwidth\":%.9g,\"dsp_cpu_seconds\":%.9g,\"hsync_updates\":%u,\"vsync_updates\":%u",fm_mode,alignment_bw,dsp_seconds,d->hsync_updates,d->vsync_updates);
    puts("}"); rc=ferror(stdout)?1:0;
done:
    if(in) fclose(in);
    if_filter_destroy(guide_filter); free(guide_iq); free(video); free(guide);
    if_filter_destroy(filter); free(m); free(d); free(iq); return rc;
}
