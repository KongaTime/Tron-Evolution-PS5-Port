# Build the PGO variants of the native title: bpgo.sh gen | use
# gen: instrumented (writes /app0/pgo/tron.profraw every 30 s); use: optimized with pgo/tron.profdata.
mode=$1
export REXGLUE_SRC=$HOME/tron-ps5/nat/rex REXGLUE_PS5_BUILD=$HOME/tron-ps5/nat/runtime2 TRON_DILIGENT_ROOT=$HOME/tron-ps5/nat TRON_PGO=$mode
G=$HOME/tron-ps5/games/tron-pgo$mode
[ -e $G ] || ln -s $HOME/tron-ps5/games/tron-native $G
cd ~/tron-ps5 && GAME=$G bash ~/PS5_ReXGlueTemplate/tools/build.sh > build-pgo$mode.log 2>&1; echo build=$?
grep -E "error|FAILED" build-pgo$mode.log | head -10; tail -3 build-pgo$mode.log
rm -rf /mnt/c/kb/tron-ps5/dist-pgo$mode && cp -r ~/PS5_ReXGlueTemplate/dist/PPSA90120 /mnt/c/kb/tron-ps5/dist-pgo$mode
# The launcher screens (art kept out of git, in title-art/).
cp ~/tron-ps5/games/tron-native/title-art/launcher-* /mnt/c/kb/tron-ps5/dist-pgo$mode/ 2>/dev/null
