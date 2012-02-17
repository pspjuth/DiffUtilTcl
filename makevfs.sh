# Script to build a multi-platform vfs

# Cross-compiling needs a cross-compiled tcl for linking
./configure --host=i586-mingw32msvc --target=i586-mingw32msvc --with-tcl=/home/peter/tcl/win/tcl/win
make clean
make
make vfswin

./configure --disable-64bit
make clean
make
make vfs32

./configure --enable-64-bit
make clean
make
make vfs64
