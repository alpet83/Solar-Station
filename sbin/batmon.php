#!/usr/bin/php
<?php
  require_once('lib/common.php');
  require_once('lib/esctext.php');

  $host = file_get_contents('/etc/hostname');
  $host = trim($host);

  $offgrid_start = 3.32;
  if (isset($argv[1]))
     $offgrid_start = floatval($argv[1]);

  function send_event($tag, $evt, $value = 0, $flags = 0)
  {
      global $host;
      $telebot_srv = trim(file_get_contents('/etc/telebot'));
      //  ts,host,tag,event,flags,value
      $evt = str_ireplace("\n", '\n', $evt);
      $data = array('event' => $evt);
      $url = "$telebot_srv/reg_event.php?tag=$tag&host=$host&value=$value&flags=$flags";
      $result = curl_http_request($url, $data);
      if (strpos($result, '#ERROR') !== false)
        fprintf(STDERR, "send_event failed: $result");
      else
        log_cmsg("~C01#SEND_EVENT($evt)~C00:\n\t~C92 $result~C00");
      return $result;
  }
   

  $dt = new DateTime(); 
  $ts = date('Y-m-d H:i:s');
  $tm = date("H:i");
  
  $obj = file_load_json('/tmp/jkbms_last.json');  
  if (is_object($obj) && isset($obj->ts)) 
     log_msg("#DBG: processing data from ".$obj->ts);
  else 
     die ("#FATAL: failed decode BMS data: \n".var_export($obj, true));

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
  $ivc = file_load_json('/tmp/inverter_cfg.json');
  $inv = file_load_json('/tmp/inverter_last.json');
  $accum = file_load_json('/tmp/inverter_accum.json');  
  $epever = file_load_json('/tmp/epever_dump.json');
  $delta = $accum->load_power - $accum->buy_power + $accum->sell_power;
  $info = sprintf("JKBMS voltage = %.1fV, current = %.1fA, SoC = %.1f%%\n", $obj->battery_voltage, $obj->battery_current, $obj->battery_SoC);

  if ($count > 0) {
    $v_avg /= $count;
    $info .= sprintf("\tcells voltage [min = %.3fV, max = %.3fV, average = %.3fV], energy = $delta kWh\n", $v_min, $v_max, $v_avg);    
    if ($v_min < 2.7)
       send_event('ALERT', "Low voltage on cell = $v_min");
    if ($v_min > 3.6)
       send_event('ALERT', "High voltage on cell = $v_max");
  }
  
  $total_current = 0;
  if (property_exists($epever, 'battery_current'))
     $total_current = $epever->battery_current;
  else
      log_msg("~C91#WARN:~C00 Epever data may be wrong: %s", json_encode($epever));
  if (is_object($inv) && is_object($inv->PV_charger)) {
     $total_current += $inv->PV_charger->chg_curr;
     log_cmsg("~C93#DBG:~C00 total chargers current = %.1fA", $total_current);
  }    

  log_msg("#INFO: $info");

  if (abs($obj->battery_current) > 50) {
    log_msg("#WARN: high current detected, sending event");
    send_event('ALERT', "Abnormal high current on {$obj->device_id} = {$obj->battery_current}");
  }
  $bpow = $obj->battery_voltage * $obj->battery_current;
  
  $H = date('H') * 1;
  $M = date('i') * 1;  
  $chg_pow = $obj->battery_voltage * $total_current; 
  $pow_min = 640;
  $inv_pow = 0;
  
  if (isset($inv->power)) {
     $pow_min = max($pow_min, $inv->power->load * 0.97); // estimated in 1 hour PV power will be enough
     $inv_pow = $inv->power->inverter;
  }   

  $on_grid = 1;
  define('ON_GRID_FILE', '/tmp/on_grid.cntr');
  if (file_exists(ON_GRID_FILE))
     $on_grid = file_read_int(ON_GRID_FILE);
  $prev = $on_grid;

  if (is_object($ivc) && isset($ivc->config)) {
    $cfg = $ivc->config;
    // checking BMS as ref, increase counter while voltage low
    if (0 == $cfg->ongrid_switch && $v_min <= 3.21) {
       $on_grid = min( 5, $on_grid + 1);
       log_cmsg("~C91#DISCHARGED:~C00 countdown to ON-Grid");
    }   
    elseif ($on_grid > 0) $on_grid --; // recovery average
       
    if ($prev < $on_grid && $on_grid == 5) {       
       $event = "Battery near discharge (v_min = $v_min, SoC = {$obj->battery_SoC}%, power chg/inv = $chg_pow/$inv_pow, energy = $delta kWh), switching invertor ON-Grid";
       $res = send_event('WARN', $event);
       log_cmsg("#SEND_EVENT: %s", $res);
       file_add_contents('/dev/kmsg', $event);
       file_put_contents('/root/inverter_cmd.lst', '20105=>1');
    } elseif (0 == $cfg->ongrid_switch)
      log_msg("#OFF_GRID: (v_min = $v_min, SoC = {$obj->battery_SoC}% charger power = $chg_pow W - discharge, counter = $on_grid");
    
    if (1 == $cfg->ongrid_switch && ($v_min >= $offgrid_start && $chg_pow >= $pow_min || $v_min >= 3.5)) {
       log_cmsg("~C92#CHARGED:~C00 countdown to OFF-Grid");
       $on_grid = max(-5 , $on_grid - 1);
    }   
    elseif ($on_grid < 0) $on_grid ++;
       
    if ($prev > $on_grid && $on_grid == -5) { //  && $H < 10 (hours range - debug)
       $event = "Battery precharged (v_min = $v_min, SoC = {$obj->battery_SoC}%, power bat/total charge/inv = $bpow/$chg_pow/$inv_pow, energy = $delta kWh), switching invertor OFF-Grid";
       $res = send_event('WARN', $event); 
       log_msg("#NOTIFY: $res");
       file_add_contents('/dev/kmsg', $event); 
       file_put_contents('/root/inverter_cmd.lst', '20105=>0');
    } elseif (1 == $cfg->ongrid_switch)
       log_cmsg("~C97#ON_GRID:~C00 (v_min = %.3f, SoC = %.1f%% power bat/total charge/inv = %dW/%dW/%dW, - waiting power breakout %dW or cell voltage %.3fV >= 3.5V, counter = %d",
                             $v_min, $obj->battery_SoC, $bpow, $chg_pow, $inv_pow, $pow_min, $offgrid_start, $on_grid);
  }

  if ($prev != $on_grid) {
    file_put_contents(ON_GRID_FILE, $on_grid);
    log_cmsg("~C93#ON_GRID:~C00 counter changed %d => %d", $prev, $on_grid);
  }  

  if (0 == $M && $H >= 15 && $H <= 21) {
    send_event('REPORT', sprintf("Status for {$obj->device_id}: %s", $info));
  }  

