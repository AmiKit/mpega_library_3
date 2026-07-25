#!/bin/bash
#
# Regenerate the committed test fixtures.
#
# The fixtures are synthetic (tones, seeded noise, silence) so they carry no
# third-party copyright and can live in a GPL repository. They are COMMITTED,
# so you do not need to run this to use the harness, it exists so the
# provenance of every fixture is reproducible and reviewable.
#
# Requires: ffmpeg with libmp3lame and the mp2 encoder.
#
# Each fixture targets a decoder path:
#   l3_44_stereo_cbr128   the common case: MPEG-1 Layer III, 44.1 kHz, CBR
#   l3_48_stereo_vbr      VBR + Xing/LAME header -> TOC seeking, duration math
#   l3_22_mono_cbr64      MPEG-2 LSF, mono
#   l3_8_mono_cbr32       MPEG-2.5 at 8 kHz, the rate the original got wrong
#   l2_44_stereo_cbr192   Layer II (a different decoder entirely)
#   l3_44_stereo_id3      ID3v2 tag in front -> the tag-skip path
#   l3_44_silence         digital silence -> dither behaviour on a null signal
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

echo
echo "Fixture checksums (commit these together with the files):"
rm -f fixtures/SHA256SUMS
( cd fixtures && sha256sum ./*.mp3 ./*.mp2 > SHA256SUMS )
cat fixtures/SHA256SUMS

cat <<'EOF'

NOTE: new fixes should come with TARGETED fixtures for their bugs, e.g.
  - an ID3v2 tag whose total size is an exact multiple of 8192
  - a free-format Layer III stream / MPEG-2.5 long frame
  - a stream that is truncated mid-frame (zero-progress guard)
A fix without a fixture that fails before it and passes after it is not verified.
EOF
