#!/bin/sh

sudo /etc/init.d/squid stop
sudo /etc/init.d/squid start
sudo rm -f /tmp/write_file*
sudo curl http://vcoblitz-cmi.cs.princeton.edu/write_file.conf > /tmp/write_file.conf
sudo curl http://vcoblitz-cmi.cs.princeton.edu/i386/write_file > /tmp/write_file
sudo chmod +x /tmp/write_file
sudo /tmp/write_file -v testvolume-jcnelson@cs.princeton.edu -s abcdef -u UG-`hostname` -p sniff -m https://syndicate-metadata.appspot.com/ -c /tmp/write_file.conf
