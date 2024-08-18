<html>
 <head>
<?php
 $ip = $_SERVER['REMOTE_ADDR'];
 $host = file_get_contents('/etc/hostname');
 echo "<!-- Hello $ip, this is $host -->\n"; 
 $json = file_get_contents('/tmp/jkbms_last.json');
 $obj = json_decode($json);

 $sw_states = ['OFF', 'ON'];

 if (is_object($obj)) {
   printf("<title>%.1fA, %.1f%% SoC Battery</title>", $obj->battery_current, $obj->battery_SoC);
 }
 else
   die("<body>#FATAL: can't decode $json");
   
?>
 <style type=text/css>
   td {
      padding-left: 10pt;
      padding-right: 7pt;
   }
 </style>
 <body>
 <h2>JKBMS status:</h2>
 <table border=1 style='border-collapse:collapse'>
 <thead> 
   <tr><th>Param<th>Value
 </thead>
 <tbody>
<?php
 $par = $obj->bms_params;
 $chg_st = $par->chg_MOS_switch > 0 ? 'ON' : 'OFF';
 $dis_st = $par->dis_MOS_switch > 0 ? 'ON' : 'OFF';
 $bal_st = $par->balance_switch > 0 ? 'ON' : 'OFF';
 $flags = $obj->battery_status_info;
 $chg_st .= ' => '.($flags & 1 ? 'ON' : 'OFF');
 $dis_st .= ' => '.($flags & 2 ? 'ON' : 'OFF');
 $bal_st .= ' => '.($flags & 4 ? 'ON' : 'OFF');

 printf("\t<tr><td>Timestamp:<td>%s</tr>\n", $obj->ts);
 printf("\t<tr><td>Charge:<td>%s</tr>\n", $chg_st);
 printf("\t<tr><td>Discharge:<td>%s</tr>\n", $dis_st);
 printf("\t<tr><td>Balance:<td>%s</tr>\n",   $bal_st);
 printf("\t<tr><td>Voltage<td><b>%.1fV</b></tr>\n", $obj->battery_voltage);
 printf("\t<tr><td>Current<td><b>%.1fA</b></tr>\n", $obj->battery_current);
 printf("\t<tr><td>Battery SOC<td>%.1f%%</tr>\n", $obj->battery_SoC);
 printf("\t<tr><td>MOS temperature<td>%.1f°C</tr>\n", $obj->MOS_temp);
 printf("\t<tr><td>Battery T1<td>%.1f°C</tr>\n", $obj->battery1_temp);
 printf("\t<tr><td>Battery T2<td>%.1f°C</tr>\n", $obj->battery2_temp);
 printf("\t<tr><td>Battery status<td>b%4b</tr>\n", $flags); 
