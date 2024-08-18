#!/usr/bin/php
<?php
  require_once('lib/common.php');
  require_once('lib/esctext.php');

  $host = file_get_contents('/etc/hostname');
  $host = trim($host);

  function send_event($tag, $evt, $value = 0, $flags = 0)
  {
      global $host;
      $telebot_srv = trim(file_get_contents('/etc/telebot'));
      //  ts,host,tag,event,flags,value
      $evt = str_ireplace("\n", '\n', $evt);
      $data = array('event' => $evt);
      $url = "$telebot_srv/reg_event.php?tag=$tag&host=$host&value=$value&flags=$flags";
      $result = curl_http_request($url, $data);
      log_cmsg("~C01#SEND_EVENT($evt)~C00:\n\t~C92 $result~C00");
      return $result;
  }
   

  $dt = new DateTime(); 
  $ts = date('Y-m-d H:i:s');
  $tm = date("H:i");
  
  $json = file_get_contents('/tmp/jkbms_last.json');
  $obj = json_decode($json);
  if (is_object($obj) && isset($obj->ts)) 
     log_msg("#DBG: processing data from ".$obj->ts);
  else 
     die ("#FATAL: failed decode $json\n");

  $dtd = new DateTime($obj->ts);
  $diff = $dt->diff($dtd);
  $elps = $diff->h * 60 + $diff->m;
  if ($elps > 10) {
    send_event('ALERT', "BMS stats outdated for $elps minutes");
    die ("#FATAL: data elapsed $elps minutes");
  }    

  $v_min = 4;
  $v_max = 0;    
  $v_avg = 0;
  $count = 0;

  foreach ($obj->cells->voltages as $i => $v) {
    if ($v_min > $v) $v_min = $v;
    if ($v_max < $v) $v_max = $v;
    $v_avg += $v;
    $count ++;
  }
  $info = sprintf("voltage = %.1fV, current = %.1fA, SoC = %.1f%%\n", $obj->battery_voltage, $obj->battery_current, $obj->battery_SoC);

  if ($count > 0) {
    $v_avg /= $count;
    $info .= sprintf("cells voltage [min = %.3fV, max = %.3fV, average = %.3fV]\n", $v_min, $v_max, $v_avg);    
    if ($v_min < 2.7)
       send_event('ALERT', "Low voltage on cell = $v_min");
    if ($v_min > 3.6)
       send_event('ALERT', "High voltage on cell = $v_max");
  }
  

  log_msg("#INFO: $info");

  if (abs($obj->battery_current) > 50) {
    log_msg("#WARN: high current detected, sending event");
    send_event('ALERT', "Abnormal high current on {$obj->device_id} = {$obj->battery_current}");
  }
  $bpow = $obj->battery_voltage * $obj->battery_current;
  $json = file_get_contents('/tmp/inverter_cfg.json');
  $inv = json_decode($json);
  if (is_object($inv) && isset($inv->config)) {
    $cfg = $inv->config;
    if (0 == $cfg->ongrid_switch && $v_min <= 3.15) {
       $event = "Battery near discharge (v_min = $v_min, SoC = {$obj->battery_SoC}%, power = $bpow), switching invertor ON-Grid";
       send_event('WARN', $event);
       file_add_contents('/dev/kmsg', $event);
       file_put_contents('/root/inverter_cmd.lst', '20105=>1');
    }
    
    if (1 == $cfg->ongrid_switch && ($v_min >= 3.3 && $bpow >= 900 || $v_min > 3.35)) {
       $event = "Battery precharged (v_min = $v_min, SoC = {$obj->battery_SoC}%, power = $bpow, switching invertor OFF-Grid";
       send_event('WARN', $event); 
       file_add_contents('/dev/kmsg', $event);
       file_put_contents('/root/inverter_cmd.lst', '20105=>0');
    }
  }

  $H = date('H') * 1;
  $M = date('i') * 1;  
  if (0 == $M && $H >= 18 && $H < 23) {
    send_event('REPORT', sprintf("Status for {$obj->device_id}: %s", $info));
  }  

