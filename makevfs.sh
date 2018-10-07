# Script to build a multi-platform vfs

# Do this separately on mac:
# ./configure
# make clean
# make
# make vfsmac

# Cross-compiling needs a cross-compiled tcl for linking
./configure --host=i586-mingw32msvc --target=i586-mingw32msvc --with-tcl=/home/peter/tcl/win/tcl/win
make clean
make
make vfswin

# 32-bit compiling needs a 32 bit compiled tcl for linking
CFLAGS=-m32 ./configure --disable-64bit --with-tcl=/home/peter/tcl/m32/tcl/unix
make clean
make
make vfs32

./configure --enable-64bit --with-tcl=/home/peter/tcl/v86/tcl/unix
make clean
make
make vfs64

# End with a sanity check
ls -l lib.vfs/DiffUtil/*/*.so lib.vfs/DiffUtil/*/*.dylib lib.vfs/DiffUtil/*/*.dll
file lib.vfs/DiffUtil/*/*.so lib.vfs/DiffUtil/*/*.dylib lib.vfs/DiffUtil/*/*.dll
