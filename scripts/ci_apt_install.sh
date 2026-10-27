#!/bin/bash
# CI only: apt-get install "$@" on a GitHub runner, surviving a crawling mirror.
# The runners fetch from azure.archive.ubuntu.com (mirror+file:/etc/apt/apt-mirrors.txt),
# which sometimes drops to a few dozen KB/s and held builds for 40+ minutes. Each
# try gets 10 minutes per apt-get call; after a failed or stalled try we switch to
# archive.ubuntu.com and go again, three tries in all.

APT_OPTS=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30)

for try in 1 2 3; do
    if sudo timeout 600 apt-get "${APT_OPTS[@]}" update &&
       sudo timeout 600 apt-get "${APT_OPTS[@]}" install -y "$@"; then
        exit 0
    fi
    if [ "$try" = 3 ]; then
        echo "apt-get failed three times" >&2
        exit 1
    fi
    echo "apt-get try $try stalled or failed; retrying from archive.ubuntu.com"
    sudo dpkg --configure -a || true
    echo "http://archive.ubuntu.com/ubuntu/" | sudo tee /etc/apt/apt-mirrors.txt > /dev/null
    sudo sed -i 's|http://azure\.archive\.ubuntu\.com|http://archive.ubuntu.com|g' \
        /etc/apt/sources.list.d/*.sources /etc/apt/sources.list 2> /dev/null || true
    sleep 10
done
