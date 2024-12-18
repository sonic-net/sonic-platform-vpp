#!/bin/bash
cd /vpp
make UNATTENDED=y PLATFORM=vpp install-dep && make UNATTENDED=y PLATFORM=vpp -j4 pkg-deb
echo ret:$?
