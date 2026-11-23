# Configure (once) and build the native runtime copy ~/tron-ps5/nat/rex -> nat/rex/out/ps5-amd64.
export PS5_CLANG=${PS5_CLANG:-clang-21}
SRC=$HOME/tron-ps5/nat/rex
B=$HOME/tron-ps5/nat/runtime2
TC=$HOME/PS5_ReXGlueTemplate/.deps/native/ps5-payload-sdk/toolchain/prospero.cmake
if [ ! -f $B/build.ninja ]; then
  cmake -S $SRC -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_ENABLE_TRACY=OFF \
    -DCMAKE_TOOLCHAIN_FILE=$TC '-DCMAKE_C_FLAGS=-march=znver2 -fno-omit-frame-pointer' \
    '-DCMAKE_CXX_FLAGS=-march=znver2 -fno-omit-frame-pointer' > $HOME/tron-ps5/configure-runtime.log 2>&1 || { tail -20 $HOME/tron-ps5/configure-runtime.log; exit 1; }
fi
ninja -C $B rexruntime rexgpu-xenos > $HOME/tron-ps5/build-runtime.log 2>&1; echo runtime=$?
grep -E "error:|FAILED" $HOME/tron-ps5/build-runtime.log | head; tail -2 $HOME/tron-ps5/build-runtime.log
ls -la $SRC/out/ps5-amd64/librexruntime.a $SRC/out/ps5-amd64/librexgpu-xenos.a
