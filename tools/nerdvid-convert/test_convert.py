#!/usr/bin/env python3
"""Tests for nerdvid_convert.py using synthetic NERDVID files (old v1 without Phase 2C fields, 30 fps, truncated, corrupt)."""
import io, os, struct, subprocess, sys, tempfile, shutil, contextlib
sys.path.insert(0, os.path.dirname(__file__))
import nerdvid_convert as nv

def make(path, fps, seconds, audio=True, drops=(), complete=True, truncate=0, p2c=True, corrupt_at=None):
    frames, index = [], []
    out = io.BytesIO()
    out.write(b"\0" * 64)
    apos = 0
    step = 1000.0 / fps
    n = int(fps * seconds)
    stored = 0
    for k in range(n):
        if k in drops:
            continue
        t = int(k * step)
        while audio and apos // 32 <= t:
            part = bytes(8192)
            out.write(nv.CHUNK.pack(b"AUDI", len(part), apos // 32, 0) + part)
            apos += len(part)
        index.append((out.tell(), t))
        out.write(nv.CHUNK.pack(b"VFRM", nv.FRAME_BYTES, t, 0) + bytes([k & 255, 0x80]) * (nv.FRAME_BYTES // 2))
        stored += 1
    idx = out.tell()
    if complete:
        for off, t in index:
            out.write(struct.pack("<II", off, t))
    flags = (nv.FLAG_COMPLETE if complete else 0) | (nv.FLAG_HAS_AUDIO if audio else 0) | (nv.FLAG_DROPPED if drops else 0)
    hdr = nv.HEADER.pack(nv.MAGIC, 1, 64, 256, 192, fps, 1, stored, len(drops), int(seconds * 1000), apos, 1 if audio else 0,
                         16000 if audio else 0, 1 if audio else 0, 1, idx if complete else 0, stored if complete else 0, flags, 0,
                         70 if p2c else 0, n if p2c else 0)
    data = bytearray(out.getvalue())
    data[:64] = hdr
    if truncate:
        data = data[:-truncate]
    if corrupt_at is not None:
        data[corrupt_at:corrupt_at + 4] = b"XXXX"
    open(path, "wb").write(bytes(data))


def make_aligned(path, fps, seconds):
    """Phase 2C.1 layout: header block + PAD to 512, video slots = PAD + VFRM, audio blocks padded to a sector."""
    out = io.BytesIO(); out.write(b"\0" * 64)
    out.write(nv.CHUNK.pack(b"PAD ", 512 - 64 - 16, 0, 0) + bytes(512 - 64 - 16))
    index, apos, stored = [], 0, 0
    n = int(fps * seconds)
    for k in range(n):
        t = int(k * 1000 / fps)
        if k % 4 == 3:
            part = bytes(4000)
            used = 16 + len(part) + 16
            total = (out.tell() + used + 511) // 512 * 512 - out.tell()
            out.write(nv.CHUNK.pack(b"AUDI", len(part), apos // 32, 0) + part + nv.CHUNK.pack(b"PAD ", total - used, 0, 0) + bytes(total - used))
            apos += len(part)
        out.write(nv.CHUNK.pack(b"PAD ", 480, 0, 0) + bytes(480))
        index.append((out.tell(), t))
        out.write(nv.CHUNK.pack(b"VFRM", nv.FRAME_BYTES, t, 0) + bytes([k & 255, 0x80]) * (nv.FRAME_BYTES // 2))
        stored += 1
    idx = out.tell()
    for off, t in index:
        out.write(struct.pack("<II", off, t))
    hdr = nv.HEADER.pack(nv.MAGIC, 1, 64, 256, 192, fps, 1, stored, 0, int(seconds * 1000), apos, 1, 16000, 1, 1, idx, stored, nv.FLAG_COMPLETE | nv.FLAG_HAS_AUDIO, 0, 70, n)
    data = bytearray(out.getvalue()); data[:64] = hdr
    open(path, "wb").write(bytes(data))

def info(path):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        class A: pass
        a = A(); a.input = path
        nv.cmd_info(a)
    return buf.getvalue()

fail = 0
def check(c, msg):
    global fail
    if not c:
        print("FAIL:", msg); fail += 1

tmp = tempfile.mkdtemp()
try:
    for fps in nv.VALID_FPS:
        p = os.path.join(tmp, "f%d.nvid" % fps)
        make(p, fps, 3, drops={5, 6} if fps == 30 else ())
        t = info(p)
        check("requested fps : %d" % fps in t, "requested fps %d" % fps)
        check("NERDVID v1" in t, "version")
        if fps == 30:
            check("dropped       : 2" in t, "drops")
            check("actual avg fps: 29." in t, "actual avg fps\n" + t)
        check("audio         : yes" in t, "audio")
    # Phase 2C.1 sector-aligned file with PAD chunks
    p = os.path.join(tmp, "aligned.nvid"); make_aligned(p, 10, 2)
    t = info(p); check("requested fps : 10" in t and "frames        : 20" in t, "aligned file info\n" + t)
    # old file (no Phase 2C fields)
    p = os.path.join(tmp, "old.nvid"); make(p, 10, 2, p2c=False)
    check("recorder" not in info(p), "old file has no recorder line")
    # truncated tail and no index: scanned
    p = os.path.join(tmp, "trunc.nvid"); make(p, 20, 2, complete=False, truncate=5000)
    t = info(p); check("frames        :" in t and "complete      : False" in t, "truncated readable")
    # corrupt chunk: stops quietly
    p = os.path.join(tmp, "corrupt.nvid"); make(p, 15, 2, complete=False, corrupt_at=64 + 16 + 8192 + 16 + 98304 + 500)
    info(p)
    # garbage
    open(os.path.join(tmp, "bad"), "wb").write(b"hello")
    try:
        info(os.path.join(tmp, "bad")); check(False, "garbage must raise")
    except nv.NvidError:
        pass
    # conversion keeps real frame count with drops filled (needs ffmpeg)
    if shutil.which("ffmpeg"):
        p = os.path.join(tmp, "f30.nvid"); o = os.path.join(tmp, "o.mp4")
        class A: pass
        a = A(); a.input = p; a.output = o; a.crf = 30; a.scale = 1
        nv.cmd_to_video(a)
        r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=r_frame_rate,nb_frames", "-of", "csv=p=0", o], capture_output=True, text=True)
        check(r.stdout.startswith("30/1"), "output is 30 fps: " + r.stdout)
        check("90" in r.stdout, "90 frames (88 stored + 2 held): " + r.stdout)
        r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=sample_rate", "-of", "csv=p=0", o], capture_output=True, text=True)
        check("16000" in r.stdout or r.stdout.strip() != "", "audio stream present: " + r.stdout)
finally:
    shutil.rmtree(tmp, ignore_errors=True)
print("converter tests", "FAILED" if fail else "OK")
sys.exit(1 if fail else 0)
