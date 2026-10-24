#include "atv.h"
#include "rx_queue.h"
#include "source_pluto.h"
#include "settings.h"
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Errors.h>
#ifndef ATV_NO_UHD
#include <uhd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#ifdef _WIN32
#define ftell _ftelli64
#define fseek _fseeki64
#endif

struct Source {
    Config *c;
    RxQueue *queue;
    SourceStats previous_stats;
#ifndef ATV_NO_UHD
    uhd_usrp_handle u;
    uhd_rx_streamer_handle stream;
    uhd_rx_metadata_handle md;
#else
    void *u; /* Always NULL in builds without UHD. */
#endif
    SoapySDRDevice *soapy;
    SoapySDRStream *ss;
    PlutoSource *pluto;
    FILE *file;
    uint64_t tick;
    double phase;
    int started;
    IQ *demo;
    size_t demo_length, demo_pos;
    double demo_rotation, demo_step;
};
#ifndef ATV_NO_UHD
static int uc(uhd_error e, const char *op)
{
    if (!e) return 0;
    char msg[512] = "";
    uhd_get_last_error(msg, sizeof(msg));
    fprintf(stderr, "%s: UHD %d: %s\n", op, e, msg); return -1;
}
#endif
static int sc(int e, const char *op)
{
    if (!e) return 0;
    fprintf(stderr, "%s: %s\n", op, SoapySDRDevice_lastError()); return -1;
}
#define U(call) do { if (uc((call), #call)) goto fail; } while (0)
#define S(call) do { if (sc((call), #call)) goto fail; } while (0)

