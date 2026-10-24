#include "settings.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include "platform.h"

typedef struct { const char *key; size_t offset; char type; double min,max; int slot; } Field;
#define C(n,t,lo,hi) {#n,offsetof(Settings,c)+offsetof(Config,n),t,lo,hi,0}
#define I(n,lo,hi) {#n,offsetof(Settings,n),'i',lo,hi,0}
#define S(n,k) {#n,offsetof(Settings,c)+offsetof(Config,n),'s',0,0,k}
static const Field fields[]={
    S(mode,0), S(args,1), S(images,2), S(antenna,3), S(ref,4), S(file,5), S(pluto_uri,6), S(audio_output,7),
    S(status_bind,8), C(status_enabled,'i',0,1), C(status_port,'i',1,65535),
    C(rate,'d',5e6,64e6), C(frequency,'d',1,1e12), C(lo,'d',-1e12,1e12),
    C(bandwidth,'d',1,64e6), C(if_bandwidth,'d',250000,64e6),
    C(alignment_bandwidth,'d',0,64e6), C(gain,'d',0,100), C(hackrf_vga,'d',0,62),
    C(line_hz,'d',15000,16250), C(rx_block,'z',2048,1048576), C(channel,'z',0,1),
    C(invert,'i',0,1), C(free_sync,'i',0,1), C(loop,'i',0,1),
    C(fm_mode,'i',0,1), C(video_deemphasis,'i',0,1), C(experimental_sync,'i',0,1),
    I(stacking,0,1), I(stack_frames,1,512), I(rolling,0,1), I(paused,0,1),
    I(vertical_shift,-312,312), I(audio_source,0,3), I(audio_muted,0,1), I(audio_swap,0,1)
};
static const Field *field(const char *key)
{
    for(size_t i=0;i<sizeof(fields)/sizeof(*fields);++i)
        if(!strcmp(key,fields[i].key)) return &fields[i];
    return NULL;
}
static void value(const Settings *s,const Field *f,char *buf,size_t n)
{
    const void *p=(const char *)s+f->offset;
    if(f->type=='s') { const char *v=*(const char *const *)p; snprintf(buf,n,"%s",v?v:""); }
    else if(f->type=='d') snprintf(buf,n,"%.17g",*(const double *)p);
    else if(f->type=='z') snprintf(buf,n,"%zu",*(const size_t *)p);
    else snprintf(buf,n,"%d",*(const int *)p);
}
static int assign(Settings *s,const Field *f,const char *v)
{
    void *p=(char *)s+f->offset;
    if(f->type=='s') {
        if(strlen(v)>=sizeof(s->strings[0]) || strchr(v,'\n') || strchr(v,'\r')) return 0;
        if(!strcmp(f->key,"mode") && strcmp(v,"demo") && strcmp(v,"file") && strcmp(v,"b210") && strcmp(v,"hackrf") && strcmp(v,"pluto") && strcmp(v,"sdrplay")) return 0;
        if(!strcmp(f->key,"ref") && strcmp(v,"internal") && strcmp(v,"external") && strcmp(v,"gpsdo")) return 0;
        if(!strcmp(f->key,"antenna") && strcmp(v,"TX/RX") && strcmp(v,"RX2")) return 0;
        strcpy(s->strings[f->slot],v); *(const char **)p=s->strings[f->slot]; return 1;
    }
    char *end; errno=0; double x=strtod(v,&end);
    if(errno || end==v || *end || !isfinite(x) || x<f->min || x>f->max || (f->type!='d' && floor(x)!=x)) return 0;
    if(!strcmp(f->key,"alignment_bandwidth") && x!=0 && x<250000) return 0;
    if(!strcmp(f->key,"hackrf_vga") && fmod(x,2)!=0) return 0;
    if(f->type=='d') *(double *)p=x;
    else if(f->type=='z') *(size_t *)p=(size_t)x;
    else *(int *)p=(int)x;
    return 1;
}
/* 1 line, 0 EOF, -1 overlong/error. Drain long input before retrying. */
static int read_line(FILE *in,char *buf,size_t n)
{
    if(!fgets(buf,(int)n,in)) return ferror(in)?-1:0;
    size_t len=strlen(buf);
    if(len && buf[len-1]!='\n' && !feof(in)) {
        int ch; while((ch=fgetc(in))!='\n' && ch!=EOF) {}
        return -1;
    }
    buf[strcspn(buf,"\r\n")]=0; return 1;
}
int settings_load(Settings *s,const char *path,int required)
{
    s->c.deviation=4e6; /* Fixed receiver default; old saved values no longer apply. */
    FILE *fp=fopen(path,"r");
    if(!fp) { if(errno==ENOENT && !required) return 1; perror(path); return 0; }
    char line[2048]; int rc,ok=1; unsigned n=0;
    while((rc=read_line(fp,line,sizeof(line)))!=0) {
        ++n;
        if(rc<0) { ok=0; break; }
        if(!*line || *line=='#') continue;
        char *eq=strchr(line,'=');
        if(!eq) { ok=0; break; }
        *eq++=0;
        if(!strcmp(line,"deviation")) continue; /* Legacy configuration. */
        const Field *f=field(line);
        if(!f || !assign(s,f,eq)) { ok=0; break; }
    }
    fclose(fp);
    /* Check after parsing: mode and rate may occur in either order. */
    if(ok && s->c.rate<settings_min_rate(s->c.mode)) ok=0;
    if(!ok) fprintf(stderr,"%s:%u: invalid configuration entry\n",path,n);
    return ok;
}
int settings_save(const Settings *s,const char *path)
{
    char tmp[4096];
    if(snprintf(tmp,sizeof(tmp),"%s.XXXXXX",path)>=(int)sizeof(tmp)) { errno=ENAMETOOLONG; return 0; }
    int fd=mkstemp(tmp); if(fd<0) return 0;
    FILE *fp=fdopen(fd,"w");
    if(!fp) { int err=errno; close(fd); unlink(tmp); errno=err; return 0; }
    int ok=fputs("# FM ATV startup configuration\n",fp)>=0;
    for(size_t i=0;i<sizeof(fields)/sizeof(*fields) && ok;++i) {
        char buf[1024]; const Field *f=&fields[i];
        const char *str=f->type=='s'?*(const char *const *)((const char *)s+f->offset):NULL;
        if(str && (strlen(str)>=sizeof(buf) || strchr(str,'\n') || strchr(str,'\r'))) { errno=EINVAL; ok=0; break; }
        value(s,f,buf,sizeof(buf));
        if(fprintf(fp,"%s=%s\n",f->key,buf)<0) ok=0;
    }
    if(fflush(fp) || fsync(fd)) ok=0;
    int err=errno;
    if(fclose(fp)) { ok=0; err=errno; }
    if(ok && atv_replace_file(tmp,path)) { ok=0; err=errno; }
    if(!ok) { unlink(tmp); errno=err?err:EIO; }
    return ok;
}
static int ask(Settings *s,SettingsPromptFn prompt,void *opaque,const char *key,const char *label,double min,double max)
{
    Field f=*field(key); if(f.type!='s') { f.min=min; f.max=max; }
    char current[1024],line[2048],details[256],error[256]="";
    value(s,&f,current,sizeof(current));
    if(f.type=='s') snprintf(details,sizeof(details),"Enter keeps the current value. '-' clears the field.");
    else snprintf(details,sizeof(details),"Enter %s between %.12g and %.12g.\nEnter keeps the current value.",f.type=='d'?"a number":"a whole number",min,max);
    for(;;) {
        SettingsPrompt q={label,current,details,error,0};
        if(!prompt(opaque,&q,line,sizeof(line))) return 0;
        if(assign(s,&f,*line?(!strcmp(line,"-") && f.type=='s'?"":line):current)) return 1;
        snprintf(error,sizeof(error),"Invalid value. %s",!strcmp(key,"alignment_bandwidth")?"Use 0 or at least 250000 Hz.":"Try again.");
    }
}
size_t settings_default_rx_block(const char *mode)
{
    /* Hardware-tested receive block sizes; keep B210 tuning responsive. */
    if(!strcmp(mode,"b210")) return 16384;
    return !strcmp(mode,"pluto")?131072:BLOCK;
}
static int ask_rx_block(Settings *s,SettingsPromptFn prompt,void *opaque)
{
    static const size_t sizes[]={2048,4096,8192,16384,32768,65536,131072,262144,524288,1048576};
    const size_t count=sizeof(sizes)/sizeof(*sizes);
    const char *hardware=!strcmp(s->c.mode,"b210")?"B210":!strcmp(s->c.mode,"hackrf")?"HackRF":!strcmp(s->c.mode,"sdrplay")?"SDRplay RSP1B":"PlutoSDR";
    size_t standard=settings_default_rx_block(s->c.mode);
    char details[2048],title[128],current[64]; size_t used=0;
    snprintf(current,sizeof(current),"%zu samples",s->c.rx_block);
    snprintf(title,sizeof(title),"Block size for %s",hardware);
    for(size_t i=0;i<count;++i)
        used+=(size_t)snprintf(details+used,sizeof(details)-used,"%2zu. %7zu samples (%.3f ms)%s%s\n",i+1,sizes[i],
            1000.0*sizes[i]/s->c.rate,sizes[i]==standard?" - default":"",sizes[i]==s->c.rx_block?" - current":"");
    snprintf(details+used,sizeof(details)-used,"Application default for %s: %zu samples.\nEnter = current value (%zu samples)\nAnalog bandwidth: %.3f MHz (automatic; %.3g MHz margin per side)",hardware,standard,s->c.rx_block,s->c.bandwidth/1e6,(s->c.rate-s->c.bandwidth)/2e6);
    const char *error="";
    for(;;) {
        char line[64]; SettingsPrompt q={title,current,details,error,0};
        if(!prompt(opaque,&q,line,sizeof(line))) return 0;
        if(!*line) return 1;
        if(strspn(line,"0123456789")==strlen(line)) {
            errno=0; unsigned long choice=strtoul(line,NULL,10);
            if(!errno && choice>=1 && choice<=count) { s->c.rx_block=sizes[choice-1]; return 1; }
        }
        error="Choose a number from the list (1..10) or press Enter.";
    }
}
static int ask_audio_output(Settings *s,SettingsPromptFn prompt,void *opaque,
                            const char *const *devices,int count)
{
    int page=0;
    const char *error="";
    for(;;) {
        char details[2048],line[64];
        size_t used=(size_t)snprintf(details,sizeof(details),"0. Windows default output\n");
        /* Six devices per page leave room for long names and help text. */
        for(int i=page;i<count && i<page+6;++i)
            used+=(size_t)snprintf(details+used,sizeof(details)-used,"%d. %.100s\n",i+1,devices[i]);
        snprintf(details+used,sizeof(details)-used,
            "%sEnter = keep current; 0 = default.%s\n"
            "FM 6 MHz / NICAM: sample rate AND video IF >= 14 MHz.\n"
            "7 MHz stereo: sample rate AND video IF >= 16 MHz.",
            count?"":"No playback devices found. Check Windows sound settings.\n",
            count>6?" N = next page; P = previous.":"");
        SettingsPrompt q={"Audio output (speakers / sound card)",
            s->c.audio_output && *s->c.audio_output?s->c.audio_output:"Windows default output",details,error,SETTINGS_VALUE};
        if(!prompt(opaque,&q,line,sizeof(line))) return 0;
        if(!*line) return 1;
        if(!strcmp(line,"n") || !strcmp(line,"N")) { if(page+6<count) page+=6; continue; }
        if(!strcmp(line,"p") || !strcmp(line,"P")) { if(page>=6) page-=6; continue; }
        if(strspn(line,"0123456789")==strlen(line)) {
            errno=0; unsigned long choice=strtoul(line,NULL,10);
            if(!errno && choice<=(unsigned long)count &&
               assign(s,field("audio_output"),choice?devices[choice-1]:"")) return 1;
        }
        error="Choose an output number from the list or press Enter.";
    }
}
const char *settings_hardware_rate_hint(unsigned index)
{
    static const char *hints[]={
        "up to 61.44 MS/s (USB 3)",
        "up to 20 MS/s",
        "approx. 6-8 MS/s (USB, observed)",
        "8 to <10 MS/s (app; untested)"
    };
    return index<4?hints[index]:"";
}
int settings_run_wizard_audio(Settings *s,SettingsPromptFn prompt,void *opaque,
                              const char *const *devices,int count)
{
    char line[64]; const char *error="";
    for(;;) {
        SettingsPrompt q={"Startup wizard","","Enter = skip and start.\nW + Enter = choose hardware and settings.",error,1};
        if(!prompt(opaque,&q,line,sizeof(line))) return 0;
        if(!*line || !strcmp(line,"s")) return 1;
        if(!strcmp(line,"w") || !strcmp(line,"W")) break;
        error="Press Enter (skip) or W + Enter.";
    }
    error="";
    for(;;) {
        char hardware_details[640];
        snprintf(hardware_details,sizeof(hardware_details),
            "1. B210 - %s\n2. HackRF - %s\n3. PlutoSDR - %s\n4. SDRplay RSP1B - %s\n"
            "Continuous RX guidance; actual rate depends on PC and connection.\n"
            "FM 6 MHz / NICAM needs >=14 MS/s AND >=14 MHz video IF.",
            settings_hardware_rate_hint(0),settings_hardware_rate_hint(1),
            settings_hardware_rate_hint(2),settings_hardware_rate_hint(3));
        SettingsPrompt q={"Which hardware are you using?","",hardware_details,error,SETTINGS_HARDWARE};
        if(!prompt(opaque,&q,line,sizeof(line))) return 0;
        if(strlen(line)==1 && *line>='1' && *line<='4') break;
        error="Choose 1, 2, 3 or 4 and press Enter.";
    }
    const char *mode=*line=='1'?"b210":*line=='2'?"hackrf":*line=='3'?"pluto":"sdrplay";
    int changed=strcmp(s->c.mode,mode)!=0;
    assign(s,field("mode"),mode);
    if(changed) { s->c.rx_block=settings_default_rx_block(mode); assign(s,field("args"),""); s->c.channel=0; assign(s,field("ref"),"internal"); }
    if(changed && !strcmp(mode,"sdrplay")) {
        s->c.rate=8e6; s->c.bandwidth=8e6; s->c.if_bandwidth=8e6;
    }
#define ASK(k,label,lo,hi) do { if(!ask(s,prompt,opaque,k,label,lo,hi)) return 0; } while(0)
    double previous_rate=s->c.rate;
    ASK("rate","Sample rate (samples/s)",settings_min_rate(mode),!strcmp(mode,"sdrplay")?9999999:!strcmp(mode,"hackrf")?20e6:!strcmp(mode,"b210")?61.44e6:64e6);
    /* Below 8 MS/s keep 500 kHz per side; otherwise keep 1 MHz.
     * SDRplay remains >= 8 MS/s, with analog BW below 8 MHz. */
    s->c.bandwidth=s->c.rate-(s->c.rate<8e6?1e6:2e6);
    if(!ask_rx_block(s,prompt,opaque)) return 0;
    /* A different receiver/rate needs a fresh, usable suggestion. Preserve
     * deliberate narrower filters when revisiting the same configuration. */
    if(changed || s->c.rate!=previous_rate || s->c.if_bandwidth>s->c.rate)
        s->c.if_bandwidth=s->c.bandwidth;
    ASK("if_bandwidth","Video IF bandwidth (Hz)",250000,s->c.rate);
    s->c.alignment_bandwidth=3200000;
    ASK("lo","Converter LO (Hz, 0 = no converter)",-1e12,1e12);
    double min_rf=!strcmp(mode,"sdrplay")?1000:1, max_rf=!strcmp(mode,"sdrplay")?2e9:6e9;
    if(s->c.frequency-s->c.lo<min_rf || s->c.frequency-s->c.lo>max_rf)
        ASK("frequency","Receive frequency including converter (Hz)",fmax(1,s->c.lo+min_rf),s->c.lo+max_rf);
    ASK("video_deemphasis","PAL de-emphasis (0 = off, 1 = on)",0,1);
    ASK("experimental_sync","Experimental sync (0 = off, 1 = on)",0,1);
    if(!strcmp(mode,"b210")) {
        ASK("args","UHD device selection (e.g. serial=...)",0,0);
        ASK("images","UHD images directory",0,0);
        ASK("channel","Receive channel",0,1);
        ASK("antenna","Antenna (TX/RX or RX2)",0,0);
        ASK("ref","Clock reference (internal, external, gpsdo)",0,0);
    } else if(!strcmp(mode,"sdrplay")) {
        ASK("args","Soapy device selection (empty = driver=sdrplay)",0,0);
        s->c.channel=0;
    } else if(!strcmp(mode,"hackrf")) {
        ASK("args","Soapy device selection (empty = driver=hackrf)",0,0);
        s->c.channel=0;
    } else {
        ASK("pluto_uri","Pluto URI (empty = automatic USB)",0,0);
        s->c.channel=0; assign(s,field("ref"),"internal");
    }
    if(count>=0 && !ask_audio_output(s,prompt,opaque,devices,count)) return 0;
    return 1;
#undef ASK
}
int settings_run_wizard(Settings *s,SettingsPromptFn prompt,void *opaque)
{
    return settings_run_wizard_audio(s,prompt,opaque,NULL,-1);
}
typedef struct { FILE *in,*out; } TerminalPrompt;
static int terminal_prompt(void *opaque,const SettingsPrompt *q,char *line,size_t n)
{
    TerminalPrompt *t=opaque;
    fprintf(t->out,"\n%s\n%s\n",q->title,q->details);
    for(;;) {
        if(*q->error) fprintf(t->out,"%s\n",q->error);
        fprintf(t->out,"Input [%s]: ",q->current); fflush(t->out);
        int rc=read_line(t->in,line,n);
        if(rc>0) return 1;
        if(!rc || ferror(t->in)) { if((q->kind==SETTINGS_WELCOME)) { *line=0; return 1; } return 0; }
        fprintf(t->out,"Input too long. Try again.\n");
    }
}
int settings_wizard(Settings *s,FILE *in,FILE *out)
{
    TerminalPrompt terminal={in,out};
    return settings_run_wizard(s,terminal_prompt,&terminal);
}
