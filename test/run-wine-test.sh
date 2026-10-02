#!/bin/sh
# Runs test/testapp.c under Wine (in Docker) through portrebind and checks that
# its sockets land on the forced address. Run via "make test".
#
# The container gets a second address, 10.99.0.2. The test app asks for the
# container's normal address (the "wrong" one); portrebind must move it.
set -e
cd "$(dirname "$0")/.."

docker build -q -t portrebind-wine-test test >/dev/null

docker run --rm --cap-add NET_ADMIN -v "$PWD/dist:/dist:ro" portrebind-wine-test sh -c '
exec 2>/dev/null
ip addr add 10.99.0.2/24 dev eth0
WRONG=$(ip -4 -o addr show dev eth0 | head -1 | sed "s/.*inet \([0-9.]*\).*/\1/")
mkdir /t && cp /dist/* /t/ && cd /t
export TESTAPP_OUT="Z:\\t\\app.txt"
failed=0

# check <name> <output> <line that must be in it>   (runs of spaces are equal)
check() {
    if echo "$2" | tr -s " " | grep -qF -- "$(echo "$3" | tr -s " ")"; then
        echo "  ok    $1"
    else
        echo "  FAIL  $1 - expected: $3"
        echo "$2" | sed "s/^/        | /"
        failed=1
    fi
}

set_ini() { sed -i "s|^$1 *=.*|$1 = $2|" portrebind.ini; }

# run <wine arguments>: prints what the launcher and the test app had to say
run() {
    rm -f app.txt
    timeout 60 wine "$@" < /dev/null
    wineserver --wait
    cat app.txt
}

set_ini ask 0

echo "without portrebind (the bug)"
out=$(run testapp.exe $WRONG)
check "explicit bind lands on wrong address" "$out" "udp bind to given ip:        $WRONG"

echo "launcher, exact ip"
set_ini ip 10.99.0.2
out=$(run portrebind.exe testapp.exe $WRONG)
check "explicit udp bind moved"        "$out" "udp bind to given ip:        10.99.0.2"
check "udp bind to 0.0.0.0 moved"      "$out" "udp bind to 0.0.0.0:         10.99.0.2"
check "loopback left alone"            "$out" "udp bind to 127.0.0.1:       127.0.0.1"
check "explicit tcp bind moved"        "$out" "tcp bind to given ip:        10.99.0.2"
check "tcp bind to 0.0.0.0 left alone" "$out" "tcp bind to 0.0.0.0:         0.0.0.0"
check "unbound sendto moved"           "$out" "udp sendto without bind:     10.99.0.2"
check "gethostbyname filtered"         "$out" "gethostbyname:                10.99.0.2"
check "getaddrinfo filtered"           "$out" "getaddrinfo:                  10.99.0.2"
check "GetAdaptersInfo filtered"       "$out" "GetAdaptersInfo:              10.99.0.2"
check "GetIpAddrTable filtered"        "$out" "GetIpAddrTable:               10.99.0.2 127.0.0.1"
[ ! -e portrebind.log ] && echo "  ok    no log file by default" ||
    { echo "  FAIL  log file written although log = 0"; failed=1; }

echo "launcher, subnet, game started by a launcher process"
set_ini ip 10.99.0.0/24
out=$(run portrebind.exe testapp.exe $WRONG --spawn)
check "child process patched too"      "$out" "udp bind to given ip:        10.99.0.2"

echo "game and args taken from the ini"
set_ini game "Z:\\\\t\\\\testapp.exe"
set_ini args $WRONG
out=$(run portrebind.exe)
check "explicit udp bind moved"        "$out" "udp bind to given ip:        10.99.0.2"

echo "rewrite_any = none, hide_other_ips = 0"
set_ini rewrite_any none
set_ini hide_other_ips 0
out=$(run portrebind.exe testapp.exe $WRONG)
check "explicit udp bind still moved"  "$out" "udp bind to given ip:        10.99.0.2"
check "udp bind to 0.0.0.0 left alone" "$out" "udp bind to 0.0.0.0:         0.0.0.0"
check "address list untouched"         "$out" "GetIpAddrTable:               $WRONG 10.99.0.2 127.0.0.1"
set_ini rewrite_any udp
set_ini hide_other_ips 1

echo "--wait: inject into an already running process"
rm -f app.txt
wine testapp.exe $WRONG --sleep &
app=$!
sleep 1
wine portrebind.exe --wait testapp.exe > /dev/null
wait $app; wineserver --wait
check "explicit udp bind moved"        "$(cat app.txt)" "udp bind to given ip:        10.99.0.2"

echo "address that does not exist, no screen to ask on"
set_ini ip 10.123.0.1
out=$(run portrebind.exe testapp.exe $WRONG)
check "launcher refuses"               "$out" "does not match any address of this machine"

# From here on there is a (fake) screen, so the dialog can show up.
Xvfb :1 -screen 0 1024x768x24 > /dev/null &
export DISPLAY=:1
sleep 1

echo "dialog: saved address is gone, pick the 2nd one, tick both boxes, Enter"
set_ini ask 1
comments=$(grep -c "^;" portrebind.ini)
rm -f app.txt
wine portrebind.exe testapp.exe $WRONG > launcher.txt &
launcher=$!
timeout 60 xdotool search --sync --name "^portrebind$" > /dev/null
sleep 2
xdotool key Down Tab space Tab space Return
wait $launcher; wineserver --wait
ini=$(tr -d " \r" < portrebind.ini)
check "game started on picked address" "$(cat app.txt)" "udp bind to given ip:        10.99.0.2"
check "address saved to ini"           "$ini" "ip=10.99.0.2"
check "dont-show-again saved to ini"   "$ini" "ask=0"
check "logging saved to ini"           "$ini" "log=1"
check "comments in ini survived"       "$(grep -c "^;" portrebind.ini)" "$comments"
check "log file written"               "$(cat portrebind.log)" "bind udp $WRONG:8086 -> 10.99.0.2:8086"

echo "dialog: not shown again"
out=$(run portrebind.exe testapp.exe $WRONG)
check "game started without asking"    "$out" "udp bind to given ip:        10.99.0.2"

echo "dialog: Cancel starts nothing"
set_ini ask 1
rm -f app.txt
wine portrebind.exe testapp.exe $WRONG > /dev/null &
launcher=$!
timeout 60 xdotool search --sync --name "^portrebind$" > /dev/null
sleep 2
xdotool key Escape
wait $launcher; wineserver --wait
[ ! -e app.txt ] && echo "  ok    game not started" || { echo "  FAIL  game started"; failed=1; }

[ $failed = 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $failed
'
