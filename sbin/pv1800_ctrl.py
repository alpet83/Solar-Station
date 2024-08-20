#!/usr/bin/env python3
#import serial
import time
import math
import json
import os
import minimalmodbus
from pymodbus.client import ModbusSerialClient as ModbusClient
from pymodbus.framer import ModbusRtuFramer
from datetime import datetime
import logging
FORMAT = ('%(asctime)-15s %(threadName)-15s '
          '%(levelname)-8s %(module)-15s:%(lineno)-8s %(message)s')

logging.basicConfig(format=FORMAT)
log = logging.getLogger()
#  log.setLevel(logging.DEBUG)

from json import encoder

encoder.FLOAT_REPR = lambda o: format(o, '.3f')
client = None  # WARN: global object instance of ModbusSerialClient
instr = None   # WARN: global object instance of minimalmodbus.Instrument
SLAVE_ID = 0x04
DEVICE = '/dev/ttyACM0'
BAUDRATE = 19200
CMD_FILE = '/root/inverter_cmd.lst'
ADDR = 20000
PV1800_ID = 20566

prev_state = {'w': 0, 'inv_curr': 0, 'grid_curr': 0, 'load_curr': 0}

def s16(i):
    return i | (-(i & 0x8000))


def read_regs(addr, count):
    rr = None
    time.sleep(0.3)  # tx separation delay
    if client:
        for attempt in range(5):
            log.setLevel(logging.ERROR)  # prevent spam about noise bytes from parallel client request
            rr = client.read_holding_registers(addr, count, slave=SLAVE_ID)
            log.setLevel(logging.NOTSET)
            if rr.isError():
                log.warning(f" ({attempt}) read_holding_registers failed {count}@{addr}")
                time.sleep(0.5)
            else:
                read = len(rr.registers)
                if read == count:
                    return rr.registers
                else:
                    log.warning(f" ({attempt}) read_holding_registers insufficient {count}@{addr} = {read}")
        assert not rr.isError(), f"#FATAL: can't read registers {count}@{addr}"
    if instr:
        result = instr.read_registers(addr, count)
    return False


def map_values(registers, descr, offset=0):
    result = {}
    for dsc in descr:
        dsl = dsc.split(':')
        if len(dsl) < 2:
            log.critical(f" invalid description used `{dsc}`, must >= 2 tokens")
            continue
        idx = int(dsl[0])
        key = dsl[1]
        val = registers[idx + offset]
        if len(dsl) == 3:
            type = dsl[2]
            if type == 'i':
                val = s16(val)
            else:  # other means float with some decimals
                decimals = int(type)
                val = val * pow(10, -decimals)
                val = round(val, decimals)

        result[key] = val
    return result



charger_info = ['0:workstate', '1:MPPT_state', '2:charging_state', '4:PV_volt:1', '5:batt_volt:1', '6:chg_curr:1',
                '7:chg_power', '8:rad_temp', '9:ext_temp', '10:batt_relay', '11:PV_relay', '12:err_msg', '13:warn_msg',
                '14:batt_volt_grade', '15:rated_curr:1']
IGL = ['0:inverter:i', '1:grid:i', '2:load']


def calc_p(data, key):
    s = data['S'][key]
    q = data['Q'][key]
    if data['power'][key] == 0:  # in case firmware not handle selling power to grid
        return math.sqrt(s * s - q * q)
    return data['power'][key]


def calc_pf(data, key):
    p = data['power'][key]
    s = data['S'][key]
    if s > 0:
        return abs(round(p / s, 3))
    return 0


