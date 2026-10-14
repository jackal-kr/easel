#!/usr/bin/env python3
"""Generates the symbolic tool icons (data/icons/scalable/actions).

Everything is drawn as filled shapes (no strokes) so GTK can recolour the
icons for light and dark themes. Run:  OUT=data/icons/scalable/actions tools/gen-icons.py
"""
import math, os
FG="#2e3436"
def f(v): return f"{v:.2f}".rstrip('0').rstrip('.')
def line(x0,y0,x1,y1,w=1.4,cap=True):
    dx,dy=x1-x0,y1-y0; L=math.hypot(dx,dy); nx,ny=-dy/L*w/2,dx/L*w/2
    ex,ey=(dx/L*w/2,dy/L*w/2) if cap else (0,0)
    pts=[(x0-ex+nx,y0-ey+ny),(x1+ex+nx,y1+ey+ny),(x1+ex-nx,y1+ey-ny),(x0-ex-nx,y0-ey-ny)]
    return poly(pts)
def poly(pts,extra=""):
    return f'<path d="M{" L".join(f(x)+" "+f(y) for x,y in pts)} Z"{extra}/>'
def circle(cx,cy,r): return f'<circle cx="{f(cx)}" cy="{f(cy)}" r="{f(r)}"/>'
def ring(cx,cy,r,w):
    return f'<path fill-rule="evenodd" d="M{f(cx-r)} {f(cy)}a{f(r)} {f(r)} 0 1 0 {f(2*r)} 0a{f(r)} {f(r)} 0 1 0 {f(-2*r)} 0ZM{f(cx-r+w)} {f(cy)}a{f(r-w)} {f(r-w)} 0 1 1 {f(2*(r-w))} 0a{f(r-w)} {f(r-w)} 0 1 1 {f(-2*(r-w))} 0Z"/>'
def rrect_ring(x,y,w,h,t,r=0):
    def rr(x,y,w,h,r):
        if r<=0: return f"M{f(x)} {f(y)}h{f(w)}v{f(h)}h{f(-w)}Z"
        return (f"M{f(x+r)} {f(y)}h{f(w-2*r)}a{f(r)} {f(r)} 0 0 1 {f(r)} {f(r)}v{f(h-2*r)}a{f(r)} {f(r)} 0 0 1 {f(-r)} {f(r)}"
                f"h{f(-(w-2*r))}a{f(r)} {f(r)} 0 0 1 {f(-r)} {f(-r)}v{f(-(h-2*r))}a{f(r)} {f(r)} 0 0 1 {f(r)} {f(-r)}Z")
    return f'<path fill-rule="evenodd" d="{rr(x,y,w,h,r)}{rr(x+t,y+t,w-2*t,h-2*t,max(0,r-t))}"/>'
def ellipse_ring(cx,cy,rx,ry,t):
    def e(rx,ry): return f"M{f(cx-rx)} {f(cy)}a{f(rx)} {f(ry)} 0 1 0 {f(2*rx)} 0a{f(rx)} {f(ry)} 0 1 0 {f(-2*rx)} 0Z"
    return f'<path fill-rule="evenodd" d="{e(rx,ry)}{e(rx-t,ry-t)}"/>'
def rot(pts,ang,cx,cy):
    a=math.radians(ang); c,s=math.cos(a),math.sin(a)
    return [(cx+(x-cx)*c-(y-cy)*s, cy+(x-cx)*s+(y-cy)*c) for x,y in pts]
def rect_pts(x,y,w,h): return [(x,y),(x+w,y),(x+w,y+h),(x,y+h)]
def dashed_poly(pts,dash=2.0,gap=1.4,w=1.3,closed=True):
    out=[]; seq=pts+([pts[0]] if closed else [])
    on=True; remain=dash
    for (x0,y0),(x1,y1) in zip(seq,seq[1:]):
        L=math.hypot(x1-x0,y1-y0); d=0
        while d<L:
            step=min(remain,L-d)
            if on:
                ax,ay=x0+(x1-x0)*d/L,y0+(y1-y0)*d/L
                bx,by=x0+(x1-x0)*(d+step)/L,y0+(y1-y0)*(d+step)/L
                out.append(line(ax,ay,bx,by,w,cap=False))
            d+=step; remain-=step
            if remain<=1e-6:
                on=not on; remain=dash if on else gap
    return out
def bez(p0,p1,p2,p3,n=24):
    pts=[]
    for i in range(n+1):
        t=i/n; u=1-t
        pts.append((u**3*p0[0]+3*u*u*t*p1[0]+3*u*t*t*p2[0]+t**3*p3[0],u**3*p0[1]+3*u*u*t*p1[1]+3*u*t*t*p2[1]+t**3*p3[1]))
    return pts
def stroke_path(pts,w=1.4):
    return [line(a[0],a[1],b[0],b[1],w) for a,b in zip(pts,pts[1:])]+[circle(p[0],p[1],w/2) for p in pts]
def side_dashes(x0,y0,x1,y1,n,w=1.4):
    L=math.hypot(x1-x0,y1-y0); d=(L-(n-1)*1.5)/n; out=[]
    for i in range(n):
        a=i*(d+1.5); b=a+d
        out.append(line(x0+(x1-x0)*a/L,y0+(y1-y0)*a/L,x0+(x1-x0)*b/L,y0+(y1-y0)*b/L,w,cap=False))
    return out

