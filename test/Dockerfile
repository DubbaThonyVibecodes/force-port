# Wine in a box, only used to run the test on Linux. Not needed on Windows.
# xvfb/xdotool/imagemagick: a fake screen, fake key presses and screenshots,
# for testing the dialog.
FROM alpine:3.22
RUN apk add --no-cache wine iproute2 xvfb xdotool imagemagick font-dejavu
ENV WINEDEBUG=-all WINEPREFIX=/wine
# Create the wine prefix once at build time, it takes a while.
RUN wineboot --init && wineserver --wait
