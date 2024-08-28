#!/bin/bash
cd /vpp
make UNATTENDED=y PLATFORM=vpp install-dep && make UNATTENDED=y PLATFORM=vpp -j4 build pkg-deb
echo ret:$?
