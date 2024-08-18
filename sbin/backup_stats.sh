#!/bin/sh
DATE=`date +'%Y-%m-%d'`
sleep 15
gzip --best -c /tmp/jkbms_stats.json > /backup/jkbms/stats_$DATE.json.gz
gzip --best -c /tmp/inverter_last.json > /backup/inverter/stats_$DATE.json.gz
#rm /tmp/jkmbs_stats.json

