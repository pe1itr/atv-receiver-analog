#include "test_signal.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

static double number(const char *s)
{
    char *end; errno=0; double v=strtod(s,&end);
    if(errno || end==s || *end || !isfinite(v)) { fprintf(stderr,"Invalid number: %s\n",s); exit(2); }
    return v;
}
static void help(void)
{
    puts("Offline FM-ATV PAL test transmitter (no radio access)\n"
         "  --output PATH       required; native float32 I,Q file, refuses overwrite\n"
         "  --seconds N         duration (default 1; 160 MB/s at 20 MS/s)\n"
         "  --rate SPS          default 20000000\n"
         "  --deviation HZ      sync-to-white frequency span (default 4000000)\n"
         "  --amplitude A       carrier magnitude 1e-9..1 (default 0.5)\n"
         "  --cnr-db DB         add complex AWGN; C/N over entire sample-rate bandwidth\n"
         "                      omitted = clean; smaller dB = more noise\n"
         "  --seed N            unsigned 64-bit noise seed (default 1)\n"
         "  --offset HZ         carrier offset from IQ center (default 0)\n"
         "  --line-offset-hz HZ offset from 15625 Hz (range -100..100)\n"
         "  --field-offset-hz HZ offset from 50 fields/s (range -1..1)\n"
         "  --color             PAL 4.43, 75% RGB colour bars (rate >= 12 MS/s)\n"
         "  --invert            reverse video modulation polarity\n"
         "Also writes PATH.json with settings and measured noise power.");
}
int main(int argc,char **argv)
{
    TestSignalConfig c={.rate=20e6,.deviation=4e6,.amplitude=.5,.seed=1};
    const char *path=NULL; double seconds=1;
    for(int j=1;j<argc;++j) {
        const char *o=argv[j];
        if(!strcmp(o,"--help")) { help(); return 0; }
        if(!strcmp(o,"--color")) { c.color=1; continue; }
        if(!strcmp(o,"--invert")) { c.invert=1; continue; }
        if(++j>=argc) { fprintf(stderr,"Missing value for %s\n",o); return 2; }
        const char *v=argv[j];
        if(!strcmp(o,"--output")) path=v;
        else if(!strcmp(o,"--seconds")) seconds=number(v);
        else if(!strcmp(o,"--rate")) c.rate=number(v);
        else if(!strcmp(o,"--deviation")) c.deviation=number(v);
        else if(!strcmp(o,"--amplitude")) c.amplitude=number(v);
        else if(!strcmp(o,"--cnr-db")) { c.cnr_db=number(v); c.noise=1; }
        else if(!strcmp(o,"--line-offset-hz")) c.line_offset_hz=number(v);
        else if(!strcmp(o,"--field-offset-hz")) c.field_offset_hz=number(v);
        else if(!strcmp(o,"--offset")) c.offset=number(v);
        else if(!strcmp(o,"--seed")) {
            char *end; errno=0;
            if(!*v || *v=='-') { fprintf(stderr,"Invalid seed\n"); return 2; }
            c.seed=strtoull(v,&end,10);
            if(errno || *end) { fprintf(stderr,"Invalid seed\n"); return 2; }
        } else { fprintf(stderr,"Unknown option: %s\n",o); return 2; }
    }
    if(fabs(c.line_offset_hz)>100 || fabs(c.field_offset_hz)>1 || !path || seconds<=0 || seconds>3600 || c.rate<8e6 || c.rate>64e6 ||
       c.deviation<=0 || fabs(c.offset)+.7*c.deviation>=c.rate/2 ||
       c.amplitude<1e-9 || c.amplitude>1 || c.cnr_db< -60 || c.cnr_db>120 || sizeof(float)!=4 || FLT_RADIX!=2) {
        fprintf(stderr,"Invalid settings; see --help. Rate 8..64 MS/s, duration 0..3600 s, C/N -60..120 dB.\n"); return 2;
    }
    if(c.color && c.rate<12e6) { fprintf(stderr,"PAL color requires at least 12 MS/s\n"); return 2; }
    uint64_t samples=(uint64_t)llround(seconds*c.rate);
    if(!samples) { fprintf(stderr,"Duration is shorter than one sample\n"); return 2; }
    char *meta=malloc(strlen(path)+6);
    TestSignal *s=malloc(sizeof(*s)); IQ *iq=malloc(BLOCK*sizeof(*iq));
    if(!meta || !s || !iq) { free(meta); free(s); free(iq); return 1; }
    sprintf(meta,"%s.json",path);
    FILE *out=fopen(path,"wbx"), *info=NULL;
    int rc=1;
    if(!out) { perror(path); goto done; }
    info=fopen(meta,"wx");
    if(!info) { perror(meta); goto done; }
    test_signal_init(s,c);
    fprintf(stderr,"Writing %.3f s, %.1f MB: %s\n",samples/c.rate,samples*8/1e6,path);
    while(s->sample<samples) {
        size_t n=samples-s->sample<BLOCK?(size_t)(samples-s->sample):BLOCK;
        test_signal_generate(s,iq,n);
        if(fwrite(iq,sizeof(*iq),n,out)!=n) { perror("IQ write"); goto done; }
    }
    if(fflush(out)) { perror("IQ flush"); goto done; }
    uint16_t endian=1;
    double power=c.amplitude*c.amplitude;
    double noise=s->noise_energy/samples;
    fprintf(info,"{\n  \"format\": \"%s\",\n  \"byte_order\": \"%s\",\n"
        "  \"sample_type\": \"float32\",\n  \"layout\": \"IQ\",\n  \"header_bytes\": 0,\n  \"bytes_per_complex_sample\": 8,\n"
        "  \"pattern\": \"%s\",\n"
        "  \"sample_rate\": %.17g,\n  \"samples\": %llu,\n  \"seconds\": %.17g,\n"
        "  \"deviation_hz\": %.17g,\n  \"amplitude\": %.17g,\n  \"offset_hz\": %.17g,\n"
        "  \"invert\": %s,\n  \"seed\": \"%llu\",\n  \"noise_model\": \"complex AWGN\",\n"
        "  \"cnr_bandwidth_hz\": %.17g,\n  \"signal_power\": %.17g,\n  \"measured_noise_power\": %.17g,\n",
        *(uint8_t*)&endian?"cf32_le":"cf32_be",*(uint8_t*)&endian?"little":"big",c.color?"PAL 4.43 75% RGB bars":"ATV 123 monochrome, independent line and field clocks",c.rate,(unsigned long long)samples,samples/c.rate,c.deviation,c.amplitude,c.offset,
        c.invert?"true":"false",(unsigned long long)c.seed,c.rate,power,noise);
    fprintf(info,"  \"line_offset_hz\": %.17g,\n  \"field_offset_hz\": %.17g,\n  \"line_hz\": %.17g,\n  \"field_hz\": %.17g,\n", c.line_offset_hz,c.field_offset_hz,15625+c.line_offset_hz,50+c.field_offset_hz);
    if(c.noise) fprintf(info,"  \"cnr_db\": %.17g,\n  \"measured_cnr_db\": %.17g\n}\n",c.cnr_db,10*log10(power/noise));
    else fputs("  \"cnr_db\": null,\n  \"measured_cnr_db\": null\n}\n",info);
    if(ferror(info) || fflush(info)) { perror("Metadata write"); goto done; }
    if(c.noise) fprintf(stderr,"C/N requested %.2f dB; measured %.2f dB over %.3f MHz\n",c.cnr_db,10*log10(power/noise),c.rate/1e6);
    rc=0;
done:
    if(out && fclose(out)) rc=1;
    if(info && fclose(info)) rc=1;
    if(rc && out) fprintf(stderr,"Generation incomplete; check/remove partial output before retrying.\n");
    free(meta); free(s); free(iq); return rc;
}