icons={}
icons["pan"]=['<rect x="4.6" y="7" width="7.6" height="7.4" rx="3.2"/>',
 '<rect x="4.9" y="2.4" width="1.8" height="7.6" rx=".9"/>','<rect x="7" y="1.2" width="1.8" height="8.6" rx=".9"/>',
 '<rect x="9.1" y="1.8" width="1.8" height="8" rx=".9"/>','<rect x="11.2" y="3.4" width="1.7" height="6.4" rx=".85"/>',
 '<rect x="2.2" y="7.4" width="1.8" height="5" rx=".9" transform="rotate(-30 3.1 9.9)"/>']
icons["magnifier"]=[ring(6.6,6.6,5,1.6), line(10.4,10.4,14.4,14.4,2.2)]
icons["select"]=(side_dashes(2,3.7,14,3.7,3)+side_dashes(14.3,3,14.3,13,3)+side_dashes(14,12.3,2,12.3,3)+side_dashes(1.7,13,1.7,3,3))
loop=[(3,6),(6,2.5),(10.5,3),(14,6.5),(12.5,11),(8,13.5),(4,11.5)]
icons["free-form-select"]=dashed_poly(loop,2.1,1.5,1.4)
blk=rot(rect_pts(2.5,5.5,11,5.5),-40,8,8.2)
icons["eraser"]=[poly(blk,' fill-opacity=".35"'),poly(rot(rect_pts(2.5,8.2,11,2.8),-40,8,8.2)),line(1.5,14,14.5,14,1.2)]
body=rot(rect_pts(7,2.2,2.6,7),45,8.3,8)
tip=rot([(7,9.2),(9.6,9.2),(8.8,13.6),(7.8,13.6)],45,8.3,8)
icons["pick-color"]=[poly(body),poly(tip),circle(11.9,4.1,2.2)]
body=rot([(3,7.6),(11,7.6),(9.8,13.8),(4.2,13.8)],-25,7,10)
rim=rot(rect_pts(2.6,5.6,8.8,1.4),-25,7,10)
handle=stroke_path(rot(bez((3.2,5.4),(3.6,1.6),(10,1.6),(10.6,5.0),n=16),-25,7,10),1.1)
icons["fill"]=[poly(body),poly(rim)]+handle+['<path d="M13.6 8.2c.9 1.3 1.6 2.2 1.6 3.1a1.6 1.6 0 0 1-3.2 0c0-.9.7-1.8 1.6-3.1z"/>']
icons["gradient"]=[f'<rect x="{1.5+i*2.2:.1f}" y="2.5" width="2.2" height="11" fill-opacity="{o}"/>' for i,o in enumerate([.12,.28,.44,.6,.8,1])]
icons["pencil"]=[poly(rot(rect_pts(5.6,1,2.8,9),-45,8,8)),poly(rot([(5.6,10),(8.4,10),(7,13.2)],-45,8,8)),poly(rot(rect_pts(5.6,0,2.8,1.4),-45,8,8),' fill-opacity=".5"')]
icons["brush"]=[line(13.6,2.4,7.2,8.8,1.6),line(7.6,8.4,6.0,10.0,2.4),
  '<path d="M5.6 10.2c-1.9 0-3.4 1.2-3.4 3.2 0 .6.3.8.7.8 1.6 0 2.5-.3 3.4-1.1.6-.6 1-1.2 1-1.9z"/>']
icons["airbrush"]=['<rect x="2" y="6.5" width="5" height="7.5" rx="1"/>','<rect x="3.2" y="4.2" width="2.6" height="2"/>',
  circle(9.6,5.2,.9),circle(12.4,3.4,.9),circle(12.6,6.8,.9),circle(9.8,8.6,.9),circle(14,10,.7),circle(11.6,10.6,.7),circle(12.8,12.8,.7)]
icons["text"]=['<path fill-rule="evenodd" d="M1 13.5L5.2 2.5h1.7L11.1 13.5H9.2l-.9-2.5H3.9l-.9 2.5zM4.5 9.4h3.1L6.05 4.8z"/>','<rect x="13" y="3.5" width="1.2" height="9"/>','<rect x="11.8" y="2.5" width="3.6" height="1.1"/>','<rect x="11.8" y="12.4" width="3.6" height="1.1"/>']
icons["line"]=[line(3,13,13,3,1.6),circle(3,13,1.7),circle(13,3,1.7)]
icons["curve"]=stroke_path(bez((2.5,12.5),(2.5,2),(13.5,14),(13.5,3.5),n=40),1.5)
icons["rectangle"]=[rrect_ring(1.5,3,13,10,1.6)]
icons["rounded-rectangle"]=[rrect_ring(1.5,3,13,10,1.6,3.4)]
icons["ellipse"]=[ellipse_ring(8,8,6.6,5,1.6)]
pg=[(2.5,6),(8,2),(13.5,5.5),(11.5,13),(4.5,12.5)]
icons["polygon"]=[line(*pg[i],*pg[(i+1)%5],1.4,cap=False) for i in range(5)]+[circle(x,y,1.1) for x,y in pg]

OUT=os.environ.get("OUT","out")
os.makedirs(OUT,exist_ok=True)
for name,els in icons.items():
    body="\n  ".join(els)
    open(f"{OUT}/easel-{name}-symbolic.svg","w").write(f'<?xml version="1.0" encoding="UTF-8"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">\n <g fill="{FG}">\n  {body}\n </g>\n</svg>\n')
