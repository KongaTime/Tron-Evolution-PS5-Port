#!/bin/bash
# Usage: run.sh <name> <seconds> [noupload]
# Kill Tron, upload eboot + flags, launch, wait, fetch the newest log (and screenshots) into logs/<name>.
set -u
# PS5_IP: the console's address; TRON_WORK: the Windows work folder (dist/, logs/,
# ctl.py); FASTSEND: the fastsend tool's folder.
IP=${PS5_IP:?set PS5_IP to the console address}
T=PPSA90120
K=${TRON_WORK:?set TRON_WORK}
FS=${FASTSEND:?set FASTSEND to the fastsend folder}
name=$1; secs=$2
out=$K/logs/$name; mkdir -p $out
python $K/ctl.py $IP kill $T >/dev/null 2>&1
sleep 2
if [ "${3:-}" != noupload ]; then
  (cd $FS && MSYS_NO_PATHCONV=1 python fastsend.py send 'C:\kb\tron-ps5\dist\PPSA90120\eboot.bin' 'C:\kb\tron-ps5\tron-flags.txt' /data/homebrew/$T 2>&1 | tail -1)
fi
for f in $(curl -s ftp://$IP:2121/data/homebrew/$T/logs/ | awk '{print $NF}' | grep -E '^(auto|cap)_.*bmp$'); do
  curl -s -Q "DELE /data/homebrew/$T/logs/$f" ftp://$IP:2121/ >/dev/null
done
for f in $(curl -s ftp://$IP:2121/data/homebrew/$T/frame_dump/cap/ | awk '{print $NF}' | grep -E 'png$'); do
  curl -s -Q "DELE /data/homebrew/$T/frame_dump/cap/$f" ftp://$IP:2121/ >/dev/null
done
python $K/ctl.py $IP launch $T 2>&1 | tail -1
sleep $secs
L=$(curl -s ftp://$IP:2121/data/homebrew/$T/logs/ | awk '{print $NF}' | grep '^tronevolution_.*log$' | sort | tail -1)
curl -s -o $out/tron.log ftp://$IP:2121/data/homebrew/$T/logs/$L
for f in $(curl -s ftp://$IP:2121/data/homebrew/$T/logs/ | awk '{print $NF}' | grep -E '^(auto|cap)_.*bmp$'); do
  curl -s -o $out/$f ftp://$IP:2121/data/homebrew/$T/logs/$f
done
mkdir -p $out/cap
for f in $(curl -s ftp://$IP:2121/data/homebrew/$T/frame_dump/cap/ | awk '{print $NF}' | grep -E 'png$'); do
  curl -s -o $out/cap/$f ftp://$IP:2121/data/homebrew/$T/frame_dump/cap/$f
done
curl -s -o $out/title-results.txt ftp://$IP:2121/data/homebrew/$T/title-results.txt
echo "log $L -> $out ($(wc -l < $out/tron.log) lines)"
