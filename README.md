WIP

UDP port forwarding:

  % sh -c "udpstream -r 127.0.0.1 53 <&1 | ssh <Remote> udpstream -s 8.8.8.8 53 >&0"

Forward "8.8.8.8 53/udp" accessed from the remote host to localhost through a SSH.

## Options

* -s sender mode

* -r receiver mode

