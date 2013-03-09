#!/usr/bin/python
#
# Test script for wpaspy
# Copyright (c) 2013, Jouni Malinen <j@w1.fi>
#
# This software may be distributed under the terms of the BSD license.
# See README for more details.

import os
import time
import wpaspy

wpas_ctrl = '/var/run/wpa_supplicant'

def wpas_connect():
    ifaces = []
    if os.path.isdir(wpas_ctrl):
        try:
            ifaces = [os.path.join(wpas_ctrl, i) for i in os.listdir(wpas_ctrl)]
        except OSError, error:
            print "Could not find wpa_supplicant: ", error
            return None

    if len(ifaces) < 1:
        print "No wpa_supplicant control interface found"
        return None

    for ctrl in ifaces:
        try:
            wpas = wpaspy.Ctrl(ctrl)
            return wpas
        except wpactrl.error, error:
            print "Error: ", error
            pass
    return None


def main():
    print "Testing wpa_supplicant control interface connection"
    wpas = wpas_connect()
    if wpas is None:
        return
    print "Connected to wpa_supplicant"
    print wpas.request('PING')

    mon = wpas_connect()
    if mon is None:
        print "Could not open event monitor connection"
        return

    mon.attach()
    print "Scan"
    print wpas.request('SCAN')

    count = 0
    while count < 10:
        count += 1
        time.sleep(1)
        while mon.pending():
            ev = mon.recv()
            print ev
            if 'CTRL-EVENT-SCAN-RESULTS' in ev:
                print 'Scan completed'
                print wpas.request('SCAN_RESULTS')
                count = 10
                pass


if __name__ == "__main__":
    main()