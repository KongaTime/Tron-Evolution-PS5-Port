# Build the native-renderer Tron title and stage it in C:\kb\tron-ps5\dist.
export REXGLUE_SRC=$HOME/tron-ps5/nat/rex REXGLUE_PS5_BUILD=$HOME/tron-ps5/nat/runtime2 TRON_DILIGENT_ROOT=$HOME/tron-ps5/nat
cd ~/tron-ps5 && GAME=$HOME/tron-ps5/games/tron-native bash ~/PS5_ReXGlueTemplate/tools/build.sh > build-native.log 2>&1; echo build=$?
grep -E "error|FAILED" build-native.log | head -20; tail -3 build-native.log
rm -rf /mnt/c/kb/tron-ps5/dist/PPSA90120 && cp -r ~/PS5_ReXGlueTemplate/dist/PPSA90120 /mnt/c/kb/tron-ps5/dist/
# The launcher screens (art kept out of git, in title-art/).
cp ~/tron-ps5/games/tron-native/title-art/launcher-* /mnt/c/kb/tron-ps5/dist/PPSA90120/ 2>/dev/null
