#!/bin/bash
sudo touch /etc/docker/daemon.json
sudo chown ubuntu:ubuntu /etc/docker/daemon.json
sudo cat <<EOT > /etc/docker/daemon.json
{
    "ipv6": true,
    "fixed-cidr-v6": "2001:3984:3989::/64"
}
EOT
sudo chown root:root /etc/docker/daemon.json
sudo systemctl restart docker