?>
 </tbody>
 </table>
 <h2>Battery cells</h2>
 <table border=1 style='border-collapse:collapse'>
 <thead> 
   <tr><th>Col 1<th>Col 2
 </thead>
 <tbody>
 <?php
  function diff_color($diff, $max_diff) {
    if (0 == $max_diff) return 'white';
    $diff = $diff * 63 / $max_diff;
    $adiff = abs($diff);
    if ($diff != 0)
       return sprintf('#%02x%02xb0', min(0xff, 0xff + $diff), min(0xff, 0xff - $diff));
  }

  $cvs = $obj->cells->voltages;
  $i_max = 0;
  $i_min = 0;
  $v_min = 4;
  $v_max = 1;
  $v_avg = 0;
  if ($obj->cells->count >= 16)
  {
    foreach ($cvs as $i => $v) {
      if ($v < $v_min) {
        $v_min = $v;
        $i_min = $i;
      }
      if ($v > $v_max) {
        $v_max = $v;
        $i_max = $i;
      }
      $v_avg += $v; 
    }
    $v_avg /= $obj->cells->count;
    $v_mid = ($v_min + $v_max) / 2;
    $range = abs($v_max - $v_min);

    for ($i = 0; $i < 8; $i ++) {
      $lid = ($i + 9);
      $rid =  (8 - $i);
      $vl = $cvs->$lid;
      $vr = $cvs->$rid;
      $ls = '';
      $rs = '';
      if ($lid == $i_min || $lid == $i_max)
          $ls .= 'font-weight: bold;';
      if ($rid == $i_min || $rid == $i_max)
          $rs .= 'font-weight: bold;';
      $ls .= sprintf('background-color: %s;', diff_color($vl - $v_avg, $range));
      $rs .= sprintf('background-color: %s;', diff_color($vr - $v_avg, $range));
      printf("\t<tr><td style='$ls'>$lid = %.3fV</td>", $vl);
      printf("<td style='$rs'>$rid = %.3fV</td></tr>\n", $vr);   
    }
    printf("<tr><td>Average:<td>%.3fV</tr>\n", $v_avg);
  }  
 ?>
 </tbody>
 </table> 
 <?php  
  $json = file_get_contents('/tmp/inverter_cfg.json');
  $inv = json_decode($json);
  if (is_object($inv) && isset($inv->machine_id)) 
     print("<h2>$inv->machine_id info</h2>\n");
  else 
     die("#ERROR: Can't decode $json/not exists inverter_cfg.json\n");

  $json = file_get_contents('/tmp/inverter_last.json');
  $obj = json_decode($json);
  if (is_object($obj) && isset($obj->ts)) 
     print("Updated {$obj->ts}\n");
  else
    die("#ERROR: Can't decode $json/not exists inverter_last.json\n");

  $now = new DateTime();
  $dtd = new DateTime($obj->ts);
  $diff = $now->diff($dtd);   
  $elps = $diff->h * 60 + $diff->m;
  if ($elps > 10) 
      die ("#FATAL: data elapsed $elps minutes");    

 ?>
 
 <table border="1" style='border-collapse:collapse'>
 <tr><th>Param</th><th>Voltage</th><th>Current</th><th>Power</th>
 <?php
  $pvc = false;
    
  if (isset($obj->PV_charger)) {
    $pvc = $obj->PV_charger;
    printf("\t<tr><td>PV<td>%.1fV<td>-<td>\n", $pvc->PV_volt);
    printf("\t<tr><td>Battery<td>%.1fV<td>-<td>\n", $pvc->batt_volt);
    $chv = $pvc->batt_volt;
    if (abs($pvc->chg_curr) > 0.1) 
        $chv = abs($pvc->chg_power / $pvc->chg_curr);
    printf("\t<tr><td>Charger<td>%.1fV<td>%.1fA<td>%.0fW\n", $chv, $pvc->chg_curr, $pvc->chg_power);        
  }
  $volt = $obj->voltages; 
  $curr = $obj->current;
  $powr = $obj->power;
  printf("\t<tr><td>Bus     <td>%.1fV<td>n/a<td>n/a\n", $volt->bus); 
  printf("\t<tr><td>Grid    <td>%.1fV<td>%.1fA<td>%.0fW\n", $volt->grid,     $curr->grid, $powr->grid); 
  printf("\t<tr><td>Inverter<td>%.1fV<td>%.1fA | %.1fA<td>%.0fW\n", $volt->inverter, $curr->inverter, $curr->control, $powr->inverter);
  printf("\t<tr><td>Load<td>^^^<td>%.1fA<td>%.0fW\n", $curr->load, $powr->load);
     // printf("<td>Inverter<td>%.1fV", $obj->voltages->inverter);
 ?> 
 </table>
 <table border="1" style='border-collapse:collapse'>
 <tr><th>Param</th><th>Value</th> 
 <?php   
   function format_relays($data, $map) {
      global $sw_states;
      $result = [];
      foreach ($map as $prefix => $key) {
        if (isset($data->$key))
            $result []= $prefix.':'.$sw_states[$data->$key];
      }
      return implode(', ', $result);
   }

   function format_set(array $list, int $bitset) {
     $result = [];
     foreach ($list as $i => $desc)                   
     if ($bitset & (1 << $i))
         $result [] = $desc; 
     return implode(', ', $result);
   }

   if ($pvc) {
      $ws = ['Init.', 'Selftest', 'Work', 'Stop'];
      print("<tr><th colspan=2>PV CHARGER DATA");
      printf("<tr><td>Device workstate<td>%s\n", $ws[$pvc->workstate]);
      $cs = ['Stop', 'Absorb charge', 'Float charge', 'EQ charge'];
      printf("<tr><td>Charging state<td>%s\n", $cs[$pvc->charging_state]);      
      $mps = ['Stop', 'MPPT', 'Current limiting'];
      printf("<tr><td>MPPT state<td>%s\n", $mps[$pvc->MPPT_state]);
      printf("<tr><td>Radiator temp.<td>%.1f°C\n", $pvc->rad_temp);
      printf("<tr><td>External temp.<td>%.1f°C\n", $pvc->ext_temp);
      $rinfo = format_relays($pvc, ['Batt'=>'batt_relay', 'PV'=>'PV_relay']);      
      print("<tr><td>Relays<td>$rinfo\n");

      if ($pvc->err_msg > 0) {
          $err_map = array('Hardware fault', 'Overcurrent', 'Current sensor failed', 'Overheat', 
                           'PV overvoltage', 'PV undervoltage', 'Battery high voltage', 'Battery low voltage',
                           'Current is uncontrollable', 'Parameter error');
          
          printf("<tr><td>Errors<td>%s\n", format_set($err_map, $pvc->err_msg));
      }    
      if ($pvc->warn_msg == 1) 
          printf("<tr><td>Warning<td>Fan Error\n");
      

   }
   print("\t<tr><th colspan=2>INVERTER DATA\n");
   $sts = ['PowerOn', 'SelfTest', 'OffGrid', 'Grid-Tie', 'ByPass', 'Stop', 'Grid charging'];
   printf("\t<tr><td>Workstate<td>%s\n", $sts[$obj->inv_workstate]);
   printf("\t<tr><td>Load usage<td>%.0f%%\n", $obj->load_percent);
   printf("\t<tr><td>Inverter freq.<td>%.1fHz\n", $obj->inv_freq);
   printf("\t<tr><td>Grid freq.<td>%.1fHz\n", $obj->grid_freq);
   $t = $obj->temp_sensors;   
   printf("\t<tr><td>AC radiator temp<td>%.0f°C\n", $t->AC_radiator);
   printf("\t<tr><td>DC radiator temp<td>%.0f°C\n", $t->DC_radiator);
   printf("\t<tr><td>Transformer temp<td>%.0f°C\n", $t->transformer);   
   $rinfo = format_relays($obj->relays, ['I'=>'inverter', 'G'=>'grid', 'L'=>'load', 'N'=>'neutral', 'E'=>'earth']);
   print("\t<tr><td>Relays<td>$rinfo\n");
   $errs_1 = file('inv_errors_1.txt');
   $errs_2 = file('inv_errors_2.txt');
   $msgs = $obj->messages;
   if ($msgs->err_1 > 0) 
      printf("<tr><td>Errors 1<td>%s\n", format_set($errs_1, $pvc->err_1));
   if ($msgs->err_2 > 0) 
      printf("<tr><td>Errors 2<td>%s\n", format_set($errs_2, $pvc->err_2)); // TODO: copypaste must be optimized
   if ($msgs->err_3 > 0)  
      printf("<tr><td>Errors 3<td>b%b\n", $msgs->err_3); 

   $warns = file('inv_warns_1.txt');
   if ($msgs->warn_1 > 0) 
      printf("\t<tr><td>Warnings<td>%s\n", format_set($warns, $pvc->warn_1));
   if ($msgs->warn_2 > 0)  
      printf("\t<tr><td>Warnings<td>b%b\n", $msgs->warn_2); 


   $arrow_map = ['PV+', 'Load+', 'Batt+', 'Grid+', 'PV=>I', 'I=>L'];
   $info = format_set($arrow_map, $obj->arrow_flags & 0x0f);
   printf("\t<tr><td>Availability<td>%s</tr>\n", $info);
   $dir_desc = ['‼', '►', '◄', '◄+►'];

   $info = 'Inverter';
   if ($obj->arrow_flags & 0x10)
      $info = sprintf("PV %s Inverter", $dir_desc[1]);
   if ($obj->arrow_flags & 0x20)
      $info = sprintf("$info %s Load", $dir_desc[1]);
   if (strlen($info) > 7)
       printf("\t<tr><td colspan=2>%s</tr>\n", htmlentities ($info));

   $info = '';
   $dir = ($obj->arrow_flags >> 6) & 3;   
   $dir = htmlentities ($dir_desc[$dir]);
   print("<tr><td colspan=2>Inverter $dir Battery\n");
   $dir = ($obj->arrow_flags >> 8) & 3;
   $dir = htmlentities ($dir_desc[$dir]);
   print("<tr><td colspan=2>Inverter $dir Grid\n");  

 ?>
 </table>
<pre>
<?php
  // print_r($obj); 
?>

