# Generates the notification cow (android/app/src/main/res/drawable/ic_stat_cow.xml + cow.svg) - the first design.
# The later "smile" design is kept as ic_stat_cow_smile.xml.
import math, sys
def f(v): return ('%.2f' % v).rstrip('0').rstrip('.')
def ell(cx, cy, rx, ry, rot=0):
    # closed ellipse as two arcs (rotation in degrees)
    t = math.radians(rot)
    x1, y1 = cx + rx*math.cos(t), cy + rx*math.sin(t)
    x2, y2 = cx - rx*math.cos(t), cy - rx*math.sin(t)
    return f"M{f(x1)},{f(y1)} A{f(rx)},{f(ry)} {f(rot)} 1,1 {f(x2)},{f(y2)} A{f(rx)},{f(ry)} {f(rot)} 1,1 {f(x1)},{f(y1)} Z"
P = dict(hx=12, hy=10.4, hrx=6.5, hry=6.1, mx=12, my=17.3, mrx=6.1, mry=4.1, gap=0.95,
         ex=3.05, ey=10.0, erx=1.4, ery=1.85)
P.update({k: float(v) for k, v in (a.split('=') for a in sys.argv[1:])})
hx,hy,hrx,hry = P['hx'],P['hy'],P['hrx'],P['hry']
mx,my,mrx,mry = P['mx'],P['my'],P['mrx'],P['mry']
grx, gry = mrx + P['gap'], mry + P['gap']
# where the head ellipse meets the gap ellipse (left side; right is mirrored)
def head_y(x): return hy + hry*math.sqrt(max(0, 1-((x-hx)/hrx)**2))  # lower half
def gap_y(x):  return my - gry*math.sqrt(max(0, 1-((x-mx)/grx)**2))  # upper half
lo, hi = hx - min(hrx, grx) + 1e-6, hx
for _ in range(80):
    mid = (lo+hi)/2
    if head_y(mid) - gap_y(mid) > 0: hi = mid
    else: lo = mid
xl = (lo+hi)/2; yl = head_y(xl); xr = 2*hx - xl
head = (f"M{f(xl)},{f(yl)} A{f(hrx)},{f(hry)} 0 1,1 {f(xr)},{f(yl)} "
        f"A{f(grx)},{f(gry)} 0 0,0 {f(xl)},{f(yl)} Z ")
ex, ey, erx, ery = P['ex'], P['ey'], P['erx'], P['ery']
eyes = ell(hx-ex, ey, erx, ery) + " " + ell(hx+ex, ey, erx, ery)
# a small heart on the forehead (cut out)
s = 0.95; cx, cy = 12, 6.55
heart = (f"M{f(cx)},{f(cy+1.55*s)} C{f(cx-0.6*s)},{f(cy+1.0*s)} {f(cx-1.6*s)},{f(cy+0.35*s)} {f(cx-1.6*s)},{f(cy-0.35*s)} "
         f"C{f(cx-1.6*s)},{f(cy-1.05*s)} {f(cx-0.55*s)},{f(cy-1.3*s)} {f(cx)},{f(cy-0.6*s)} "
         f"C{f(cx+0.55*s)},{f(cy-1.3*s)} {f(cx+1.6*s)},{f(cy-1.05*s)} {f(cx+1.6*s)},{f(cy-0.35*s)} "
         f"C{f(cx+1.6*s)},{f(cy+0.35*s)} {f(cx+0.6*s)},{f(cy+1.0*s)} {f(cx)},{f(cy+1.55*s)} Z")
headPath = head + eyes + " " + heart
spark = ell(hx-ex+0.45, ey-0.7, 0.5, 0.5) + " " + ell(hx+ex+0.45, ey-0.7, 0.5, 0.5) + " " + \
        ell(hx-ex-0.42, ey+0.75, 0.26, 0.26) + " " + ell(hx+ex-0.42, ey+0.75, 0.26, 0.26)
muzzle = ell(mx, my, mrx, mry) + " " + ell(mx-2.25, my+0.15, 0.78, 1.05) + " " + ell(mx+2.25, my+0.15, 0.78, 1.05)
ears = ell(4.35, 9.0, 2.55, 1.3, 22) + " " + ell(19.65, 9.0, 2.55, 1.3, -22)
horns = ("M7.6,5.6 Q6.3,4.6 6.55,2.3 Q8.6,3.2 9.5,4.75 Z "
         "M16.4,5.6 Q17.7,4.6 17.45,2.3 Q15.4,3.2 14.5,4.75 Z")
paths = [("horns", horns, "nonZero"), ("ears", ears, "nonZero"), ("head", headPath, "evenOdd"),
         ("sparkle", spark, "nonZero"), ("muzzle", muzzle, "evenOdd")]
if __name__ == "__main__":
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24">']
    for n, d, r in paths:
        svg.append(f'<path fill="#fff" fill-rule="{"evenodd" if r=="evenOdd" else "nonzero"}" d="{d}"/>')
    svg.append('</svg>')
    open('cow.svg','w').write('\n'.join(svg))
    xml = ['<?xml version="1.0" encoding="utf-8"?>',
           '<!-- LunaShare notification icon: a little cow (status bar: white on transparent, Android tints it) -->',
           '<vector xmlns:android="http://schemas.android.com/apk/res/android"',
           '    android:width="24dp" android:height="24dp" android:viewportWidth="24" android:viewportHeight="24">']
    for n, d, r in paths:
        xml.append(f'    <!-- {n} -->')
        xml.append(f'    <path android:fillColor="#FFFFFFFF" android:fillType="{r}" android:pathData="{d}"/>')
    xml.append('</vector>')
    open('ic_stat_cow.xml','w').write('\n'.join(xml) + '\n')
