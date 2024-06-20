#!/bin/bash


docker run -u $(id -u):$(id -g) -v $HOME/workspace/old/build/sonic-buildimage/src/sonic-sairedis:/sonic-sairedis -it sonic-vpp-dev bash
