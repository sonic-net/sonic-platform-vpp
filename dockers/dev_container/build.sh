#!/bin/bash

ROOT=$(git rev-parse --show-toplevel)
cp Dockerfile $ROOT/build/sonic-buildimage/target/debs/bullseye
cd $ROOT/build/sonic-buildimage/target/debs/bullseye
docker build -f "$ROOT/build/sonic-buildimage/target/debs/bullseye/Dockerfile" -t sonic-vpp-dev:latest "$ROOT/build/sonic-buildimage/target/debs/bullseye"
