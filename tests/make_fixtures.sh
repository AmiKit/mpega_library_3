#!/bin/bash
#
# Regenerate the committed test fixtures.
#
# Most fixtures are synthetic (tones, seeded noise, silence); the three loud
# ones are short excerpts of a real track, see their recipe below. They are
# COMMITTED, so you do not need to run this to use the harness, it exists so
# the provenance of every fixture is reproducible and reviewable. Running it
# whole regenerates everything: to add one fixture, run only its block and
# check that the existing lines of SHA256SUMS did not change.
#
# Requires: ffmpeg with libmp3lame and the mp2 encoder.
#
# Which decoder path each fixture targets: the table in tests/README.md.
#
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p fixtures

DUR=2
OUTOPTS="-flags +bitexact -map_metadata -1"   # output-side options

echo "Generating fixtures with $(ffmpeg -version 2>/dev/null | head -1)"

gen() {   # gen <outfile> <ffmpeg input+codec args...>
    local out="fixtures/$1"; shift
    rm -f "$out"
    # shellcheck disable=SC2086
    ffmpeg -hide_banner -loglevel error -y "$@" $OUTOPTS "$out"
    printf '  %-26s %8s bytes\n' "$(basename "$out")" "$(stat -c%s "$out")"
}

# Two different tones per channel so L and R are never identical, that is what
# makes force_mono and any channel mix-up visible in the hashes.
STEREO_44="-fflags +bitexact -f lavfi -i sine=f=440:r=44100:d=$DUR
           -fflags +bitexact -f lavfi -i sine=f=660:r=44100:d=$DUR
           -filter_complex [0][1]amerge=inputs=2"
STEREO_48="-fflags +bitexact -f lavfi -i sine=f=1000:r=48000:d=$DUR
           -fflags +bitexact -f lavfi -i anoisesrc=c=pink:r=48000:d=$DUR:seed=42
           -filter_complex [0][1]amerge=inputs=2"

gen l3_44_stereo_cbr128.mp3 $STEREO_44 -c:a libmp3lame -b:a 128k -ac 2
gen l3_48_stereo_vbr.mp3    $STEREO_48 -c:a libmp3lame -q:a 4 -ac 2
gen l3_22_mono_cbr64.mp3    -fflags +bitexact -f lavfi -i sine=f=300:r=22050:d=$DUR -c:a libmp3lame -b:a 64k -ac 1
gen l3_8_mono_cbr32.mp3     -fflags +bitexact -f lavfi -i sine=f=200:r=8000:d=$DUR  -c:a libmp3lame -b:a 32k -ac 1
gen l2_44_stereo_cbr192.mp2 $STEREO_44 -c:a mp2 -b:a 192k -ac 2
gen l3_44_silence.mp3       -fflags +bitexact -f lavfi -i anullsrc=r=44100:cl=stereo -t $DUR -c:a libmp3lame -b:a 128k -ac 2

# ID3v2 in front of the audio: exercises the tag-skip path in InitDecoder.
rm -f fixtures/l3_44_stereo_id3.mp3
# shellcheck disable=SC2086
ffmpeg -hide_banner -loglevel error -y $STEREO_44 -c:a libmp3lame -b:a 128k -ac 2 \
    -flags +bitexact -id3v2_version 3 \
    -metadata title=mpega-fixture -metadata artist=synthetic \
    -metadata comment="$(head -c 600 /dev/zero | tr '\0' 'x')" \
    fixtures/l3_44_stereo_id3.mp3
printf '  %-26s %8s bytes\n' l3_44_stereo_id3.mp3 "$(stat -c%s fixtures/l3_44_stereo_id3.mp3)"

# --- targeted regression fixture for an 8192-byte ID3v2 tag -------------------
# An ID3v2 tag whose TOTAL size (10-byte header + payload) is an exact multiple
# of the 8192-byte stream buffer. CheckID3() returns 10 + syncsafe(size), so the
# size field must be 8182. In InitDecoder that makes "skip" decrement to exactly
# zero in the skip loop, the one path where "bytes" was never re-based, so the
# guard bzero ran a whole buffer past the allocation.
#
# Before the fix this file aborts under AddressSanitizer inside InitDecoder;
# after it, it decodes normally. Keep it: it is the only fixture that covers
# that path.
rm -f fixtures/l3_44_id3_8192.mp3
ffmpeg -hide_banner -loglevel error -y $STEREO_44 -c:a libmp3lame -b:a 128k -ac 2 \
    -flags +bitexact -id3v2_version 0 -write_id3v1 0 /tmp/id3base.mp3
