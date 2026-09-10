#!/usr/bin/env python3
"""Decode every original MTH frame using the exact Vita THP unpacker and host TurboJPEG."""
import argparse, ctypes as c, hashlib, json, struct, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--movie',type=Path,default=Path('orig/GALE01/files/MvOpen.mth'))
p.add_argument('--output',type=Path,default=Path('build/vita/host/movie-check.json'))
a=p.parse_args();a.output.parent.mkdir(parents=True,exist_ok=True)
lib=a.output.parent/'libthp_check.dylib'
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2','-dynamiclib','vita/thp_jpeg.c','-o',str(lib)],check=True)
u=c.CDLL(str(lib));u.mv_thp_jpeg_unpack.argtypes=[c.c_void_p,c.c_size_t,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
t=c.CDLL('/opt/homebrew/lib/libturbojpeg.dylib')
t.tjInitDecompress.restype=c.c_void_p
t.tjDecompressHeader3.argtypes=[c.c_void_p,c.c_void_p,c.c_ulong,*([c.POINTER(c.c_int)]*4)]
t.tjDecompress2.argtypes=[c.c_void_p,c.c_void_p,c.c_ulong,c.c_void_p,c.c_int,c.c_int,c.c_int,c.c_int,c.c_int]
t.tjGetErrorStr2.argtypes=[c.c_void_p];t.tjGetErrorStr2.restype=c.c_char_p
t.tjDestroy.argtypes=[c.c_void_p]
h=t.tjInitDecompress();assert h
hashes={};max_jpeg=0
try:
 with a.movie.open('rb') as f:
  header=f.read(64);magic,_,version,maximum,w,ht,fps,count,offset,_,size,*_=struct.unpack('>16I',header)
  assert (magic,version,w,ht,fps)==(0x4d544850,2,640,480,30)
  dst=c.create_string_buffer((maximum+32)*2);pixels=c.create_string_buffer(w*ht*4)
  for i in range(count):
   assert 8<=size<=maximum+32, (i,size,maximum)
   f.seek(offset);raw=f.read(size);assert len(raw)==size
   payload=raw[4:];written=c.c_size_t()
   r=u.mv_thp_jpeg_unpack(payload,len(payload),dst,len(dst),c.byref(written))
   assert r==0,(i,r)
   width,height,sub,color=[c.c_int() for _ in range(4)]
   assert t.tjDecompressHeader3(h,dst,written.value,c.byref(width),c.byref(height),c.byref(sub),c.byref(color))==0
   assert (width.value,height.value)==(w,ht)
   r=t.tjDecompress2(h,dst,written.value,pixels,w,w*4,ht,7,2048)
   assert r==0,(i,t.tjGetErrorStr2(h))
   if i in (0,1,100,count//2,count-1):hashes[str(i)]=hashlib.sha256(pixels.raw).hexdigest()
   max_jpeg=max(max_jpeg,written.value)
   if i==0:
    # Rejection must not write output when capacity is insufficient.
    tiny=c.create_string_buffer(b'KEEP',4);n=c.c_size_t(99)
    assert u.mv_thp_jpeg_unpack(payload,len(payload),tiny,4,c.byref(n))==-4
    assert tiny.raw==b'KEEP' and n.value==0
    for bad in (b'',b'not JPEG',payload[:100],payload[:payload.rfind(b'\xff\xd9')]):
     assert u.mv_thp_jpeg_unpack(bad,len(bad),dst,len(dst),c.byref(n))!=0
   offset+=size;size=struct.unpack_from('>I',raw)[0]
   if (i+1)%500==0: print(f'Movie frames decoded: {i+1}/{count}',flush=True)
finally:t.tjDestroy(h)
report={'frames_decoded':count,'width':w,'height':ht,'fps':fps,'max_jpeg_bytes':max_jpeg,'rgba_sha256_samples':hashes,'scope':'CPU decode; Vita presentation/timing/audio unverified'}
a.output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
