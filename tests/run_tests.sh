#!/usr/bin/env bash
# Exercises camdvd + extract.py against simulated camcorder discs.
# Run from the camdvd folder:  tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
BIN=bin/camdvd
T=$(mktemp -d "${TMPDIR:-/tmp}/camdvd-test.XXXXXX")
FX=tests/fixtures
pass=0; fail=0
ok()   { echo "PASS  $1"; pass=$((pass+1)); }
bad()  { echo "FAIL  $1"; fail=$((fail+1)); }
expect_rc() { local want=$1 got=$2 name=$3; [ "$got" = "$want" ] && ok "$name (exit $got)" || bad "$name: exit $got, wanted $want"; }

# Fixtures: three short NTSC DVD clips, made once.
if [ ! -f $FX/clip3.mpg ]; then
  mkdir -p $FX
  for i in 1 2 3; do
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=720x480:rate=30000/1001:duration=6" \
      -f lavfi -i "sine=frequency=$((300*i)):duration=6" -vf setfield=tff -target ntsc-dvd -flags +ilme+ildct $FX/clip$i.mpg
  done
fi

# Disc layout like an unfinalized camcorder DVD-R:
#   LBA 0-2047     reserved track, never written (filled in at finalize)
#   LBA 2048-      recording track: clip1, 32 sectors of management data, clip2, clip3
python3 - "$T/disc.img" $FX <<'EOF'
import os, sys
out, fx = sys.argv[1], sys.argv[2]
with open(out, "wb") as f:
    f.write(b"\0" * 2048 * 2048)
    f.write(open(fx + "/clip1.mpg", "rb").read())
    f.write(os.urandom(2048 * 32).replace(b"\x00\x00\x01\xba", b"\xff\xff\xff\xff"))
    f.write(open(fx + "/clip2.mpg", "rb").read())
    f.write(open(fx + "/clip3.mpg", "rb").read())
