#!/bin/bash -eu

"$CXX" $CXXFLAGS -std=c++23 \
    -I"$SRC/nativeshift/src/NativeShift.Core/include" \
    "$SRC/nativeshift/fuzz/format_detector_fuzzer.cpp" \
    "$SRC/nativeshift/src/NativeShift.Core/src/format_detector.cpp" \
    "$SRC/nativeshift/src/NativeShift.Core/src/formats.cpp" \
    $LIB_FUZZING_ENGINE \
    -o "$OUT/format_detector_fuzzer"

cp "$SRC/nativeshift/fuzz/format_detector_fuzzer.dict" "$OUT/"
cp "$SRC/nativeshift/fuzz/format_detector_fuzzer.options" "$OUT/"
