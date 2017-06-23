#!/bin/bash

set -e

PROJECT=cfq/cfqd
GIT_SHA1=`git rev-parse --short HEAD`

mkdir -p build/docker/

cp Builds/Docker/rippled.cfg build/rippled Builds/Docker/entry.sh build/docker/
chmod +x build/docker/entry.sh
cp doc/cfqd.logrotate build/docker/rippled.logrotate
chmod 644 build/docker/rippled.logrotate
cp Builds/Docker/Dockerfile-catalog build/docker/Dockerfile

docker build -t $PROJECT:$GIT_SHA1 build/docker/
docker tag $PROJECT:$GIT_SHA1 $PROJECT:latest

if [ -n "$BRANCH" ]; then
  docker tag $PROJECT:latest $PROJECT:$BRANCH
fi
