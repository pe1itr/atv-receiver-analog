#include "source_pluto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#ifdef HAVE_LIBIIO
#include <iio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

static double monotonic_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER now,frequency;
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    return (double)now.QuadPart/frequency.QuadPart;
#else
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    return now.tv_sec+now.tv_nsec*1e-9;
#endif
}

typedef struct { unsigned shift, mask, sign; int swap; } SampleFormat;
static inline int unpack_sample(const char *p, const SampleFormat *f)
{
    uint16_t word; memcpy(&word,p,sizeof(word));
    if(f->swap) word=(uint16_t)((word>>8)|(word<<8));
    unsigned value=(word>>f->shift)&f->mask;
    return value>=f->sign?(int)value-(int)(2*f->sign):(int)value;
}

struct PlutoSource {
    struct iio_context *ctx;
    struct iio_device *rx;
    struct iio_channel *phy, *lo, *i, *q;
    struct iio_buffer *buffer;
    size_t capacity;
    float scale_i, scale_q;
    int native_format;
    SampleFormat format_i, format_q;
    uint64_t read_blocks, read_samples;
    double refill_seconds, conversion_seconds;
};
#define CHECK(call) do { int rc=(call); if(rc<0) { \
    fprintf(stderr,"Pluto: %s: %s\n",#call,strerror(-rc)); goto fail; } } while(0)

/* Same USB selection policy as datv-transmitter, selecting RX devices. */
static struct iio_context *open_context(const char *uri)
{
    if(uri && *uri) return iio_create_context_from_uri(uri);
    struct iio_scan_context *scan=iio_create_scan_context("usb",0);
    if(!scan) return NULL;
    struct iio_context_info **list=NULL;
    ssize_t n=iio_scan_context_get_info_list(scan,&list);
    struct iio_context *selected=NULL;
    for(ssize_t k=0;k<n;++k) {
        struct iio_context *ctx=iio_create_context_from_uri(iio_context_info_get_uri(list[k]));
        if(!ctx) continue;
        if(!iio_context_find_device(ctx,"ad9361-phy") || !iio_context_find_device(ctx,"cf-ad9361-lpc")) {
            iio_context_destroy(ctx); continue;
        }
        if(selected) {
            iio_context_destroy(ctx); iio_context_destroy(selected); selected=NULL;
            fprintf(stderr,"Pluto: multiple devices; select one with --pluto-uri\n"); break;
        }
        selected=ctx;
    }
    if(list) iio_context_info_list_free(list);
    iio_scan_context_destroy(scan);
    return selected;
}
void pluto_source_stop(PlutoSource *s)
{
    if(s && s->buffer) { iio_buffer_destroy(s->buffer); s->buffer=NULL; }
}
int pluto_source_tune(PlutoSource *s, double *hz, double *gain)
{
    /* Called on the RX thread. Recreate the buffer to discard old tuning. */
    pluto_source_stop(s);
    CHECK(iio_channel_attr_write_longlong(s->lo,"frequency",llround(*hz)));
    long long actual;
    CHECK(iio_channel_attr_read_longlong(s->lo,"frequency",&actual));
    *hz=(double)actual;
    /* AD9361 standard full gain tables (Analog Devices Linux driver):
     * <=1.3 GHz: 73 dB, <=4 GHz: 71 dB, above: 62 dB.
     * Clamp after tuning because crossing a band changes the gain range. */
    double max_gain=*hz<=1300000000?73:*hz<=4000000000?71:62;
    *gain=round(fmin(max_gain,fmax(0,*gain)));
    CHECK(iio_channel_attr_write_double(s->phy,"hardwaregain",*gain));
    CHECK(iio_channel_attr_read_double(s->phy,"hardwaregain",gain));
    return 0;
fail: return -1;
}
PlutoSource *pluto_source_open(Config *c)
{
    if(c->channel || strcmp(c->ref,"internal")) {
        fprintf(stderr,"Pluto: use --channel 0 and --ref internal\n"); return NULL;
    }
    PlutoSource *s=calloc(1,sizeof(*s));
    if(!s) return NULL;
    s->ctx=open_context(c->pluto_uri);
    if(!s->ctx) { fprintf(stderr,"Pluto: cannot open AD9361; check USB or --pluto-uri\n"); goto fail; }
    CHECK(iio_context_set_timeout(s->ctx,1000));
    struct iio_device *phy=iio_context_find_device(s->ctx,"ad9361-phy");
    s->rx=iio_context_find_device(s->ctx,"cf-ad9361-lpc");
    if(!phy || !s->rx) { fprintf(stderr,"Pluto: AD9361 RX devices missing\n"); goto fail; }
    s->phy=iio_device_find_channel(phy,"voltage0",false);
    s->lo=iio_device_find_channel(phy,"altvoltage0",true);
    s->i=iio_device_find_channel(s->rx,"voltage0",false);
    s->q=iio_device_find_channel(s->rx,"voltage1",false);
    if(!s->phy || !s->lo || !s->i || !s->q) { fprintf(stderr,"Pluto: RX channels missing\n"); goto fail; }
    CHECK(iio_channel_attr_write(s->phy,"rf_port_select","A_BALANCED"));
    CHECK(iio_channel_attr_write(s->phy,"gain_control_mode","manual"));
    CHECK(iio_channel_attr_write_longlong(s->phy,"sampling_frequency",llround(c->rate)));
    CHECK(iio_channel_attr_write_longlong(s->phy,"rf_bandwidth",llround(c->bandwidth)));
    long long actual;
    CHECK(iio_channel_attr_read_longlong(s->phy,"sampling_frequency",&actual));
    c->rate=(double)actual;
    CHECK(iio_channel_attr_read_longlong(s->i,"sampling_frequency",&actual));
    if(actual!=llround(c->rate)) { fprintf(stderr,"Pluto: FPGA and PHY sample rates differ\n"); goto fail; }
    CHECK(iio_channel_attr_read_longlong(s->phy,"rf_bandwidth",&actual));
    c->bandwidth=(double)actual;
    if(pluto_source_tune(s,&c->frequency,&c->gain)) goto fail;
    const struct iio_data_format *fi=iio_channel_get_data_format(s->i), *fq=iio_channel_get_data_format(s->q);
    if(!fi || !fq || fi->length!=16 || fq->length!=16 ||
       (fi->bits!=12 && fi->bits!=16) || (fq->bits!=12 && fq->bits!=16) ||
       !fi->is_signed || !fq->is_signed || fi->repeat>1 || fq->repeat>1 ||
       fi->bits+fi->shift>16 || fq->bits+fq->shift>16) {
        fprintf(stderr,"Pluto: unsupported IQ format (signed 12/16-bit required)\n"); goto fail;
    }
    s->scale_i=1.0f/(1u<<(fi->bits-1)); s->scale_q=1.0f/(1u<<(fq->bits-1));
    const uint16_t endian=1;
    int host_be=*(const unsigned char *)&endian==0;
    s->native_format=!fi->shift && !fq->shift && fi->is_be==host_be && fq->is_be==host_be &&
        (fi->is_fully_defined || fi->bits==16) && (fq->is_fully_defined || fq->bits==16);
    s->format_i=(SampleFormat){fi->shift,(1u<<fi->bits)-1,1u<<(fi->bits-1),fi->is_be!=host_be};
    s->format_q=(SampleFormat){fq->shift,(1u<<fq->bits)-1,1u<<(fq->bits-1),fq->is_be!=host_be};
    for(unsigned k=0;k<iio_device_get_channels_count(s->rx);++k) {
        struct iio_channel *ch=iio_device_get_channel(s->rx,k);
        if(iio_channel_is_scan_element(ch)) iio_channel_disable(ch);
    }
    iio_channel_enable(s->i); iio_channel_enable(s->q);
    CHECK(iio_device_set_kernel_buffers_count(s->rx,4));
    fprintf(stderr,"Pluto: RX1 / A_BALANCED, manual gain, libiio\n");
    fprintf(stderr,"Pluto IQ format: I %s:s%u/16>>%u; Q %s:s%u/16>>%u; %s conversion\n",
        fi->is_be?"be":"le",fi->bits,fi->shift,fq->is_be?"be":"le",fq->bits,fq->shift,
        s->native_format?"native":"inline signed");
    return s;
fail: pluto_source_close(s); return NULL;
}
int pluto_source_read(PlutoSource *s, IQ *iq, size_t n)
{
    if(!n) return 0;
    if(s->buffer && s->capacity!=n) pluto_source_stop(s);
    if(!s->buffer) {
        s->buffer=iio_device_create_buffer(s->rx,n,false); s->capacity=n;
        if(!s->buffer) { perror("Pluto RX buffer"); return -1; }
    }
    double before=monotonic_seconds();
    ssize_t bytes=iio_buffer_refill(s->buffer);
    double received=monotonic_seconds();
    s->refill_seconds+=received-before;
    if(bytes==-ETIMEDOUT || bytes==-EAGAIN) return 0;
    if(bytes==-EOVERFLOW) return -2;
    if(bytes<0) { fprintf(stderr,"Pluto RX: %s\n",strerror((int)-bytes)); return -1; }
    ptrdiff_t step=iio_buffer_step(s->buffer);
    char *pi=iio_buffer_first(s->buffer,s->i), *pq=iio_buffer_first(s->buffer,s->q);
    char *end=iio_buffer_end(s->buffer);
    if(step<4 || bytes%step || (size_t)(bytes/step)>n || !pi || !pq ||
       (bytes && (pi>end-2 || pq>end-2 ||
        (bytes/step-1)>(end-2-pi)/step || (bytes/step-1)>(end-2-pq)/step))) {
        fprintf(stderr,"Pluto: invalid RX buffer layout\n"); return -1;
    }
    size_t count=(size_t)(bytes/step);
    for(size_t k=0;k<count;++k) {
        int16_t vi,vq;
        if(s->native_format) { memcpy(&vi,pi+k*step,2); memcpy(&vq,pq+k*step,2); }
        else { vi=(int16_t)unpack_sample(pi+k*step,&s->format_i); vq=(int16_t)unpack_sample(pq+k*step,&s->format_q); }
        iq[k]=(IQ){vi*s->scale_i,vq*s->scale_q};
    }
    ++s->read_blocks; s->read_samples+=count;
    s->conversion_seconds+=monotonic_seconds()-received;
    return (int)count;
}
void pluto_source_close(PlutoSource *s)
{
    if(!s) return;
    pluto_source_stop(s);
    if(s->read_blocks) fprintf(stderr,"Pluto RX timing: %llu blocks; average refill %.3f ms; average conversion %.3f ms; read throughput %.3f MS/s\n",
        (unsigned long long)s->read_blocks,1000*s->refill_seconds/s->read_blocks,
        1000*s->conversion_seconds/s->read_blocks,
        s->read_samples/fmax(1e-9,s->refill_seconds+s->conversion_seconds)/1e6);
    if(s->i) iio_channel_disable(s->i);
    if(s->q) iio_channel_disable(s->q);
    if(s->ctx) iio_context_destroy(s->ctx);
    free(s);
}
#else
PlutoSource *pluto_source_open(Config *c)
{
    (void)c; fprintf(stderr,"Pluto: this build needs libiio >= 0.23 and < 1.0 (libiio-dev); rebuild with make -B\n"); return NULL;
}
int pluto_source_tune(PlutoSource *s,double *hz,double *gain) { (void)s; (void)hz; (void)gain; return -1; }
int pluto_source_read(PlutoSource *s,IQ *iq,size_t n) { (void)s; (void)iq; (void)n; return -1; }
void pluto_source_stop(PlutoSource *s) { (void)s; }
void pluto_source_close(PlutoSource *s) { (void)s; }
#endif
