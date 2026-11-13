#include "light_stack.h"
#include <string.h>
void light_stack_reset(LightStack *s)
{
    s->count=s->next=0;
}
void light_stack_add(LightStack *s,const uint32_t *pixels,uint32_t *output)
{
    int first=s->count==0,full=s->count==LIGHT_STACK_FRAMES;
    if(s->count<LIGHT_STACK_FRAMES) ++s->count;
    for(size_t j=0;j<WIDTH*HEIGHT;++j) {
        uint32_t old=full?s->history[s->next][j]:0,new=pixels[j];
        s->history[s->next][j]=new;
        unsigned red=(first?0:s->sum[j][0])+((new>>16)&255)-((old>>16)&255);
        unsigned green=(first?0:s->sum[j][1])+((new>>8)&255)-((old>>8)&255);
        unsigned blue=(first?0:s->sum[j][2])+(new&255)-(old&255);
        s->sum[j][0]=red; s->sum[j][1]=green; s->sum[j][2]=blue;
        /* Running sums keep the cost independent of the window size. The
         * steady-state divisor is constant so the compiler can optimize it. */
        unsigned count=full?LIGHT_STACK_FRAMES:s->count;
        output[j]=0xff000000u|((red+count/2)/count<<16)|
            ((green+count/2)/count<<8)|((blue+count/2)/count);
    }
    s->next=(s->next+1)%LIGHT_STACK_FRAMES;
}
