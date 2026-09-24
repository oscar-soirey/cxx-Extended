cmake -S . -B build ^
    -DCMAKE_TOOLCHAIN_FILE=../CXXEToolchain.cmake ^
    -DCXXE_EXECUTABLE="../../cxxe.exe"
pause