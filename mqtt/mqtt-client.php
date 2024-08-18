<?php
    require __DIR__ . '/../vendor/autoload.php';
    // require __DIR__ . '/../shared/config.php';
    
    require_once ("lib/common.php");
    require_once ("lib/esctext.php");

    use PhpMqtt\Client\ConnectionSettings;
    use PhpMqtt\Client\Exceptions\MqttClientException;
    use PhpMqtt\Client\MqttClient;
    # use Psr\Log\LogLevel;

    $server   = '192.168.16.55';
    $port     = 1883;
    $clientId = 'SolarStation';
    # $logger = new SimpleLogger(LogLevel::INFO);
    define('SENSOR_PATH', 'homeassistant/sensor/solar_station');
    define('SWITCH_PATH', 'homeassistant/switch/solar_station');

    $root = SENSOR_PATH;

    $mqtt = null;

    class MqttConfigFactory {
        public $proto;
        public $group = 'sensor';
        public $configs = array();
        public $dev_id = 'nope';

        public function __construct(string $proto_file, string $device_id) {            
            $this->dev_id = $device_id;
            $this->load($proto_file);
        } 

        public function set_device(?string $device_class, string $unit = '') {
            if ($device_class)
                $this->proto->device_class = $device_class;            
            
            $this->proto->device->identifiers = [$this->dev_id];

            $std_sens = [];
            $std_sens['current'] = 'A';
            $std_sens['energy'] = 'kWh';
            $std_sens['frequency'] = 'Hz';
            $std_sens['power'] = 'W';
            $std_sens['temperature'] =  '°C';
            $std_sens['state_of_charge'] = '%';
            $std_sens['voltage'] = 'V';           
            

            if (isset($std_sens[$device_class]))
                $unit = $std_sens[$device_class];            
            $this->proto->unit_of_measurement = $unit;
            return $this->proto;
        }
        public function load(string $proto_file, ?string $dev_id = null) {
            $this->group = str_replace('.json', '', $proto_file);
            $this->proto = file_load_json($proto_file);            
            if ($dev_id)
                $this->dev_id = $dev_id;
        }

        public function add(string $value_key, ?string $value_name = null, array $dev_props = []) {
            $fork = clone $this->proto;
            $fork->value_template = sprintf('{{ value_json.%s }}', $value_key);
            foreach ($dev_props as $key => $value)
              $fork->device->$key = $value;
            if ($value_name)
               $fork->name = $value_name; 
            else
               $fork->name = str_replace('_', ' ', $value_key);
            $prefix = $this->group;
            if (isset($fork->device_class))
                $prefix = $fork->device_class;

            $uid = strtolower($this->dev_id.$value_key);
            $fork->unique_id = $prefix.md5($uid);
            $this->configs[$value_key] = $fork;
            return $this;
        }

        public function publish(MqttClient $mqqt) {
            global $root;
            foreach ($this->configs as $key => $config) {
               $json = json_encode($config);               
               $path = strtolower("$root/$key/config");
               log_cmsg("~C97#PUB_CFG:~C00  $path : $json"); 
               $mqqt->publish($path, $json);
            }   
            $this->configs = [];
        }
    }


    $sws = ['OFF', 'ON'];
    $inv_cfg = null;

    try {
        $mqtt = new MqttClient($server, $port, $clientId, MqttClient::MQTT_3_1, null);
        $connectionSettings = (new ConnectionSettings)
            ->setUsername('script')
            ->setPassword('in271MQQT');

        log_cmsg("~C92 #DBG:~C00 trying connect...");
        $mqtt->connect($connectionSettings, true);
        $minute = date('i');


        if (1) {
            log_cmsg("~C92 #DBG:~C00 trying send test...");
            
            $factory = new MQTTConfigFactory('bms_sensor.json', 'JKBMS');
            $root = SENSOR_PATH.'_jkbms';
            $factory->set_device('temperature');
            $factory->add('MOS_temperature')
                    ->add('battery_T1')
                    ->add('battery_T2');

            $factory->set_device('voltage');
            $factory->add('battery_voltage')
                    ->add('min_cell_volt')
                    ->add('avg_cell_volt')
                    ->add('max_cell_volt');                
            $factory->set_device('current');
            
            $factory->add('battery_current');

            $factory->set_device('battery');
            $factory->add('state_of_charge');

            $factory->publish($mqtt); // update hourly

            $factory->load('bms_switch.json', 'JKBMS');
            $factory->add('balance_sw')
                    ->add('charge_MOS_sw') 
                    ->add('discharge_MOS_sw');
            $root = SWITCH_PATH.'_jkbms';
            $factory->publish($mqtt); // update hourly

            // =================================== INVERTER ================================

            $inv_cfg = file_load_json('/tmp/inverter_cfg.json');
            $factory = new MQTTConfigFactory('inverter_sensor.json', 'Inverter');            
            if ($inv_cfg)  {
                $factory->proto->device->hw_version = $inv_cfg->inv_hardware_ver;
                $factory->proto->device->sw_version = $inv_cfg->inv_software_ver;
            }    
            
            $factory->add('PV_radiator')
                    ->add('PV_external')
                    ->add('AC_radiator')
                    ->add('DC_radiator')
                    ->add('transformer');
            $factory->set_device('voltage');
            $factory->add('PV_voltage', 'PV voltage')
                    ->add('battery_v',  'Battery voltage')
                    ->add('bus_v',      'Bus voltage')
                    ->add('inverter_v', 'Inverter voltage')
                    ->add('grid_v',     'Grid voltage');
            $factory->set_device('current');
            $factory->add('control_c',  'Control current')                    
                    ->add('inverter_c', 'Inverter current')
                    ->add('load_c',     'Load current')
                    ->add('grid_c',     'Grid current')
                    ->add('charger_c',  'PV charger current');
            $factory->set_device('power');
            $factory->add('inverter_p', 'Inverter power')
                    ->add('load_p',     'Load power')
                    ->add('grid_p',     'Grid power');
            $factory->set_device('frequency');
            $factory->add('inverter_f', 'Inverter frequency')
                    ->add('grid_f',     'Grid frequency');
            
            
            $factory->set_device('enum');
            $factory->add('load_pp', 'Load %');
            unset($factory->proto->state_class);
            $factory->add('inv_relay',  'Relay inverter')
                    ->add('grid_relay', 'Relay grid')
                    ->add('N_relay',    'Relay Neutral')
                    ->add('DC_relay',   'Relay DC')
                    ->add('E_relay',    'Relay Earth');
            $factory->set_device('energy');              
            $factory->proto->state_class = 'total';
            $factory->add('accu_chg_power',  'Accum. charger power') 
                    ->add('accu_dis_power',  'Accum. discharger power')
                    ->add('accu_buy_power',  'Accum. buy power')
                    ->add('accu_sell_power', 'Accum. sell power')
                    ->add('accu_load_power', 'Accum. load power')
                    ->add('accu_self_use',   'Accum. self use power')
                    ->add('accu_PV_sell_p', 'Accum. PV sell power')
                    ->add('accu_grid_chg',  'Accum. discharger power')
                    ->add('accu_gen_power',   'Accum. PV generated power');;

            $root = SENSOR_PATH.'_inv';
            $factory->publish($mqtt);
            $factory->load('inverter_sw.json', 'JKBMS');
            $factory->add('ongrid_sw',  'ONGrid switch')
                    ->add('offgrid_sw',  'OFFGrid work')
                    ->add('dis_to_grid', 'Discharge to grid');                   

            $root = SWITCH_PATH.'_inv';
            $factory->publish($mqtt);           
            

            // $mqtt->publish('homeassistant/sensor/solar_station/test', json_encode(['message' =>  'Hello World!']), MqttClient::QOS_AT_MOST_ONCE);
            function publish_bms() {
                global $mqtt, $root, $sws;
                $bms = file_load_json('/tmp/jkbms_last.json');
                $elps = diff_minutes('now', $bms->ts);
                if ($elps > 10) {
                    log_cmsg("~C91#ERROR:~C00 JKBMS last outdated for $elps minutes");
                    return;
                }   
                $max_v = 0;
                $min_v = 3.7;
                $avg_v = 0;
                foreach ($bms->cells->voltages as $i => $v) {
                    $max_v = max($max_v, $v);
                    $min_v = min($min_v, $v);                                    
                }                
                $avg_v = round($bms->battery_voltage / $bms->bms_params->batt_string_set, 3);
                $par = $bms->bms_params;
                $json = json_encode(['MOS_temperature' => $bms->MOS_temp, 'battery_T1' => $bms->battery1_temp, 'battery_T2' => $bms->battery2_temp, 
                                     'battery_current' => $bms->battery_current, 'battery_voltage' => $bms->battery_voltage, 
                                     'min_cell_volt' => $min_v, 'max_cell_volt' => $max_v, 'avg_cell_volt' => $avg_v,
                                     'state_of_charge' => $bms->battery_SoC]);
                log_cmsg("~C97#PUB:~C00 $json");
                $mqtt->publish(SENSOR_PATH."_jkbms/state", $json);
                
                $json = json_encode(['balance_sw' => $sws[$par->balance_switch], 'charge_MOS_sw' => $sws[$par->chg_MOS_switch], 'discharge_MOS_sw' => $sws[$par->dis_MOS_switch]]);
                log_cmsg("~C97#PUB:~C00 $json");
                $mqtt->publish(SWITCH_PATH."_jkbms/state", $json);
            }   
            function publish_inverter() {
                global $mqtt, $root, $sws, $inv_cfg;
                $inv = file_load_json('/tmp/inverter_last.json');
                if (!is_object($inv)) {
                    log_cmsg("~C91#ERROR:~C00 can't load invertor data");
                    return;
                }

                $elps = diff_minutes('now', $inv->ts);
                if ($elps > 10) {
                    log_cmsg("~C91#ERROR:~C00 Inverter last outdated for $elps minutes");
                    return;
                }   
                $pvc = $inv->PV_charger;
                $tms = $inv->temp_sensors;
                $volt = $inv->voltages;
                $curr = $inv->current;
                $powr = $inv->power;
                $rs = $inv->relays;
                $accu = file_load_json('/tmp/inverter_accum.json');
                $charger = ['PV_radiator' => $pvc->rad_temp, 'PV_external' => $pvc->ext_temp, 
                            'AC_radiator' => $tms->AC_radiator, 'DC_radiator' => $tms->DC_radiator, 'transformer' => $tms->transformer,
                            'PV_voltage' => $pvc->PV_volt];

                $sensors = ['battery_v' => $volt->battery, 'bus_v' => $volt->bus, 'inverter_v' => $volt->inverter, 'grid_v' => $volt->grid,
                            'control_c' => $curr->control, 'inverter_c' => $curr->inverter, 'load_c' => $curr->load, 'grid_c' => $curr->grid,
                            'load_pp' => $inv->load_percent, 'inverter_p' => $powr->inverter, 'load_p' => $powr->load, 'grid_p' => $powr->grid,
                            'charger_c' => $pvc->chg_curr, 'inverter_f' => $inv->inv_freq, 'grid_f' => $inv->grid_freq,
                            'inv_relay' => $sws[$rs->inverter], 'grid_relay' => $sws[$rs->grid], 'N_relay' => $sws[$rs->neutral], 'DC_relay' => $sws[$rs->DC],
                            'E_relay' => $sws[$rs->earth]];

                foreach ($charger as $key => $val)
                  if ($val != 0)
                     $sensors[$key] = $val;

                // $sensors = array_merge($sensors, $temp);

                if (is_object($accu)) {
                    $map = ['accu_chg_power' => $accu->charger_power, 'accu_dis_power' => $accu->discharger_power, 
                            'accu_buy_power' => $accu->buy_power, 'accu_sell_power' => $accu->sell_power, 
                            'accu_load_power' => $accu->load_power, 'accu_self_use' => $accu->self_use,
                            'accu_PV_sell_p' => $accu->PV_sell_power, 'accu_grid_chg' => $accu->grid_charger,
                            'accu_gen_power' => $accu->load_power - $accu->buy_power + $accu->sell_power];
                    $sensors = array_merge($sensors, $map);                    
                }
                $json = json_encode($sensors);
                log_cmsg("~C97#PUB:~C00 $json");
                $mqtt->publish(SENSOR_PATH."_inv/state", $json);
                if ($inv_cfg && $cfg = $inv_cfg->config) {
                    // print_r($cfg);
                    $json = json_encode(['ongrid_sw' => $sws[$cfg->ongrid_switch], 'offgrid_sw' => $sws[$cfg->offgrid_work], 'dis_to_grid' => $sws[$cfg->dischg_to_grid]]);
                    log_cmsg("~C97#PUB:~C00 $json");
                    $mqtt->publish(SWITCH_PATH."_inv/state", $json);
                }    
                
            }

            publish_bms();
            publish_inverter();
            $mqtt->disconnect();
        }

        
    }  catch (MqttClientException $e) {
        // MqttClientException is the base exception of all exceptions in the library. Catching it will catch all MQTT related exceptions.
        log_cmsg('~C91 #EXCEPTION:~C00  %s', $e->getMessage());
    }
