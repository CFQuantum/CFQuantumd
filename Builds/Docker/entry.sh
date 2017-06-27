#!/bin/bash

echo "====[ Generating rippled.cfg ]==========="
cat > /opt/ripple/conf/rippled.cfg <<- EOF

echo "====[ rippled.cfg ]======================"
cat /opt/ripple/conf/rippled.cfg
echo "========================================="
/opt/ripple/bin/rippled --conf /opt/ripple/conf/rippled.cfg --quorum 2
