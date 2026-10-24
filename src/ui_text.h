#ifndef UI_TEXT_H
#define UI_TEXT_H
#include <SDL.h>
int ui_text_init(SDL_Renderer *renderer);
void ui_text_draw(SDL_Renderer *renderer,int x,int y,const char *text,int scale);
/* Release cached glyph textures before destroying the SDL renderer. */
void ui_text_destroy(SDL_Renderer *renderer);
#endif
