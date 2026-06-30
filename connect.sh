sudo nmcli device wifi connect "wifinp"
sudo nmcli connection modify "wifinp" ipv4.route-metric 50
sudo nmcli connection up "wifinp"
