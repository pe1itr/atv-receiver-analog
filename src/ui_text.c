#include "ui_text.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#ifndef _WIN32
#include <fontconfig/fontconfig.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    SDL_Rect source;
    int left, top;
} Glyph;
typedef struct {
    SDL_Texture *texture;
    Glyph glyph[256];
    int pixels, baseline;
    float density;
} Atlas;
typedef struct TextCache {
    SDL_Renderer *renderer;
    FT_Library library;
    FT_Face face;
    Atlas atlas[7];
    struct TextCache *next;
} TextCache;
static TextCache *caches;

static TextCache *find_cache(SDL_Renderer *renderer)
{
    for(TextCache *c=caches;c;c=c->next) if(c->renderer==renderer) return c;
    return NULL;
}
int ui_text_init(SDL_Renderer *renderer)
{
    if(find_cache(renderer)) return 1;
    TextCache *c=calloc(1,sizeof(*c));
    if(!c) return 0;
#ifdef _WIN32
    int ok=0;
    char *base=SDL_GetBasePath();
    char path[4096];
    if(!base) goto done;
    int n=snprintf(path,sizeof(path),"%sfont.ttf",base);
    SDL_free(base);
    if(n<0 || n>=(int)sizeof(path) || FT_Init_FreeType(&c->library) ||
       FT_New_Face(c->library,path,0,&c->face)) goto done;
#else
    FcPattern *request=FcNameParse((const FcChar8 *)"monospace:style=Regular");
    FcPattern *match=NULL; FcResult result;
    int ok=0;
    if(!request || FT_Init_FreeType(&c->library)) goto done;
    FcConfigSubstitute(NULL,request,FcMatchPattern); FcDefaultSubstitute(request);
    match=FcFontMatch(NULL,request,&result);
    FcChar8 *path=NULL; int index=0;
    if(!match || FcPatternGetString(match,FC_FILE,0,&path)!=FcResultMatch) goto done;
    FcPatternGetInteger(match,FC_INDEX,0,&index);
    if(FT_New_Face(c->library,(const char *)path,index,&c->face)) goto done;
#endif
    c->renderer=renderer; c->next=caches; caches=c; ok=1;
done:
#ifndef _WIN32
    if(match) FcPatternDestroy(match);
    if(request) FcPatternDestroy(request);
#endif
    if(!ok) {
        fprintf(stderr,"Cannot load monospace font\n");
        if(c->face) FT_Done_Face(c->face);
        if(c->library) FT_Done_FreeType(c->library);
        free(c);
    }
    return ok;
}
static int make_atlas(TextCache *c,Atlas *a,int pixels,float density)
{
    if(FT_Set_Pixel_Sizes(c->face,0,(FT_UInt)pixels)) return 0;
    int width=1,height=1;
    for(unsigned ch=32;ch<256;++ch) {
        if(FT_Load_Char(c->face,ch,FT_LOAD_RENDER)) continue;
        FT_GlyphSlot g=c->face->glyph;
        if((int)g->bitmap.width>width) width=(int)g->bitmap.width;
        if((int)g->bitmap.rows>height) height=(int)g->bitmap.rows;
    }
    width+=2; height+=2;
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,width*16,height*16,32,SDL_PIXELFORMAT_RGBA32);
    if(!surface) return 0;
    SDL_FillRect(surface,NULL,SDL_MapRGBA(surface->format,255,255,255,0));
    if(!FT_Load_Char(c->face,'H',FT_LOAD_RENDER)) a->baseline=c->face->glyph->bitmap_top;
    for(unsigned ch=32;ch<256;++ch) {
        if(FT_Load_Char(c->face,ch,FT_LOAD_RENDER)) continue;
        FT_GlyphSlot g=c->face->glyph;
        SDL_Rect rect={(int)(ch%16)*width+1,(int)(ch/16)*height+1,(int)g->bitmap.width,(int)g->bitmap.rows};
        a->glyph[ch]=(Glyph){rect,g->bitmap_left,g->bitmap_top};
        for(int y=0;y<rect.h;++y) {
            const unsigned char *row=g->bitmap.buffer+(g->bitmap.pitch>=0?y:rect.h-1-y)*abs(g->bitmap.pitch);
            Uint32 *out=(Uint32 *)((unsigned char *)surface->pixels+(rect.y+y)*surface->pitch)+rect.x;
            for(int x=0;x<rect.w;++x) {
                unsigned char alpha=g->bitmap.pixel_mode==FT_PIXEL_MODE_MONO?((row[x/8]&(128>>(x%8)))?255:0):row[x];
                out[x]=SDL_MapRGBA(surface->format,255,255,255,alpha);
            }
        }
    }
    SDL_Texture *texture=SDL_CreateTextureFromSurface(c->renderer,surface);
    SDL_FreeSurface(surface);
    if(!texture) return 0;
    SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture,SDL_ScaleModeLinear);
    if(a->texture) SDL_DestroyTexture(a->texture);
    a->texture=texture; a->pixels=pixels; a->density=density;
    return 1;
}
/* Existing UI positions use fixed-width cells. Rasterize at the output scale,
 * then place glyphs in those cells; resizing never magnifies the old bitmap. */
void ui_text_draw(SDL_Renderer *renderer,int x,int y,const char *text,int scale)
{
    if(scale<1 || scale>6 || !ui_text_init(renderer)) return;
    TextCache *c=find_cache(renderer);
    float sx,sy; SDL_RenderGetScale(renderer,&sx,&sy);
    float density=fmaxf(1,fmaxf(sx,sy));
    int pixels=(int)lroundf(7*scale*density);
    Atlas *a=&c->atlas[scale];
    if(!a->texture || a->pixels!=pixels || fabsf(a->density-density)>.01f)
        if(!make_atlas(c,a,pixels,density)) return;
    Uint8 red,green,blue,alpha;
    SDL_GetRenderDrawColor(renderer,&red,&green,&blue,&alpha);
    SDL_SetTextureColorMod(a->texture,red,green,blue); SDL_SetTextureAlphaMod(a->texture,alpha);
    const unsigned char *p=(const unsigned char *)text;
    for(;*p;x+=4*scale) {
        unsigned ch=*p++;
        if(ch>=0xc2 && ch<=0xc3 && (*p&0xc0)==0x80) ch=((ch&31)<<6)|(*p++&63);
        else if(ch>=128) { while((*p&0xc0)==0x80) ++p; ch='?'; }
        if(ch<32) ch='?';
        Glyph *g=&a->glyph[ch];
        if(!g->source.w || !g->source.h) continue;
        SDL_FRect dest={x+g->left/density,y+(a->baseline-g->top)/density,
            g->source.w/density,g->source.h/density};
        SDL_RenderCopyF(renderer,a->texture,&g->source,&dest);
    }
}
void ui_text_destroy(SDL_Renderer *renderer)
{
    TextCache **link=&caches;
    while(*link && (*link)->renderer!=renderer) link=&(*link)->next;
    if(!*link) return;
    TextCache *c=*link; *link=c->next;
    for(size_t i=0;i<sizeof(c->atlas)/sizeof(*c->atlas);++i)
        if(c->atlas[i].texture) SDL_DestroyTexture(c->atlas[i].texture);
    FT_Done_Face(c->face); FT_Done_FreeType(c->library); free(c);
}