print(os.path.getsize(out) // 2048)
EOF
END=$(( $(stat -c %s "$T/disc.img" 2>/dev/null || stat -f %z "$T/disc.img") / 2048 ))
echo "simulated disc: recording ends at LBA $END"

base_cfg() {
  cat <<EOF
image $T/disc.img
vendor HL-DT-ST DVDRAM GP65NB60
profile 0x0011
disc_status 1
capacity 0
track 1 0 2048 blank
track 2 2048 300000 nwa $END
EOF
}
{ base_cfg; echo "bad 5500 5509"; echo "flaky 3000 3003 2"; } > $T/damaged.sim
{ base_cfg; echo "bad 5500 5509"; echo "flaky 3000 3003 2"; echo "wedge_after 150"; } > $T/wedge.sim

# 1. info reports the reserved track and the open recording track
out=$($BIN info -d sim:$T/damaged.sim)
echo "$out" | sed 's/^/      /'
echo "$out" | grep -q "incomplete (not finalized)" && echo "$out" | grep -q "recorded $((END-2048)) sectors" \
  && ok "info finds recorded area" || bad "info output"

# 2. rip with damage: flaky sectors recovered, bad sectors reported, exit 2
$BIN rip -d sim:$T/damaged.sim -o $T/a.img > $T/a.log 2>&1; rc=$?
expect_rc 2 $rc "rip damaged disc"
grep -q "Unreadable: 10 sectors" $T/a.log && ok "exactly the 10 bad sectors are unreadable" || { bad "bad sector count"; tail -5 $T/a.log; }

python3 - $T/disc.img $T/a.img $END <<'EOF' && ok "image matches disc except bad sectors" || bad "image content"
import sys
a = open(sys.argv[1], "rb").read(); b = open(sys.argv[2], "rb").read().ljust(len(a), b"\0")
end = int(sys.argv[3]); S = 2048
for l in range(2048, end):
    want = a[l*S:(l+1)*S] if not 5500 <= l <= 5509 else b"\0" * S
    if b[l*S:(l+1)*S] != want:
        print("mismatch at LBA", l); sys.exit(1)
EOF

# 3. drive drops out mid-rip, then resume finishes the job
$BIN rip -d sim:$T/wedge.sim -o $T/b.img > $T/b1.log 2>&1; rc=$?
expect_rc 3 $rc "drive drop is detected and progress saved"
$BIN rip -d sim:$T/damaged.sim -o $T/b.img > $T/b2.log 2>&1; rc=$?
expect_rc 2 $rc "resume after drop"
grep -q "Resuming:" $T/b2.log && ok "resume used the saved map" || bad "resume message"
cmp -s $T/a.img $T/b.img && ok "resumed image identical to uninterrupted rip" || bad "resumed image differs"

# 4. a different disc is refused when resuming
python3 -c "
import os; open('$T/other.img','wb').write(os.urandom(2048*$END))"
sed "s#$T/disc.img#$T/other.img#" $T/damaged.sim > $T/other.sim
$BIN rip -d sim:$T/other.sim -o $T/a.img > $T/c.log 2>&1; rc=$?
expect_rc 1 $rc "wrong disc refused"
grep -q "does not match" $T/c.log && ok "wrong-disc message" || bad "wrong-disc message"

# 5. drive that reports nothing: --probe finds the recording
cat > $T/hidden.sim <<EOF
image $T/disc.img
profile 0x0011
disc_status -1
capacity 0
readable 0 $((END-1))
EOF
$BIN rip -d sim:$T/hidden.sim -o $T/p.img > $T/p0.log 2>&1; rc=$?
expect_rc 1 $rc "no track info without --probe gives a clear error"
grep -q "try --probe\|Try --probe" $T/p0.log && ok "error suggests --probe" || bad "probe hint"
$BIN rip -d sim:$T/hidden.sim -o $T/p.img --probe > $T/p.log 2>&1; rc=$?
expect_rc 0 $rc "probe rip"

# 6. DVD-RAM: random access disc, read by capacity
cat > $T/ram.sim <<EOF
image $T/disc.img
profile 0x0012
disc_status 2
capacity $END
EOF
$BIN rip -d sim:$T/ram.sim -o $T/r.img > $T/r.log 2>&1; rc=$?
expect_rc 0 $rc "DVD-RAM rip"
grep -q "DVD-RAM" $T/r.log && ok "DVD-RAM detected" || bad "DVD-RAM detection"

# 7. retry-bad after "cleaning the disc": bad sectors become readable
base_cfg > $T/clean.sim
$BIN rip -d sim:$T/clean.sim -o $T/a.img --retry-bad > $T/d.log 2>&1; rc=$?
expect_rc 0 $rc "retry-bad recovers sectors on a second attempt"
cmp -s <(head -c $((END*2048)) $T/disc.img | tail -c +$((2048*2048+1))) <(head -c $((END*2048)) $T/a.img | tail -c +$((2048*2048+1))) \
  && ok "image now complete" || bad "image incomplete after retry-bad"

# 8. extraction: three recordings, durations about 6 s, damage noted on scene 2
python3 extract.py $T/b.img -o $T/scenes --format mpg,prores,mp4 > $T/x.log 2>&1; rc=$?
cat $T/x.log | sed 's/^/      /'
expect_rc 0 $rc "extract"
n=$(ls $T/scenes/scene_*.mpg 2>/dev/null | wc -l | tr -d ' ')
[ "$n" = "3" ] && ok "three recordings split out" || bad "found $n recordings, wanted 3"
dur_ok() {  # file min max
  local d; d=$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$1")
  python3 -c "import sys; d=float('$d'); sys.exit(0 if $2 < d < $3 else 1)" \
    && ok "$(basename $1) plays, ${d}s" || bad "$(basename $1) duration $d, wanted $2-$3"
}
for f in $T/scenes/scene_*.{mpg,mov,mp4}; do dur_ok "$f" 5.5 6.6; done
grep -q "scene 002.*10 unreadable sectors" $T/x.log && ok "damage attributed to scene 2" || bad "damage note"
ffmpeg -nostdin -v error -i $T/scenes/scene_002.mpg -f null - 2>$T/dec.log; \
  [ $(wc -l < $T/dec.log) -lt 40 ] && ok "damaged scene decodes end to end ($(wc -l < $T/dec.log | tr -d ' ') decoder warnings)" || bad "damaged scene decode"
ffprobe -v error -select_streams v -show_entries stream=field_order -of default=nw=1:nk=1 $T/scenes/scene_001.mov | grep -q "tt\|tb" \
  && ok "ProRes keeps interlacing" || bad "ProRes field order"

# 9. burst-written discs: pieces rejoin into whole scenes
python3 tests/make_burst_disc.py $T $FX >/dev/null || bad "build burst discs"
python3 extract.py $T/burst.img -o $T/burst --join > $T/burst.log 2>&1; rc=$?
expect_rc 0 $rc "extract burst-written disc"
n=$(ls $T/burst/scene_*.mpg 2>/dev/null | wc -l | tr -d ' ')
[ "$n" = "2" ] && ok "15 bursts rejoined into 2 scenes" || { bad "burst disc gave $n scenes, wanted 2"; cat $T/burst.log; }
dur_ok $T/burst/scene_001.mpg 29.5 30.6
dur_ok $T/burst/scene_002.mpg 19.5 20.6
dur_ok $T/burst/whole_disc.mpg 49.5 50.8
python3 extract.py $T/burst.img -o $T/burst-pieces --split pieces > /dev/null 2>&1
n=$(ls $T/burst-pieces/scene_*.mpg 2>/dev/null | wc -l | tr -d ' ')
[ "$n" = "14" ] && ok "--split pieces shows every burst with video in it" || bad "--split pieces gave $n"

python3 extract.py $T/nav.img -o $T/nav > $T/nav.log 2>&1; rc=$?
expect_rc 0 $rc "extract disc with scene numbers"
n=$(ls $T/nav/scene_*.mpg 2>/dev/null | wc -l | tr -d ' ')
[ "$n" = "3" ] && ok "scene numbers split a continuous clock, and join across a clock restart" || { bad "nav disc gave $n scenes, wanted 3"; cat $T/nav.log; }
for f in $T/nav/scene_*.mpg; do dur_ok "$f" 11.5 12.8; done

echo
echo "$pass passed, $fail failed   (work dir $T)"
[ $fail -eq 0 ]
