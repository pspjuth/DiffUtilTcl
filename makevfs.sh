# Script to build a multi-platform vfs

# Cross-compiling needs a cross-compiled tcl for linking
CFLAGS=-fPIC ./configure --host=i586-mingw32msvc --target=i586-mingw32msvc --with-tcl=/home/peter/tcl/win/tcl/win
make clean
make
make vfswin

# 32-bit compiling needs a 32 bit compiled tcl for linking
CFLAGS=-m32 ./configure --disable-64bit --with-tcl=/home/peter/tcl/m32/tcl/unix
make clean
make
make vfs32

./configure --enable-64bit
make clean
make
make vfs64
