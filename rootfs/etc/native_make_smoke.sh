set -x
export PATH=/bin:/usr/bin:/
echo native-make-start
cp /src/Makefile.make_smoke /work/Makefile
cp /src/test_native.c /work/test_native.c
echo direct-shell-path-start
/bin/sh -c 'printenv PATH'
echo direct-shell-path-done
echo direct-abs-cc-start
/bin/sh -c '/bin/cc /src/test_native.c -o /work/hello_abs'
if [ -f /work/hello_abs ]; then
    /work/hello_abs
    echo direct-abs-cc: PASS
else
    echo direct-abs-cc: FAIL
fi
echo direct-path-cc-start
/bin/sh -c 'cc /src/test_native.c -o /work/hello_path'
if [ -f /work/hello_path ]; then
    /work/hello_path
    echo direct-path-cc: PASS
else
    echo direct-path-cc: FAIL
fi
echo direct-cwd-cc-start
cd /work
/bin/sh -c 'cc test_native.c -o hello_cwd'
if [ -f /work/hello_cwd ]; then
    /work/hello_cwd
    echo direct-cwd-cc: PASS
else
    echo direct-cwd-cc: FAIL
fi
cd /work
export ORTHOS_SH_DEBUG=1
make
if [ ! -f /work/hello ]; then
    echo "ERROR: /work/hello not found"
    exit 1
fi
./hello
echo "native-make: PASS"
echo native-make-end
