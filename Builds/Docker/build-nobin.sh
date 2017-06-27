#!/bin/bash

set -e

PROJECT=cfq/cfqd-nobin
GIT_SHA1=`git rev-parse --short HEAD`

mkdir -p build/docker/

cp Builds/Docker/*.cfg build/docker/
cp doc/cfqd.logrotate build/docker/rippled.logrotate
chmod 644 build/docker/rippled.logrotate
cp Builds/Docker/Dockerfile-testnet build/docker/Dockerfile

docker build -t $PROJECT:$GIT_SHA1 build/docker/
docker tag $PROJECT:$GIT_SHA1 $PROJECT:latest

if [ -n "$BRANCH" ]; then
  docker tag $PROJECT:latest $PROJECT:$BRANCH
fi
