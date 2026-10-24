#!/usr/bin/env python3
"""Compare the actual receiver's live/rolling-stack traces with known TX lettering.
A translation-tolerant normalized correlation after 4x4 averaging is a proxy
for retained picture structure, not a calibrated SNR or OCR score. Row means
are removed so horizontal sync bands cannot dominate the measurement.
"""
import argparse
import csv
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('directory',type=Path)
p.add_argument('--partial', action='store_true')
a=p.parse_args();root=a.directory

def reduce(image):
    z=np.asarray(image,dtype=np.float64).reshape(72,4,160,4).mean(axis=(1,3))
    return z-z.mean(axis=1,keepdims=True)
template=root/'template.pgm'
if not template.exists(): template=next(root.glob('*-template.pgm'))
ref=reduce(Image.open(template));rf=np.fft.rfft2(ref)
refnorm=np.sqrt(np.sum(ref*ref))
summaries={};records={}
for name in ('fm0-8','fm1-8','fm0-6','fm1-6'):
    path=root/f'{name}.csv'
    if not path.exists(): continue
    with path.open() as f: rows=list(csv.DictReader(f))
    available=(root/f'{name}.gray').stat().st_size//(2*288*640)
    n=min(len(rows),available)
    if not n: continue
    frames=np.memmap(root/f'{name}.gray',dtype=np.uint8,mode='r',shape=(n,2,288,640))
    scores=np.zeros((n,2));shifts=np.zeros((n,2,2),dtype=int)
    for i in range(n):
        for kind in range(2):
            z=reduce(frames[i,kind]);den=np.sqrt(np.sum(z*z))*refnorm
            corr=np.fft.irfft2(np.fft.rfft2(z)*rf.conj(),s=z.shape)
            idx=np.unravel_index(np.argmax(corr),corr.shape)
            scores[i,kind]=corr[idx]/den if den else 0
            shifts[i,kind]=idx
    times=np.array([float(r['time']) for r in rows[:n]])
    counts=np.array([int(r['count']) for r in rows[:n]])
    accepted=np.array([int(r['accepted']) for r in rows[:n]])
    total=sum(max(0,int(x)-int(y)) if x>=y else int(x) for x,y in zip(accepted,np.r_[0,accepted[:-1]]))
    summary={'fields':n,'last_time':float(times[-1]),'accepted_across_resets':total,
             'max_stack':int(counts.max()),'final_stack':int(counts[-1])}
    for k,kind in enumerate(('live','stack')):
        valid=(counts>0) if k else np.ones(n,dtype=bool)
        v=np.where(valid,scores[:,k],0);best=int(np.argmax(v))
        summary[kind]={'best_correlation':float(v[best]),'best_time':float(times[best]),
                       'fields_above_0_3':int(np.sum(v>.3)),
                       'fields_above_0_5':int(np.sum(v>.5)),
                       'final_correlation':float(v[-1])}
        Image.fromarray(np.asarray(frames[best,k])).save(root/f'{name}-{kind}-best.png')
    np.savez(root/f'{name}-metrics.npz',time=times,score=scores,count=counts,shifts=shifts)
    summaries[name]=summary;records[name]=(frames,times,scores,counts)
    print(name,json.dumps(summary),flush=True)
(root/'summary.json').write_text(json.dumps(summaries,indent=2)+'\n')
# Same times for both detectors, selected by max(FM0, FM1) live structure in
# each five-second interval. Selection is symmetric and shows the whole pass.
for bw in (8,6):
    names=[f'fm{m}-{bw}' for m in (0,1)]
    if any(name not in records for name in names):continue
    panel=Image.new('RGB',(1280,8*180),(18,22,28));draw=ImageDraw.Draw(panel)
    for interval in range(8):
        candidates=[]
        for name in names:
            _,t,s,_=records[name];ii=np.where((t>=5*interval)&(t<5*(interval+1)))[0]
            if len(ii):
                idx=ii[np.argmax(s[ii,0])];candidates.append((s[idx,0],t[idx]))
        if not candidates:continue
        time=max(candidates)[1]
        for m,name in enumerate(names):
            frames,t,s,c=records[name];i=int(np.argmin(abs(t-time)))
            for k,kind in enumerate(('live','stack')):
                x=m*640+k*320;y=interval*180
                draw.text((x+5,y+4),f'{name} {kind} t={t[i]:.2f}s r={s[i,k]:.2f} n={c[i]}',fill='white')
                panel.paste(Image.fromarray(np.asarray(frames[i,k])).resize((320,144)),(x,y+25))
    panel.save(root/f'timeline-{bw}MHz.png')

# A common-time close-up of the second useful burst, no per-detector selection.
panel=Image.new('RGB',(1280,4*320),(18,22,28));draw=ImageDraw.Draw(panel)
for row,(bw,kind,k) in enumerate(((8,'live',0),(8,'stack',1),(6,'live',0),(6,'stack',1))):
    for mode in (0,1):
        name=f'fm{mode}-{bw}'
        if name not in records:continue
        frames,t,s,c=records[name];i=int(np.argmin(abs(t-21.0)))
        draw.text((mode*640+8,row*320+7),f'FM{mode} / {bw} MHz / {kind} / t={t[i]:.3f}s / r={s[i,k]:.3f} / stack {c[i]}',fill='white')
        panel.paste(Image.fromarray(np.asarray(frames[i,k])),(mode*640,row*320+28))
panel.save(root/'burst-21s.png')

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
fig,axes=plt.subplots(2,2,figsize=(12,7),sharex=True,sharey='row')
for col,bw in enumerate((8,6)):
    for mode in (0,1):
        name=f'fm{mode}-{bw}'
        if name not in records:continue
        _,t,s,c=records[name]
        for k in (0,1):
            v=s[:,k].copy()
            if k: v[c==0]=float('nan')
            axes[k,col].plot(t,v,label=f'FM{mode}',lw=.8,alpha=.85)
    for k in (0,1):
        axes[k,col].set_title(f'{bw} MHz — '+('livevelden' if k==0 else 'rolling stack 25'))
        axes[k,col].grid(alpha=.25);axes[k,col].legend()
        axes[k,col].set_xlim(0,40);axes[k,col].set_ylim(0,.45 if k==0 else 1)
        axes[k,col].set_ylabel('Correlatie met uitgezonden testkaart')
    axes[1,col].set_xlabel('Seconden vanaf begin IQ-bestand')
fig.suptitle('Scatter FM0/FM1: alle velden, dezelfde ontvanger en uitlijn-IF van 3,2 MHz')
fig.tight_layout();fig.savefig(root/'quality-over-time.png',dpi=150)