def scan_realtime():
    data = {}
    now = datetime.now()
    data['ts'] = now.strftime("%Y-%m-%d %H:%M:%S")
    registers = read_regs(15201, 22)
    if registers:
        data['PV_charger'] = map_values(registers, charger_info)
    registers = read_regs(25201, 79)
    vals = map_values(registers, ['0:inv_workstate', '1:AC_volt_grade', '2:rate_power', '15:load_percent'])
    data.update(vals)

    data['voltages'] = map_values(registers, ['4:battery:1', '5:inverter:1', '6:grid:1', '7:bus:1'])
    data['current'] = map_values(registers, ['8:control:1', '9:inverter:1', '10:grid:1', '11:load:1'])
    data['power'] = map_values(registers, IGL, 12)
    data['S'] = map_values(registers, IGL, 16)
    data['Q'] = map_values(registers, IGL, 20)
    for key in data['power'].keys():
        data['power'][key] = calc_p(data, key)
    data['PF'] = {'inverter': calc_pf(data, 'inverter'), 'grid': calc_pf(data, 'grid'), 'load': calc_pf(data, 'load')}

    vals = map_values(registers, ['24:inv_freq:2', '25:grid_freq:2'])
    data.update(vals)
    data['temp_sensors'] = map_values(registers, ['0:AC_radiator', '1:transformer', '2:DC_radiator'], 32)
    relays = map_values(registers, IGL, 36)
    relays.update(map_values(registers, ['0:neutral', '1:DC', '2:earth'], 39))
    data['relays'] = relays
    # TODO: store accumulated stats in dedicated file
    data['messages'] = map_values(registers, ['0:err_1', '1:err_2', '2:err_3', '4:warn_1', '5:warn_2'], 60)
    sn = registers[68] * 0x10000 + registers[69]
    data['serial'] = f"{sn:08x}"
    vals = map_values(registers, ['0:hw_ver', '1:sw_ver', '6:rated_power', '7:comm_proto_ver', '8:arrow_flags'], 70)
    data.update(vals)
    data['battery'] = map_values(registers, ['0:power:2', '1:curr:i', '2:volt_grade'], 72)
    jss = json.dumps(data)
    print(f'#REALTIME: {jss}\n')
    log.handlers[0].flush()
    with open('/tmp/inverter_last.json', 'w') as f:
        f.write(jss)

    accu = {}
    accu_map = ['charger_power', 'discharger_power', 'buy_power', 'sell_power', 'load_power', 'self_use',
                'PV_sell_power', 'grid_charger'];
    ofs = 44  #  25245 - 25201
    for param in accu_map:
        mwh = registers[ofs]
        kwh = round(registers[ofs + 1] * 0.1, 1)
        accu[param] = mwh * 1000 + kwh   # total kWh
        ofs += 2
    with open('/tmp/inverter_accum.json', 'w') as f:
        f.write(json.dumps(accu))

    # checking: need add same json to history relative previous state?
    prev_state['w'] += 1
    updated = (prev_state['w'] == 1)

    curr = data['current']
    batt = data['battery']

    if not updated:
        updated |= (abs(prev_state['inv_curr'] - curr['inverter']) > 0.2)
        updated |= (abs(prev_state['grid_curr'] - curr['grid']) > 0.2)
        updated |= (abs(prev_state['load_curr'] - curr['load']) > 0.2)
        updated |= (abs(prev_state['batt_curr'] - batt['curr']) > 2)

    if updated:
        print("#DBG: check prev_state: " + json.dumps(prev_state))
        with open('/tmp/inverter_stats.json', 'a') as f:
            f.write(jss + "\n")

    prev_state['inv_curr'] = curr['inverter']
    prev_state['grid_curr'] = curr['grid']
    prev_state['load_curr'] = curr['load']
    prev_state['batt_curr'] = batt['curr']


def process_cmd(cmd):
    pair = cmd.split('=>')
    if len(pair) == 2:
        addr = int(pair[0])
        val = int(pair[1])
        if (addr >= 20002) and (addr <= 20144):
            now = datetime.now()
            ts = now.strftime("%Y-%m-%d %H:%M:%S")
            print(f"#CMD: trying set [{addr}] = {val}")
            result = 'FAIL'
            for attempt in range(10):
                w = 'OK'
                if client:
                    w = client.write_register(addr, val, slave=SLAVE_ID)
                elif instr:
                    w = instr.write_register(add, val)
                registers = read_regs(addr, 1);
                if registers and (registers[0] == val):
                    result = 'OK'
                    break
                else:
                    sv = registers[0]
                    log.warning(f"({attempt}) failed write [{addr}] = {val}, stored {sv}, result = {w}")
                    time.sleep(1)

            with open(CMD_FILE + '.log', "a") as cmdl:
                cmdl.write(f"[{ts}] {cmd} = {result}\n")  # log processed command
        else:
            print(f"#ERROR: outrange address value = {addr} for write op")


def parse_cmds():
    if not os.path.exists(CMD_FILE):
        return
    with open(CMD_FILE, "r") as cmdf:
        data = cmdf.read().splitlines()
        os.remove(CMD_FILE)
        for line in data:
            process_cmd(line)


