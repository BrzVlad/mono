#!/bin/sh

git clean -xfd
./autogen.sh CFLAGS="-O2 -g3" --disable-boehm --disable-libraries --without-shared_mono --with-sgen-default-concurrent=yes --prefix=/home/vbrezae/Xamarin/installation/
make get-monolite-latest
make
make -j1 install

#while true; do
#	make -j1 -C mono/tests clean
#	make -j1 -C mono/tests check
#
#	make -j1 -C mcs clean
#	make -j1 -C mcs
#
#	make -j1 -C mcs/class/corlib clean
#	make -j1 -C mcs/class/corlib run-test
#	make -j1 -C mcs/class/System.Windows.Forms clean
#	make -j1 -C mcs/class/System.Windows.Forms run-test
#	make -j1 -C mcs/class/System.Data clean
#	make -j1 -C mcs/class/System.Data run-test
#done
