#!/bin/sh

asterisk -rx "core restart now"

sleep 4

asterisk -rx "logger rotate"

echo "restart done!"