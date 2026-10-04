#!/usr/bin/env python3
"""
nerdvid-convert: convert between nerdMod Camera videos (.nvid, see docs/NERDVID.md) and ordinary video files.

  nerdvid_convert.py to-video  in.nvid  out.mp4     [--crf 20] [--scale 2]
  nerdvid_convert.py from-video in.mp4  out.nvid    [--fps 10] [--no-audio]
  nerdvid_convert.py info      in.nvid

It needs only Python 3 and an ffmpeg you already have installed; no codec is bundled or implemented here.
A recording that was cut short (power loss) has no index and is read by scanning the chunks.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

MAGIC = b"NVID"
HEADER = struct.Struct("<4sHHHHHHIIIIHHHHIIII2I")  # 64 bytes
CHUNK = struct.Struct("<4sIII")
FRAME_W, FRAME_H = 256, 192
FRAME_BYTES = FRAME_W * FRAME_H * 2

FLAG_COMPLETE, FLAG_HAS_AUDIO, FLAG_INNER, FLAG_DROPPED = 1, 2, 4, 8


class NvidError(Exception):
    pass


def read_header(f):
    raw = f.read(HEADER.size)
    if len(raw) < HEADER.size:
        raise NvidError("file too short")
    (magic, version, hdr_size, w, h, fps_num, fps_den, frames, dropped, duration, audio_bytes, audio_fmt, audio_rate,
     audio_ch, video_fmt, index_off, index_cnt, flags, start_unix, r0, r1) = HEADER.unpack(raw)
    if magic != MAGIC:
        raise NvidError("not a NERDVID file")
    if version != 1 or (w, h) != (FRAME_W, FRAME_H) or video_fmt != 1:
        raise NvidError("unsupported NERDVID variant (version %d, %dx%d, format %d)" % (version, w, h, video_fmt))
    return dict(header_size=hdr_size, frames=frames, dropped=dropped, duration_ms=duration, audio_bytes=audio_bytes,
                audio_format=audio_fmt, audio_rate=audio_rate or 16000, audio_channels=audio_ch or 1, index_offset=index_off,
                index_count=index_cnt, flags=flags, start_unix=start_unix)


def iter_chunks(f, info):
    """Yields (fourcc, time_ms, payload) in file order. Stops quietly at a truncated tail."""
    f.seek(info["header_size"])
    end = info["index_offset"] if (info["flags"] & FLAG_COMPLETE) and info["index_offset"] else None
    pos = info["header_size"]
    while end is None or pos < end:
        raw = f.read(CHUNK.size)
        if len(raw) < CHUNK.size:
            return
        fourcc, size, time_ms, _ = CHUNK.unpack(raw)
        if fourcc not in (b"VFRM", b"AUDI") or size > (1 << 20):
            return  # garbage: treat as end of data
        payload = f.read(size)
        if len(payload) < size:
            return  # truncated chunk
        pos += CHUNK.size + size
        yield fourcc, time_ms, payload


def cmd_info(args):
    with open(args.input, "rb") as f:
        info = read_header(f)
        frames = audio = 0
        last = 0
        for fourcc, t, p in iter_chunks(f, info):
            if fourcc == b"VFRM":
                frames += 1
                last = t
            else:
                audio += len(p)
    print("file          :", args.input)
    print("complete      :", bool(info["flags"] & FLAG_COMPLETE))
    print("camera        :", "inner" if info["flags"] & FLAG_INNER else "outer")
    print("frames        : %d (header says %d, %d skipped)" % (frames, info["frames"], info["dropped"]))
    print("duration      : %.1f s" % ((info["duration_ms"] or last) / 1000.0))
    print("audio         : %s (%d bytes, %d Hz)" % ("yes" if info["audio_format"] == 1 else "no", audio, info["audio_rate"]))


def require_ffmpeg():
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit("ffmpeg was not found in PATH. Install it from your package manager or https://ffmpeg.org/.")
    return exe


def cmd_to_video(args):
    ffmpeg = require_ffmpeg()
    with open(args.input, "rb") as f, tempfile.TemporaryDirectory() as tmp:
        info = read_header(f)
        fps = 10
        raw_video = os.path.join(tmp, "video.raw")
        raw_audio = os.path.join(tmp, "audio.raw")
        frames = 0
        with open(raw_video, "wb") as vout, open(raw_audio, "wb") as aout:
            last_frame = None
            next_slot = 0
            for fourcc, t, payload in iter_chunks(f, info):
                if fourcc == b"AUDI":
                    aout.write(payload)
                    continue
                # constant 10 fps output: repeat the previous frame over gaps (skipped frames), never drop a real one
                slot = int(round(t * fps / 1000.0))
                if last_frame is not None:
                    while next_slot < slot:
                        vout.write(last_frame)
                        frames += 1
                        next_slot += 1
                vout.write(payload)
                frames += 1
                next_slot = max(next_slot, slot) + 1
                last_frame = payload
        if frames == 0:
            sys.exit("no video frames in %s" % args.input)
        has_audio = os.path.getsize(raw_audio) > 0 and info["audio_format"] == 1
        cmd = [ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgr555le", "-s", "%dx%d" % (FRAME_W, FRAME_H),
               "-r", str(fps), "-i", raw_video]
        if has_audio:
            cmd += ["-f", "s16le", "-ar", str(info["audio_rate"]), "-ac", "1", "-i", raw_audio]
        vf = "scale=iw*%d:ih*%d:flags=neighbor" % (args.scale, args.scale) if args.scale > 1 else "null"
        cmd += ["-vf", vf, "-pix_fmt", "yuv420p", "-c:v", "libx264", "-crf", str(args.crf)]
        if has_audio:
            cmd += ["-c:a", "aac", "-shortest"]
        cmd += [args.output]
        subprocess.run(cmd, check=True)
    print("wrote", args.output)


def cmd_from_video(args):
    ffmpeg = require_ffmpeg()
    fps = args.fps
    with tempfile.TemporaryDirectory() as tmp:
        raw_video = os.path.join(tmp, "video.raw")
        subprocess.run([ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", args.input, "-an", "-vf",
                        "fps=%d,scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2" % (fps, FRAME_W, FRAME_H, FRAME_W, FRAME_H),
                        "-f", "rawvideo", "-pix_fmt", "bgr555le", raw_video], check=True)
        audio = b""
        if not args.no_audio:
            raw_audio = os.path.join(tmp, "audio.raw")
            r = subprocess.run([ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", args.input, "-vn", "-ac", "1", "-ar", "16000",
                                "-f", "s16le", raw_audio])
            if r.returncode == 0 and os.path.exists(raw_audio):
                audio = open(raw_audio, "rb").read()
        size = os.path.getsize(raw_video)
        frames = size // FRAME_BYTES
        if frames == 0:
            sys.exit("no frames could be decoded from %s" % args.input)
        duration = int(frames * 1000 / fps)
        index = []
        with open(args.output, "wb") as out, open(raw_video, "rb") as vin:
            flags = FLAG_COMPLETE | (FLAG_HAS_AUDIO if audio else 0)
            out.write(b"\0" * HEADER.size)
            apos = 0
            audio_chunk = 16000 * 2 // 2  # half a second
            audio_chunk -= audio_chunk % 16
            for n in range(frames):
                t = int(n * 1000 / fps)
                # audio belonging to this frame time first, in half-second chunks
                while audio and apos < len(audio) and apos // 32 <= t:
                    part = audio[apos:apos + audio_chunk]
                    part += b"\0" * (-len(part) % 16)
                    out.write(CHUNK.pack(b"AUDI", len(part), apos // 32, 0) + part)
                    apos += audio_chunk
                frame = vin.read(FRAME_BYTES)
                # set bit 15 on every pixel, as the DSi writes it
                frame = bytearray(frame)
                frame[1::2] = bytes(b | 0x80 for b in frame[1::2])
                index.append((out.tell(), t))
                out.write(CHUNK.pack(b"VFRM", FRAME_BYTES, t, 0) + bytes(frame))
            index_off = out.tell()
            for off, t in index:
                out.write(struct.pack("<II", off, t))
            out.seek(0)
            out.write(HEADER.pack(MAGIC, 1, 64, FRAME_W, FRAME_H, fps, 1, frames, 0, duration, len(audio) + (-len(audio) % 16 if audio else 0),
                                  1 if audio else 0, 16000 if audio else 0, 1 if audio else 0, 1, index_off, frames, flags, 0, 0, 0))
    print("wrote", args.output, "(%d frames, %.1f s%s)" % (frames, duration / 1000.0, ", with audio" if audio else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("info")
    a.add_argument("input")
    a.set_defaults(fn=cmd_info)
    a = sub.add_parser("to-video")
    a.add_argument("input")
    a.add_argument("output")
    a.add_argument("--crf", type=int, default=20)
    a.add_argument("--scale", type=int, default=2, help="integer upscale factor (nearest neighbour), default 2")
    a.set_defaults(fn=cmd_to_video)
    a = sub.add_parser("from-video")
    a.add_argument("input")
    a.add_argument("output")
    a.add_argument("--fps", type=int, default=10)
    a.add_argument("--no-audio", action="store_true")
    a.set_defaults(fn=cmd_from_video)
    args = ap.parse_args()
    try:
        args.fn(args)
    except NvidError as e:
        sys.exit("error: %s" % e)
    except subprocess.CalledProcessError as e:
        sys.exit("ffmpeg failed (%s)" % e)


if __name__ == "__main__":
    main()