static int tune_now(Source *s, double *hz, double *gain)
{
    if (!strcmp(s->c->mode,"sdrplay") && (!isfinite(*hz) || *hz<1000 || *hz>2e9)) {
        fprintf(stderr,"SDRplay tuning requires 1 kHz..2 GHz after converter LO\n");
        return -1;
    }
    if (s->pluto) return pluto_source_tune(s->pluto,hz,gain);
#ifndef ATV_NO_UHD
    if (s->u) {
        uhd_tune_request_t req = {.target_freq=*hz, .rf_freq_policy=UHD_TUNE_REQUEST_POLICY_AUTO,
            .dsp_freq_policy=UHD_TUNE_REQUEST_POLICY_AUTO, .args=""};
        uhd_tune_result_t result;
        U(uhd_usrp_set_rx_freq(s->u, &req, s->c->channel, &result));
        uhd_meta_range_handle range;
        U(uhd_meta_range_make(&range));
        uhd_error range_error=uhd_usrp_get_rx_gain_range(s->u,"",s->c->channel,range);
        if(!range_error) range_error=uhd_meta_range_clip(range,*gain,true,gain);
        uhd_meta_range_free(&range);
        U(range_error);
        U(uhd_usrp_set_rx_gain(s->u, *gain, s->c->channel, ""));
        U(uhd_usrp_get_rx_freq(s->u, s->c->channel, hz));
        U(uhd_usrp_get_rx_gain(s->u, s->c->channel, "", gain));
    } else
#endif
    if (s->soapy) {
        S(SoapySDRDevice_setFrequency(s->soapy, SOAPY_SDR_RX, s->c->channel, *hz, NULL));
        if(!strcmp(s->c->mode,"sdrplay")) {
            /* SDRplay exposes attenuation, not positive gain. Keep the UI's
             * increasing-gain convention: 0..39 dB above minimum IF gain. */
            double reduction=59-round(fmin(39,fmax(0,*gain)));
            S(SoapySDRDevice_setGainElement(s->soapy, SOAPY_SDR_RX, 0, "IFGR", reduction));
            *gain=59-SoapySDRDevice_getGainElement(s->soapy, SOAPY_SDR_RX, 0, "IFGR");
        } else if(!strcmp(s->c->mode,"hackrf")) {
            /* Bypass Soapy's total-gain distribution. Keep VGA and RF amp
             * fixed; only the LNA follows the receiver gain control. */
            *gain=8*round(fmin(40,fmax(0,*gain))/8);
            S(SoapySDRDevice_setGainElement(s->soapy,SOAPY_SDR_RX,s->c->channel,"VGA",s->c->hackrf_vga));
            S(SoapySDRDevice_setGainElement(s->soapy,SOAPY_SDR_RX,s->c->channel,"AMP",14));
            S(SoapySDRDevice_setGainElement(s->soapy,SOAPY_SDR_RX,s->c->channel,"LNA",*gain));
            *gain=SoapySDRDevice_getGainElement(s->soapy,SOAPY_SDR_RX,s->c->channel,"LNA");
            S(SoapySDRDevice_lastStatus());
        } else {
            SoapySDRRange range=SoapySDRDevice_getGainRange(s->soapy,SOAPY_SDR_RX,s->c->channel);
            S(SoapySDRDevice_lastStatus());
            *gain=fmin(range.maximum,fmax(range.minimum,*gain));
            if(range.step>0)
                *gain=fmin(range.maximum,range.minimum+round((*gain-range.minimum)/range.step)*range.step);
            S(SoapySDRDevice_setGain(s->soapy, SOAPY_SDR_RX, s->c->channel, *gain));
            *gain = SoapySDRDevice_getGain(s->soapy, SOAPY_SDR_RX, s->c->channel);
        }
        *hz = SoapySDRDevice_getFrequency(s->soapy, SOAPY_SDR_RX, s->c->channel);
    }
    return 0;
fail: return -1;
}
typedef struct { Source *s; double *hz, *gain; } Tune;
static int tune_control(void *opaque)
{
    Tune *t=opaque;
    return tune_now(t->s,t->hz,t->gain);
}
int source_tune(Source *s, double *hz, double *gain)
{
    Tune t={s,hz,gain};
    /* UHD permits settings on a separate thread from recv(). Keep draining
     * the B210 during its slow control calls instead of overflowing USB. */
    if(s->u && s->queue) return rx_queue_control_live(s->queue,tune_control,&t);
    return s->queue ? rx_queue_control(s->queue,tune_control,&t) : tune_now(s,hz,gain);
}
Source *source_open(Config *c)
{
    Source *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->c = c;
    if(!strcmp(c->mode,"hackrf") && (!isfinite(c->hackrf_vga) || c->hackrf_vga<0 || c->hackrf_vga>62 || fmod(c->hackrf_vga,2)!=0)) {
        fprintf(stderr,"HackRF VGA must be 0..62 dB in 2 dB steps\n"); goto fail;
    }
    int rsp=!strcmp(c->mode,"sdrplay");
    if(rsp && (!isfinite(c->rate) || c->rate<8e6 || c->rate>=10e6 ||
       !isfinite(c->bandwidth) || c->bandwidth<=0 || c->bandwidth>8e6 ||
       !isfinite(c->frequency) || c->frequency<1000 || c->frequency>2e9 || c->channel!=0)) {
        fprintf(stderr,"SDRplay: requires 8 <= rate < 10 MS/s, BW <= 8 MHz, RF 1 kHz..2 GHz, channel 0\n");
        goto fail;
    }
    if (!strcmp(c->mode, "demo")) {
        s->demo_length=(size_t)llround(c->rate*.040);
        s->demo=malloc(s->demo_length*sizeof(IQ));
        if (!s->demo) goto fail;
        demo_generate(s->demo,s->demo_length,c->rate,c->deviation,&s->tick,&s->phase);
        s->demo_step=s->phase;
        return s;
    }
    if (!strcmp(c->mode, "file")) {
        s->file = fopen(c->file, "rb");
        if (!s->file) { perror(c->file); goto fail; }
        return s;
    }
    if (!strcmp(c->mode, "b210")) {
#ifndef ATV_NO_UHD
        const char *files[] = {"usrp_b210_fpga.bin", "usrp_b200_fw.hex"};
        char path[4096];
        for (int i = 0; i < 2; ++i) {
            int n = snprintf(path, sizeof(path), "%s/%s", c->images, files[i]);
            if (n < 0 || n >= (int)sizeof(path) || access(path, R_OK)) {
                fprintf(stderr, "Kintex-7 image missing: %s; set --uhd-images-dir\n", path); goto fail;
            }
        }
        if (setenv("UHD_IMAGES_DIR", c->images, 1)) { perror("setenv"); goto fail; }
        fprintf(stderr, "Kintex-7 images: %s; RX channel %zu, antenna %s\n", c->images, c->channel, c->antenna);
        U(uhd_usrp_make(&s->u, c->args));
        U(uhd_usrp_set_clock_source(s->u, c->ref, 0));
        U(uhd_usrp_set_rx_rate(s->u, c->rate, c->channel));
        U(uhd_usrp_get_rx_rate(s->u, c->channel, &c->rate));
        U(uhd_usrp_set_rx_bandwidth(s->u, c->bandwidth, c->channel));
        U(uhd_usrp_get_rx_bandwidth(s->u, c->channel, &c->bandwidth));
        U(uhd_usrp_set_rx_antenna(s->u, c->antenna, c->channel));
        if (source_tune(s, &c->frequency, &c->gain)) goto fail;
        U(uhd_rx_streamer_make(&s->stream));
        U(uhd_rx_metadata_make(&s->md));
        uhd_stream_args_t args = {.cpu_format="fc32", .otw_format="sc16", .args="",
            .channel_list=&c->channel, .n_channels=1};
        U(uhd_usrp_get_rx_stream(s->u, &args, s->stream));

#else
        fprintf(stderr,"B210/UHD is not included in this build. Choose HackRF or Pluto.\n"); goto fail;
#endif

    } else if (!strcmp(c->mode,"pluto")) {
        s->pluto=pluto_source_open(c);
        if(!s->pluto) goto fail;
    } else {
        if(rsp) {
            SoapySDRKwargs args=SoapySDRKwargs_fromString(c->args);
            SoapySDRKwargs_set(&args,"driver","sdrplay");
            s->soapy=SoapySDRDevice_make(&args);
            SoapySDRKwargs_clear(&args);
        } else s->soapy = SoapySDRDevice_makeStrArgs(*c->args ? c->args : "driver=hackrf");
        if (!s->soapy) { fprintf(stderr, "Soapy: %s%s\n", SoapySDRDevice_lastError(), rsp?"; install SDRplay API 3.15+ and SoapySDRPlay3; check API service and USB connection":""); goto fail; }
        S(SoapySDRDevice_setSampleRate(s->soapy, SOAPY_SDR_RX, c->channel, c->rate));
        c->rate = SoapySDRDevice_getSampleRate(s->soapy, SOAPY_SDR_RX, c->channel);
        S(SoapySDRDevice_setBandwidth(s->soapy, SOAPY_SDR_RX, c->channel, c->bandwidth));
        c->bandwidth = SoapySDRDevice_getBandwidth(s->soapy, SOAPY_SDR_RX, c->channel);
        if(rsp) {
            S(SoapySDRDevice_setGainMode(s->soapy, SOAPY_SDR_RX, 0, false));
            S(SoapySDRDevice_setGainElement(s->soapy, SOAPY_SDR_RX, 0, "RFGR", 0));
        }
        if (source_tune(s, &c->frequency, &c->gain)) goto fail;
        s->ss = SoapySDRDevice_setupStream(s->soapy, SOAPY_SDR_RX, SOAPY_SDR_CF32, &c->channel, 1, NULL);
        if (!s->ss) { fprintf(stderr, "setupStream: %s\n", SoapySDRDevice_lastError()); goto fail; }

    }
    if (!isfinite(c->rate) || c->rate < settings_min_rate(c->mode) || c->rate > 64e6) {
        fprintf(stderr, "Actual sample rate outside %.0f..64 MS/s\n",settings_min_rate(c->mode)/1e6); goto fail;
    }
    if(rsp && (c->rate>=10e6 || !isfinite(c->bandwidth) || c->bandwidth<=0 || c->bandwidth>8e6)) {
        fprintf(stderr,"Actual SDRplay rate/BW outside requested limits\n"); goto fail;
    }
    fprintf(stderr, "Actual RX: %.6f MHz, %.3f MS/s, BW %.3f MHz, gain %.1f dB\n",
        c->frequency/1e6, c->rate/1e6, c->bandwidth/1e6, c->gain);
    if(!strcmp(c->mode,"hackrf"))
        fprintf(stderr,"HackRF: RX LNA %.0f dB (UI), RX VGA %.0f dB (fixed), RF AMP 14 dB (on)\n",c->gain,c->hackrf_vga);
    return s;
fail: source_close(s); return NULL;
}
static int hardware_read(void *opaque, IQ *iq, size_t n)
{
    Source *s=opaque;
    if(s->pluto) return pluto_source_read(s->pluto,iq,n);
    if(!s->started) {
    #ifndef ATV_NO_UHD
        if(s->u) {
            uhd_stream_cmd_t cmd={.stream_mode=UHD_STREAM_MODE_START_CONTINUOUS,.stream_now=true};
            if(uc(uhd_rx_streamer_issue_stream_cmd(s->stream,&cmd),"start RX")) return -1;
        } else
    #endif
        {
            if(!s->ss) {
                s->ss=SoapySDRDevice_setupStream(s->soapy,SOAPY_SDR_RX,SOAPY_SDR_CF32,&s->c->channel,1,NULL);
                if(!s->ss) { fprintf(stderr,"setupStream: %s\n",SoapySDRDevice_lastError()); return -1; }
            }
            if(sc(SoapySDRDevice_activateStream(s->soapy,s->ss,0,0,0),"activate RX")) return -1;
        }
        s->started=1;
    }
    void *buffers[] = {iq};
#ifndef ATV_NO_UHD
    if (s->u) {
        size_t got = 0;
        if (uc(uhd_rx_streamer_recv(s->stream, buffers, n, &s->md, .1, false, &got), "recv")) return -1;
        uhd_rx_metadata_error_code_t e;
        if (uc(uhd_rx_metadata_error_code(s->md, &e), "metadata")) return -1;
        if (e == UHD_RX_METADATA_ERROR_CODE_TIMEOUT) return 0;
        if (e == UHD_RX_METADATA_ERROR_CODE_OVERFLOW) return -2;
        if (e != UHD_RX_METADATA_ERROR_CODE_NONE) {
            char msg[512]; uhd_rx_metadata_strerror(s->md, msg, sizeof(msg));
            fprintf(stderr, "RX: %s\n", msg); return -1;
        }
        return (int)got;
    }
#endif
    if (s->soapy) {
        int flags = 0; long long time = 0;
        int got = SoapySDRDevice_readStream(s->soapy, s->ss, buffers, n, &flags, &time, 100000);
        if (got == SOAPY_SDR_TIMEOUT) return 0;
        if (got == SOAPY_SDR_OVERFLOW) return -2;
        if (got < 0) { fprintf(stderr, "RX: %s\n", SoapySDR_errToStr(got)); return -1; }
        return got;
    }
    return -1;
}
int source_read(Source *s, IQ *iq, size_t n)
{
    if(s->u || s->soapy || s->pluto) {
        if(!s->queue) {
            size_t slots=(size_t)ceil(s->c->rate*.1/n);
            if(slots<4) slots=4;
            if(slots>256) slots=256;
            s->queue=rx_queue_create(n,slots,hardware_read,s);
            if(!s->queue) { fprintf(stderr,"Cannot create SDR receive queue\n"); return -1; }
            fprintf(stderr,"SDR receive thread: %zu samples/block, %.1f ms IQ buffer\n",
                n,1000.0*n*slots/s->c->rate);
        }
        return rx_queue_read(s->queue,iq,n);
    }
    if (s->file) {
        size_t got = fread(iq, sizeof(*iq), n, s->file);
        if (ferror(s->file)) { perror("IQ read"); return -1; }
        if(!got && s->c->loop) {
            if(ftell(s->file)==0) { fprintf(stderr,"Cannot loop an empty IQ file\n"); return -1; }
            if(fseek(s->file,0,SEEK_SET)) { perror("IQ rewind"); return -1; }
            return -3;
        }
        return (int)got;
    }
    size_t done=0;
    while(done<n) {
        size_t part=s->demo_length-s->demo_pos;
        if(part>n-done) part=n-done;
        float re=cos(s->demo_rotation), im=sin(s->demo_rotation);
        for(size_t j=0;j<part;++j) {
            IQ v=s->demo[s->demo_pos+j];
            iq[done+j]=(IQ){v.i*re-v.q*im,v.i*im+v.q*re};
        }
        done+=part; s->demo_pos+=part;
        if(s->demo_pos==s->demo_length) {
            s->demo_pos=0; s->demo_rotation=remainder(s->demo_rotation+s->demo_step,2*PI);
        }
    }
    return (int)n;
}
void source_get_stats(Source *s, SourceStats *stats)
{
    memset(stats,0,sizeof(*stats));
    if(s && s->queue) rx_queue_stats(s->queue,stats);
    if(s) {
        stats->hardware_overflows+=s->previous_stats.hardware_overflows;
        stats->discarded_samples+=s->previous_stats.discarded_samples;
        if(stats->peak_samples<s->previous_stats.peak_samples) stats->peak_samples=s->previous_stats.peak_samples;
        if(!stats->capacity_samples) stats->capacity_samples=s->previous_stats.capacity_samples;
    }
}
void source_stop(Source *s)
{
    if(!s) return;
    rx_queue_stop(s->queue);
    pluto_source_stop(s->pluto);
#ifndef ATV_NO_UHD
    if(s->started && s->stream) {
        uhd_stream_cmd_t cmd={.stream_mode=UHD_STREAM_MODE_STOP_CONTINUOUS,.stream_now=true};
        uc(uhd_rx_streamer_issue_stream_cmd(s->stream,&cmd),"stop RX");
    }
#endif
    if(s->started && s->ss) SoapySDRDevice_deactivateStream(s->soapy,s->ss,0,0);
    s->started=0;
}
void source_pause(Source *s)
{
    if(!s) return;
    source_stop(s);
    /* SoapyHackRF retains a partially consumed read buffer across deactivate /
     * activate, while resetting its ring counters. Recreate the stream so the
     * next read cannot release an old buffer into the new receive session.
     * source_stop has joined the RX thread before we touch the stream. */
    if(s->ss) {
        SoapySDRDevice_closeStream(s->soapy,s->ss);
        s->ss=NULL;
    }
    SourceStats stats; source_get_stats(s,&stats);
    stats.queued_samples=0; s->previous_stats=stats;
    rx_queue_destroy(s->queue); s->queue=NULL;
}
void source_close(Source *s)
{
    if(!s) return;
    source_stop(s);
    rx_queue_destroy(s->queue);
#ifndef ATV_NO_UHD
    if(s->stream) uhd_rx_streamer_free(&s->stream);
    if(s->md) uhd_rx_metadata_free(&s->md);
    if(s->u) uhd_usrp_free(&s->u);
#endif
    if(s->ss) SoapySDRDevice_closeStream(s->soapy,s->ss);
    if(s->soapy) SoapySDRDevice_unmake(s->soapy);
    pluto_source_close(s->pluto);
    if(s->file) fclose(s->file);
    free(s->demo);
    free(s);
}