calibration_params = ['9:batt_voltage:i', '10:inv_voltage:i', '11:grid_voltage:i', '12:bus_voltage:i',
                      '13:ctrl_current:i', '14:inv_current:i', '15:grid_current:i']
config_params = ['0:offgrid_work', '1:output_volt:1', '2:output_freq:2', '3:search_mode', '4:ongrid_switch',  # 5-6?
                 '7:dischg_to_grid', '8:energy_use_mode', '10:grid_prot_std', '11:solar_use_aim', '12:max_dis_current:1',
                 '17:batt_stop_dis:1', '18:batt_stop_chg:1', '24:grid_max_chg_curr:1', '26:batt_low_volt:1',
                 '26:batt_high_volt:1', '31:max_comb_chg_curr:1', '41:system_setting', '42:chg_source_prio',
                 '43:solar_power_bal']
charger_params = ['0:machine_id', '3:hw_ver', '4:sw_ver', '5:pv_volt_calib', '6:bt_volt_calib', '7:curr_calib']
batt_params = ['2:float_volt:1', '3:absorb_volt:1', '4:batt_low_volt:1', '6:batt_high_volt:1', '7:pv_max_chg_curr:1',
               '9:batt_type', '10:batt_capacity', '17:batt_eq_enable', '18:batt_eq_volt:1', '20:batt_eq_time',
               '21:batt_eq_timeout', '22:batt_eq_intv']


def read_loop():
    registers = read_regs(ADDR, 16)
    data = {}
    conf = {}
    midx = registers[0]
    prefix = midx.to_bytes(2, 'big').decode('utf-8')
    code = registers[1]
    data['machine_id'] = prefix + str(code)
    sn = registers[2] * 0x10000 + registers[3]
    data['serial_num'] = f"{sn:08x}"
    data.update(map_values(registers, ['4:inv_hardware_ver', '5:inv_software_ver', '6:protocol_edition']))
    data['calibration'] = map_values(registers, calibration_params)
    registers = read_regs(20101, 44)
    jss = json.dumps(registers)
    with open('/tmp/inverter_raw.json', 'w') as f:
        f.write(jss)

    data['config'] = map_values(registers, config_params)
    registers = read_regs(10001, 8)
    chgc = map_values(registers, charger_params)
    sn = registers[1] * 0x10000 + registers[2]
    chgc['serial'] = f"{sn:08x}"
    registers = read_regs(10101, 23)  # charging voltages, 10009-10100 is reserved
    battp = map_values(registers, batt_params)
    chgc.update(battp)
    data['chg_config'] = chgc
    jss = json.dumps(data)
    print(f'#CONFIG {jss}')
    with open('/tmp/inverter_cfg.json', 'w') as f:
        f.write(jss)
    while time.time() % 60 < 50:
        scan_realtime()
        parse_cmds()
        time.sleep(5)
    return True

def using_pymodbus():
    global client
    print("#DBG: Using `pymodbus`, connecting to " + DEVICE)
    # OLD pymodbus version: method='rtu', port=DEVICE, stopbits=1, bytesize=8, baudrate=BAUDRATE, timeout=2
    # handle_local_echo=True framer=ModbusRtuFramer,
    client = ModbusClient(DEVICE, baudrate=BAUDRATE, timeout=0.2, name='machine')

    result = False
    if client.connect():
        log.debug("#CONNECTED: trying read registers...\n")
        registers = read_regs(ADDR, 1)
        if registers:
            midx = registers[0]
            log.info(f" machine idx = {midx}\n")
            if midx == PV1800_ID:
                result = read_loop()
        else:
            log.critical("#FAILED: can't read register\n")
        client.close()
    else:
        log.critical("#FATAL: can't connect to device...\n")
    return result


def using_mini():
    global instr
    # Test connection is available
    log.debug("#DBG: trying minimalmodbus...\n")
    instr = minimalmodbus.Instrument(DEVICE, 4, debug=True)
    instr.serial.baudrate = BAUDRATE
    instr.serial.bytesize = 8
    instr.serial.stopbits = 1
    instr.serial.timeout = 2
    # instr.clear_buffers_before_each_transaction = True
    instr.mode = minimalmodbus.MODE_RTU
    midx = instr.read_register(ADDR)
    log.info(f"Machine index = {midx}\n")
    if midx == PV1800_ID:
        return read_loop()
    # instr.close()


if __name__ == "__main__":
    time.sleep(1)  # datalogger conflict prevention with crontab usage
    #
    if not using_pymodbus():
        using_mini()