python3 - <<'PY'
payload = 8192 - 10
syncsafe = bytes([(payload >> 21) & 0x7F, (payload >> 14) & 0x7F,
                  (payload >> 7) & 0x7F, payload & 0x7F])
tag = b"ID3" + bytes([3, 0, 0]) + syncsafe + b"\x00" * payload
assert len(tag) == 8192
open("fixtures/l3_44_id3_8192.mp3", "wb").write(tag + open("/tmp/id3base.mp3", "rb").read())
PY
rm -f /tmp/id3base.mp3
printf '  %-26s %8s bytes  (ID3v2 total exactly 8192)\n' l3_44_id3_8192.mp3 "$(stat -c%s fixtures/l3_44_id3_8192.mp3)"

# --- targeted regression fixture for the trailing-ID3v1 end of stream ---------
# The same audio as l3_44_stereo_cbr128.mp3 with a valid 128-byte ID3v1 tag
# appended, which is how the overwhelming majority of real-world MP3s end.
#
# Once a frame has decoded, stream.sync is set, and mad_header_decode then
# refuses to search for the next sync word: it tests the byte in front of it and
# fails. An ID3v1 tag starts with 'T', so its first byte is a hard LOSTSYNC that
# the wrapper used to report as MPEGA_ERR_NO_SYNC. Every file therefore ended in
# an error rather than a clean EOF, and a player showed one at the end of every
# normal track.
#
# Not one of the other fixtures covers this: the two named "id3" carry ID3v2 at
# the START of the file, which exercises a completely different path. Keep this
# one. Built by appending rather than by ffmpeg -write_id3v1 so the bytes are
# fixed and the fixture stays reproducible across ffmpeg versions.
rm -f fixtures/l3_44_stereo_id3v1.mp3
python3 - <<'PY'
def field(s, n):
    return s.encode("ascii").ljust(n, b"\x00")[:n]

tag = (b"TAG" + field("mpega regression", 30) + field("mpega.library", 30)
       + field("test fixtures", 30) + field("2026", 4)
       + field("trailing ID3v1 end of stream", 30) + bytes([12]))
assert len(tag) == 128 and tag[:3] == b"TAG"
open("fixtures/l3_44_stereo_id3v1.mp3", "wb").write(
    open("fixtures/l3_44_stereo_cbr128.mp3", "rb").read() + tag)
PY
printf '  %-26s %8s bytes  (128-byte ID3v1 trailer)\n' l3_44_stereo_id3v1.mp3 "$(stat -c%s fixtures/l3_44_stereo_id3v1.mp3)"

