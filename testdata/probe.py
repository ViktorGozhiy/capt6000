import struct, subprocess, sys, os, re
def raster_dims(ppd):
    out = subprocess.run(["cupsfilter","-m","application/vnd.cups-raster","-p",ppd,"testpage.pdf"],
                         capture_output=True)
    d = out.stdout
    if len(d) < 2000: return None
    off = 4 + 64*4
    vals = struct.unpack_from('<'+'I'*100, d, off)
    # order after the 4 strings:
    names = ["AdvanceDistance","AdvanceMedia","Collate","CutMedia","Duplex","HWRes0","HWRes1",
             "bb0","bb1","bb2","bb3","InsertSheet","Jog","LeadingEdge","Margin0","Margin1",
             "ManualFeed","MediaPosition","MediaWeight","MirrorPrint","NegativePrint","NumCopies",
             "Orientation","OutputFaceUp","PageSizeW","PageSizeH","Separations","TraySwitch","Tumble",
             "cupsWidth","cupsHeight","cupsMediaType","cupsBitsPerColor","cupsBitsPerPixel",
             "cupsBytesPerLine"]
    h = dict(zip(names, vals))
    return h["cupsWidth"], h["cupsHeight"], h["cupsBytesPerLine"]

base = open("../ppd/Canon-LBP6000.ppd").read()
for dw, dh in [(0,0), (-0.12,-0.24), (-0.24,-0.36), (-0.06,-0.12)]:
    right = 582.72 + dw
    top   = 827.28 + dh
    mod = base.replace('*ImageableArea A4/A4: "14.4 14.16 582.72 827.28"',
                       '*ImageableArea A4/A4: "14.4 14.16 %g %g"' % (right, top))
    assert mod != base or (dw==0 and dh==0)
    open("/tmp/probe.ppd","w").write(mod)
    print(f"right={right:.2f} top={top:.2f} -> {raster_dims('/tmp/probe.ppd')}")
