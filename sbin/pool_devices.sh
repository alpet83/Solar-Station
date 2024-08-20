#!/bin/bash
cd /home/alpet/solar
TS=`date`
ntpdate pool.ntp.org
source ./venv/bin/activate
nohup /usr/sbin/pv1800_ctrl.py > /tmp/pv1800_ctrl.log 2>> /tmp/pv1800_ctrl.err &
/usr/sbin/jkbms_ctrl.py > /tmp/jkbms_ctrl.log 2>> /tmp/jkbms_ctrl.err
cp -f  ~/root/inverter*.log /var/www/html
# await for some data saved
sleep 5
ps x | grep php > /tmp/php.lst

if [ $(grep -c "mqtt-client.php" /tmp/php.lst) -eq 1 ];
then 
 echo "$TS. #OK: mqtt-client already started " > /tmp/mqtt_client.nfo
else
 php mqtt-client.php /etc/MQTT_creds > /tmp/mqtt-client.log
fi