# --- loud broadband fixtures for the Hi-Fi clipping class ---------------------
# Three 2.5 s excerpts of loud, dense real music (source: our own track, the
# AmiKit X13 theme remix, AmiKitX13Remix.mp3, 48 kHz VBR with cover art; the
# three loudest stretches, at 100.0 s, 107.5 s and 113.5 s), re-encoded as
# self-contained 320 kbit/s CBR streams at 44.1 kHz. Re-encoded rather than
# stream-copied on purpose: a raw cut starts mid bit-reservoir, the first
# frames cannot decode, and decode_ref stops at the first error.
#
# They exist because every other fixture is QUIET, and a Hi-Fi clipping bug
# (the unused FPM_M68K scaling dropping the top five bits past 2^58)
# decoded all of them bit-perfectly while crackling audibly on real music.
# Proven when added (rounded output against ffmpeg's float decoder): the
# FPM_M68K core clips 1017-1191 samples per excerpt where the music stays
# below full scale, the shipping FPM_64BIT core none (peaks 0.89-0.91 of
# full scale, within 1 LSB). Synthetic pure tones do NOT reproduce the
# class; broadband energy across many subbands is needed.
#
# The source track is not in the repo, so these regenerate only when LOUD_SRC
# names it; otherwise the committed copies stand.
LOUD_SRC="${LOUD_SRC:-AmiKitX13Remix.mp3}"
if [ -f "$LOUD_SRC" ]; then
    for spec in "a 100.0" "b 107.5" "c 113.5"; do
        n=${spec%% *}; ss=${spec##* }
        ffmpeg -hide_banner -loglevel error -y -ss "$ss" -t 2.5 -i "$LOUD_SRC" -map 0:a \
            -c:a libmp3lame -b:a 320k -ac 2 -ar 44100 -fflags +bitexact \
            -id3v2_version 0 -write_id3v1 0 "fixtures/l3_44_stereo_loud_$n.mp3"
        printf '  %-26s %8s bytes  (loud excerpt @ %s s)\n' \
            "l3_44_stereo_loud_$n.mp3" "$(stat -c%s "fixtures/l3_44_stereo_loud_$n.mp3")" "$ss"
    done
else
    echo "  (loud fixtures kept as committed; source track not present)"
fi

# --- targeted regression fixture for the ReplayGain freeze --------------------
# A LAME Info tag whose Radio Replay Gain field has name and originator SET.
# That combination routes MPEGA_open through the gain computation, the two
# lines that were the library's only floating point, and that froze every
# 68020 build solid (unopened mathieeedoubbas/doubtrans, jump through the
# $FFFFFFFF autoinit sentinel). No other fixture reaches this path; the bug
# went unnoticed precisely because nothing here had a ReplayGain tag.
#
# The field is set to -13.2 dB (the same value as the file that found the
# bug), which after tag.c's LAME >= 3.95.1 reference-level compensation and
# the decoder's +6 dB pregain lands at -7.2 dB: gain 0.4365. The expected
# hashes therefore also pin the fixed-point gain arithmetic itself.
rm -f fixtures/l3_48_stereo_rgain.mp3
python3 - <<'PY'
src = open("fixtures/l3_48_stereo_vbr.mp3", "rb").read()
buf = bytearray(src)

# Locate the Xing/Info frame: ffmpeg writes a small ID3v2 in front, so find
# the marker and step back over the MPEG-1-stereo header + side info
# (4 + 32 bytes). tag.c reads the encoder string at frame+$9A and the Radio
# Replay Gain at frame+$AB, and validates a CRC over frame[0:190] stored at
# frame+$BE.
mark = buf.find(b"Xing")
if mark < 0:
    mark = buf.find(b"Info")
assert mark >= 36, "no Xing/Info header"
f = mark - 36
assert buf[f] == 0xFF and (buf[f + 1] & 0xE0) == 0xE0, "no sync at frame start"

# tag.c only applies LAME semantics when the version string says LAME.
buf[f + 0x9A:f + 0x9A + 9] = b"LAME3.100"

# 3 bits name=1 (RADIO), 3 bits originator=1 (set by artist),
# 1 bit sign=1 (negative), 9 bits magnitude=132 tenth-dB.
field = (1 << 13) | (1 << 10) | (1 << 9) | 132
buf[f + 0xAB] = field >> 8
buf[f + 0xAC] = field & 0xFF

# CRC-16/ARC over the frame's first 190 bytes, big-endian at $BE (mad/crc.c).
crc = 0
for b in buf[f:f + 190]:
    crc ^= b
    for _ in range(8):
        crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
buf[f + 0xBE] = crc >> 8
buf[f + 0xBF] = crc & 0xFF

open("fixtures/l3_48_stereo_rgain.mp3", "wb").write(bytes(buf))
PY
printf '  %-26s %8s bytes  (Radio RG -13.2 dB, originator set)\n' l3_48_stereo_rgain.mp3 "$(stat -c%s fixtures/l3_48_stereo_rgain.mp3)"

# --- mono part followed by a stereo part, one stream ---------------------------
# 1 s of mono, then the audio of l3_44_stereo_cbr128.mp3 without its ID3v2 tag,
# both 44.1 kHz Layer III, concatenated as one byte stream. Real streams do this
# when a mono jingle precedes a stereo track. With the harness row "mixq" (mono
# output: low quality, half rate; stereo output: high quality, full rate) the
# output configuration switches mid-stream. The half-rate option set for the
# mono part used to survive the switch, so the stereo part came out with half
# the samples while dec_frequency claimed the full rate: double speed.
rm -f fixtures/l3_44_monostereo.mp3
ffmpeg -hide_banner -loglevel error -y -fflags +bitexact -f lavfi -i sine=f=500:r=44100:d=1 \
    -c:a libmp3lame -b:a 64k -ac 1 -flags +bitexact -map_metadata -1 \
    -id3v2_version 0 -write_id3v1 0 -write_xing 0 /tmp/monopart.mp3
python3 - <<'PY'
st = open("fixtures/l3_44_stereo_cbr128.mp3", "rb").read()
skip = 0
if st[:3] == b"ID3":
    skip = 10 + ((st[6] & 0x7F) << 21 | (st[7] & 0x7F) << 14 | (st[8] & 0x7F) << 7 | (st[9] & 0x7F))
assert st[skip] == 0xFF and (st[skip + 1] & 0xE0) == 0xE0, "no sync after ID3v2"
open("fixtures/l3_44_monostereo.mp3", "wb").write(open("/tmp/monopart.mp3", "rb").read() + st[skip:])
PY
rm -f /tmp/monopart.mp3
printf '  %-26s %8s bytes  (mono, then stereo)\n' l3_44_monostereo.mp3 "$(stat -c%s fixtures/l3_44_monostereo.mp3)"

# --- Xing header whose frame count is zero --------------------------------------
# l3_48_stereo_vbr.mp3 with the Xing "frames" field cleared (LAME CRC fixed up
# so the rest of the tag still parses). Encoders that write the header before
# they know the count, and truncated rips, leave exactly this. The decoder used
# to trust it: duration 0 ms, and every seek jumped to the 99 % TOC entry.
rm -f fixtures/l3_48_stereo_xing0.mp3
python3 - <<'PY'
buf = bytearray(open("fixtures/l3_48_stereo_vbr.mp3", "rb").read())
mark = buf.find(b"Xing")
assert mark >= 36, "no Xing header"
flags = int.from_bytes(buf[mark + 4:mark + 8], "big")
assert flags & 1, "Xing header carries no frame count"
buf[mark + 8:mark + 12] = bytes(4)
f = mark - 36
crc = 0
for b in buf[f:f + 190]:
    crc ^= b
    for _ in range(8):
        crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
buf[f + 0xBE] = crc >> 8
buf[f + 0xBF] = crc & 0xFF
open("fixtures/l3_48_stereo_xing0.mp3", "wb").write(bytes(buf))
PY
printf '  %-26s %8s bytes  (Xing frames = 0)\n' l3_48_stereo_xing0.mp3 "$(stat -c%s fixtures/l3_48_stereo_xing0.mp3)"

# --- VBR with a Xing header but no TOC ------------------------------------------
# Half a second of silence (32 kbit/s frames), then pink noise (about 180), with
# the TOC flag cleared afterwards: frames and bytes stay, so a seek has to go by
# the average bitrate. The LAME tag no longer parses (it is read from inside the
# old TOC), like the encoders that write no TOC. A seek used to take the bitrate
# of the last decoded frame, here a silent one, and landed near the start.
rm -f fixtures/l3_44_notoc.mp3
ffmpeg -hide_banner -loglevel error -y \
    -fflags +bitexact -f lavfi -i anoisesrc=c=pink:r=44100:d=$DUR:a=0.5:seed=42 \
    -fflags +bitexact -f lavfi -i anoisesrc=c=pink:r=44100:d=$DUR:a=0.5:seed=7 \
    -filter_complex "[0][1]amerge=inputs=2,volume=volume=0:enable='lt(t,0.5)'" \
    -c:a libmp3lame -q:a 2 -ac 2 $OUTOPTS /tmp/notocbase.mp3
python3 - <<'PY'
buf = bytearray(open("/tmp/notocbase.mp3", "rb").read())
mark = buf.find(b"Xing")
assert mark >= 36, "no Xing header"
flags = int.from_bytes(buf[mark + 4:mark + 8], "big")
assert flags & 3 == 3 and flags & 4, "Xing header with frames, bytes and TOC expected"
buf[mark + 4:mark + 8] = (flags & ~4).to_bytes(4, "big")
open("fixtures/l3_44_notoc.mp3", "wb").write(bytes(buf))
PY
rm -f /tmp/notocbase.mp3
printf '  %-26s %8s bytes  (silence then noise, Xing without TOC)\n' l3_44_notoc.mp3 "$(stat -c%s fixtures/l3_44_notoc.mp3)"

# --- regression fixture for the hook size fallback ----------------------------
# A 10000-byte ID3v2 tag, 100 bytes of padding after it, then plain CBR audio
# with NO Xing/LAME header. Read through the hook row ("hook", stream_size 0),
# the open falls back to max(first read, tag size) = 10000 for the stream size,
# while the skip in front of the audio is 10100. Subtracting the two
# unguarded reported a duration of hours. Every other fixture carries a
# Xing/LAME header that corrects the size first, which is why the bug
# slipped past.
rm -f fixtures/l3_44_id3_pad_noxing.mp3
ffmpeg -hide_banner -loglevel error -y $STEREO_44 -c:a libmp3lame -b:a 128k -ac 2 \
    -flags +bitexact -map_metadata -1 -id3v2_version 0 -write_id3v1 0 -write_xing 0 /tmp/padbase.mp3
python3 - <<'PY'
payload = 10000 - 10
syncsafe = bytes([(payload >> 21) & 0x7F, (payload >> 14) & 0x7F,
                  (payload >> 7) & 0x7F, payload & 0x7F])
tag = b"ID3" + bytes([3, 0, 0]) + syncsafe + bytes(payload)
assert len(tag) == 10000
audio = open("/tmp/padbase.mp3", "rb").read()
assert audio[0] == 0xFF and (audio[1] & 0xE0) == 0xE0, "audio must start with a frame"
assert b"Xing" not in audio[:200] and b"Info" not in audio[:200], "no Xing/Info header wanted"
open("fixtures/l3_44_id3_pad_noxing.mp3", "wb").write(tag + bytes(100) + audio)
PY
rm -f /tmp/padbase.mp3
printf '  %-26s %8s bytes  (ID3v2 10000 + 100 padding, no Xing)\n' l3_44_id3_pad_noxing.mp3 "$(stat -c%s fixtures/l3_44_id3_pad_noxing.mp3)"

# --- a track that starts mid-stream -------------------------------------------
# l3_44_stereo_cbr128.mp3 cut at a frame boundary without re-encoding, the way
# frame-exact cutters and stream recorders make files: the first frame needs
# bit-reservoir bytes from before the cut, and the Info frame is gone. The open
# used to decode the first two frames from leftover reservoir bytes, as noise.
# Cut from a committed fixture, so this block needs no ffmpeg.
python3 - <<'PY'
BR = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
SR = [44100, 48000, 32000]
d = open("fixtures/l3_44_stereo_cbr128.mp3", "rb").read()
i = 10 + (d[6] << 21 | d[7] << 14 | d[8] << 7 | d[9]) if d[:3] == b"ID3" else 0
frames = []                                    # (offset, main_data_begin)
while i + 6 < len(d) and d[i] == 0xFF and (d[i + 1] & 0xFE) == 0xFA:
    o = i + (4 if d[i + 1] & 1 else 6)         # side info, after the CRC if any
    frames.append((i, d[o] << 1 | d[o + 1] >> 7))
    i += 144000 * BR[d[i + 2] >> 4] // SR[d[i + 2] >> 2 & 3] + (d[i + 2] >> 1 & 1)
assert i == len(d), "MPEG-1 Layer III frames up to the end expected"
k = next(n for n in range(15, len(frames)) if frames[n][1] > 0)
open("fixtures/l3_44_midstream.mp3", "wb").write(d[frames[k][0]:])
print("  cut at frame %d, main_data_begin %d" % (k, frames[k][1]))
PY
printf '  %-26s %8s bytes  (cut mid-stream, no Xing)\n' l3_44_midstream.mp3 "$(stat -c%s fixtures/l3_44_midstream.mp3)"

# --- two ID3v2 tags in a row --------------------------------------------------
# The audio of l3_44_stereo_cbr128.mp3 behind two 10000-byte tags instead of its
# own, the way taggers that do not remove the old tag leave files. Only the
# first tag used to be skipped; no frame turned up in the first buffer and the
# open failed. Through a hook without a size the big first tag also kept the
# size guess away from the LAME music length: 0 ms. Built from a committed
# fixture, so this block needs no ffmpeg.
python3 - <<'PY'
d = open("fixtures/l3_44_stereo_cbr128.mp3", "rb").read()
assert d[:3] == b"ID3", "cbr128 must start with a tag"
audio = d[10 + (d[6] << 21 | d[7] << 14 | d[8] << 7 | d[9]):]
payload = 10000 - 10
tag = b"ID3" + bytes([3, 0, 0]) + bytes([(payload >> s) & 0x7F for s in (21, 14, 7, 0)]) + bytes(payload)
assert len(tag) == 10000
open("fixtures/l3_44_two_id3.mp3", "wb").write(tag + tag + audio)
PY
printf '  %-26s %8s bytes  (two 10000-byte ID3v2 tags)\n' l3_44_two_id3.mp3 "$(stat -c%s fixtures/l3_44_two_id3.mp3)"

# --- junk in front of the audio -------------------------------------------------
# 9000 seeded random bytes, then the audio of l3_44_stereo_cbr128.mp3: no frame
# in the first 8 KB, so the open fails unless the player passes check_mpeg 0
# (decode_ref --nocheck). That path used to skip the end of the open, and the
# first decode returned MPEGA_ERR_NO_SYNC. No ffmpeg needed.
python3 - <<'PY'
import random
d = open("fixtures/l3_44_stereo_cbr128.mp3", "rb").read()
audio = d[10 + (d[6] << 21 | d[7] << 14 | d[8] << 7 | d[9]):]
random.seed(1)
open("fixtures/l3_44_junk.mp3", "wb").write(bytes(random.getrandbits(8) for _ in range(9000)) + audio)
PY
printf '  %-26s %8s bytes  (9000 junk bytes in front)\n' l3_44_junk.mp3 "$(stat -c%s fixtures/l3_44_junk.mp3)"

# --- two files joined into one -------------------------------------------------
# l3_44_stereo_id3v1.mp3 and l3_44_stereo_cbr128.mp3 back to back, as
# "cat a.mp3 b.mp3" makes them: the first one's ID3v1 tag and the second one's
# ID3v2 tag sit where the next frame should be. The decode used to stop there
# with MPEGA_ERR_NO_SYNC. The 39 zero bytes after the first file's ID3v2 tag
# move its frames so that with the 100-byte reads of the "short" row only 8
# bytes of the second tag's header are in the buffer when the decoder meets
# it, fewer than the 10 the tag check needs. At the end an ID3v1 tag cut short
# (63 of its 128 bytes, a broken download): its skip runs past the end of the
# file, which the "loop" row needs. No ffmpeg needed.
python3 - <<'PY'
a = open("fixtures/l3_44_stereo_id3v1.mp3", "rb").read()
t = 10 + (a[6] << 21 | a[7] << 14 | a[8] << 7 | a[9])
b = open("fixtures/l3_44_stereo_cbr128.mp3", "rb").read()
open("fixtures/l3_44_joined.mp3", "wb").write(a[:t] + bytes(39) + a[t:] + b + b"TAG" + b" " * 60)
PY
printf '  %-26s %8s bytes  (two files joined)\n' l3_44_joined.mp3 "$(stat -c%s fixtures/l3_44_joined.mp3)"

# --- a loud track made 10.5 dB louder, beyond full scale ------------------------
# l3_44_stereo_loud_a.mp3 with 7 added to every global_gain (7 x 1.5 dB), which
# is what MP3Gain does: it decodes to about three times full scale (peak 3.06).
# The long-block IMDCT and the synthesis DCT used to run past Q28's +-8.0
# there, so loud samples wrapped and flipped sign (5969 of them in -hifi before
# the headroom; ffmpeg's float decoder as the reference). The Hi-Fi flavours
# now clip; -bugfix keeps
# the original limit of about twice full scale. The Info frame is left alone,
# its LAME CRC covers the side info. No ffmpeg needed.
python3 - <<'PY'
d = bytearray(open("fixtures/l3_44_stereo_loud_a.mp3", "rb").read())
BR = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
SR = [44100, 48000, 32000]
i = 0
while i + 36 < len(d):
    assert d[i] == 0xFF and d[i + 1] == 0xFB and d[i + 3] >> 6 != 3, "MPEG-1 L3 stereo, no CRC"
    if d[i + 36:i + 40] not in (b"Xing", b"Info"):
        si = int.from_bytes(d[i + 4:i + 36], "big")
        for k in range(4):                      # granule x channel, 59 bits each
            shift = 256 - (20 + k * 59 + 21) - 8
            g = (si >> shift) & 0xFF
            si = si & ~(0xFF << shift) | (min(g + 7, 255) << shift)
        d[i + 4:i + 36] = si.to_bytes(32, "big")
    i += 144000 * BR[d[i + 2] >> 4] // SR[d[i + 2] >> 2 & 3] + (d[i + 2] >> 1 & 1)
open("fixtures/l3_44_stereo_hot.mp3", "wb").write(bytes(d))
PY
printf '  %-26s %8s bytes  (loud_a + 10.5 dB through global_gain)\n' l3_44_stereo_hot.mp3 "$(stat -c%s fixtures/l3_44_stereo_hot.mp3)"

echo
echo "Fixture checksums (commit these together with the files):"
rm -f fixtures/SHA256SUMS
( cd fixtures && sha256sum ./*.mp3 ./*.mp2 > SHA256SUMS )
cat fixtures/SHA256SUMS
