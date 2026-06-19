#!/bin/bash

INTERFACE="enP8p1s0"
GATEWAY="147.127.120.2"
FIRST_ADDR="94"

HOSTNAME=$(hostname)
X="${HOSTNAME: -1}"

WORKER_IP="147.127.121.$((FIRST_ADDR + X))"

nmcli con delete "static-$INTERFACE"
nmcli con add type ethernet ifname $INTERFACE con-name "static-$INTERFACE" ipv4.addresses $WORKER_IP/24 ipv4.gateway $GATEWAY ipv4.method manual
nmcli con up "static-$INTERFACE"
echo "Configuration réussie pour le worker $HOSTNAME, adresse IP attribuée : $WORKER_IP"

sed -i '/orin-nano-/d' /etc/hosts
for i in {0..5}; do
    echo "147.127.121.$((FIRST_ADDR + i)) orin-nano-$i" >> /etc/hosts
done
echo "Configuration de la résolution des autres workers faite."
