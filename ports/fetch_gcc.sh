#!/bin/bash
set -e

# GCC 4.9.4 and dependencies versions
GCC_VER=4.9.4
GMP_VER=6.1.0
MPFR_VER=3.1.4
MPC_VER=1.0.3

MIRROR=https://ftp.gnu.org/gnu

cd ports

echo "Downloading GCC ${GCC_VER}..."
curl -O ${MIRROR}/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.gz

echo "Downloading dependencies..."
curl -O ${MIRROR}/gmp/gmp-${GMP_VER}.tar.bz2
curl -O ${MIRROR}/mpfr/mpfr-${MPFR_VER}.tar.bz2
curl -O ${MIRROR}/mpc/mpc-${MPC_VER}.tar.gz

echo "Extracting GCC..."
tar -xzf gcc-${GCC_VER}.tar.gz

echo "Extracting dependencies inside GCC tree..."
cd gcc-${GCC_VER}
tar -xjf ../gmp-${GMP_VER}.tar.bz2
mv gmp-${GMP_VER} gmp
tar -xjf ../mpfr-${MPFR_VER}.tar.bz2
mv mpfr-${MPFR_VER} mpfr
tar -xzf ../mpc-${MPC_VER}.tar.gz
mv mpc-${MPC_VER} mpc

echo "GCC source tree is ready in ports/gcc-${GCC_VER}"
