set -e

if [ -z "$GIT_SHA1" ]; then
  GIT_SHA1=`git rev-parse --short HEAD`
fi
if [ -z "$PROJECT" ]; then
  PROJECT=cfq/cfqd
fi

if [ -z "$DOCKER_EMAIL" -o -z "$DOCKER_USERNAME" -o -z "$DOCKER_PASSWORD" ];then
  echo "Docker credentials are not set. Can't login to docker, no containers will be pushed."
  exit 0
fi

if [ -n "$CIRCLE_PR_NUMBER" ]; then
  echo "Not pushing results of a pull request build."
  exit 0
fi

docker login -e $DOCKER_EMAIL -u $DOCKER_USERNAME -p $DOCKER_PASSWORD
docker push $PROJECT:$GIT_SHA1
if [ -n "$BRANCH" ]; then
  docker push $PROJECT:$BRANCH
fi
docker push $PROJECT:latest
