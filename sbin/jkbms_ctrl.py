#!/usr/bin/python3

import time
import json
import argparse
import serial
from datetime import datetime

def crcJK232(byteData):
    """
    Generate JK RS232 / RS485 CRC
    - 2 bytes, the verification field is "command code + length byte + data segment content",
    the verification method is thesum of the above fields and then the inverse plus 1, the high bit is in the front and the low bit is in the back.
    """
    CRC = 0
    for b in byteData:
        CRC += b
    crc_low = CRC & 0xFF
    crc_high = (CRC >> 8) & 0xFF
    return [crc_high, crc_low]   


## Vars ################
port = "/dev/ttyUSB0"
baud = 115200
read_all = b'\x4E\x57\x00\x13\x00\x00\x00\x00\x06\x03\x00\x00\x00\x00\x00\x00\x68\x00\x00\x01\x29'
# for id=1 b'\x4E\x57\x00\x13\x00\x00\x00\x01\x06\x03\x00\x00\x00\x00\x00\x00\x68\x00\x00\x01*'
########################

def format_rqs(cmd, frame_info = '00', rec_num = 0, bms_id = 0):
    rqs = bytes()
    bo = 'big'
    rqs += b"NW\x00" # 19 bytes length
    payload = bytes.fromhex(frame_info)
    rqs_len = 18 + len(payload)
    rqs += rqs_len.to_bytes(1, bo)
    rqs += bms_id.to_bytes(4, bo)
    rqs += cmd.to_bytes(1, bo)
    rqs += b"\x03"  # frame source - PC Host computer
    rqs += b"\x00"  # transport_type.to_bytes(1, bo)
    rqs += payload
    rqs += rec_num.to_bytes(4, bo)
    rqs += b"\x68\x00\x00"  # end flag and zeros
    crc_high, crc_low = crcJK232(rqs)
    rqs += crc_high.to_bytes(1, bo)
    rqs += crc_low.to_bytes(1, bo)
    return rqs

bms_params = [[0x8e, 2, "total_OVP", 0.01],
              [0x8f, 2, "total_UVP", 0.01],
              [0x90, 2, "cell_OVP", 0.001],
              [0x91, 2, "cell_OVP_recv", 0.001],
              [0x92, 2, "cell_OVP_delay", 1],
              [0x93, 2, "cell_UVP", 0.001],
              [0x94, 2, "cell_UVP_recv", 0.001],
              [0x95, 2, "cell_UVP_delay", 1],
              [0x96, 2, "diff_tresh", 0.001],
              [0x97, 2, "dis_OCP", 1],
              [0x98, 2, "dis_OCP_delay", 1],
              [0x99, 2, "chg_OCP", 1],
              [0x9a, 2, "chg_OCP_delay", 1],
              [0x9b, 2, "balance_start", 0.001],
              [0x9c, 2, "balance_diff",  0.001],
              [0x9d, 1, "balance_switch", 1],
              [0x9e, 2, "MOS_temp_tresh", 1],
              [0x9f, 2, "MOS_temp_recv", 1],
              [0xa0, 2, "batt_temp_tresh", 1],
              [0xa1, 2, "batt_temp_recv", 1],
              [0xa2, 2, "batt_temp_diff", 1],
              [0xa3, 2, "batt_OVH_chg", 1],
              [0xa4, 2, "batt_OVH_dis", 1],
              [0xa5, 2, "batt_LTP_chg", 1],
              [0xa6, 2, "batt_LTP_crecv", 1],
              [0xa7, 2, "batt_LTP_dis", 1],
              [0xa8, 2, "batt_LTP_drecv", 1],
              [0xa9, 1, "batt_string_set", 1],
              [0xaa, 4, "batt_capacity_set", 1],
              [0xab, 1, "chg_MOS_switch", 1],
              [0xac, 1, "dis_MOS_switch", 1],
              [0xad, 1, "prot_board_addr", 1],
              [0xad, 2, "current_calibr", 0.001],
              [0xae, 1, "prot_board_addr", 1],
              [0xaf, 1, "battery_type", 1],
              [0xb0, 2, "sleep_wait", 1],
              [0xb1, 1, "low_volume_alarm", 1],
              [0xb3, 1, "dedic_chg_switch", 1],
              [0xb6, 4, "sys_working_hrs", 1 / 60],
              [0xb8, 1, "start_calibr", 1],
              [0xb9, 4, "actual_batt_cap", 1],
              [0xc0, 1, "protocol_ver", 1]]

def bytes2hex(b):
  return '0x' + b.hex()
          
def bytes2int(b):  
  return int(b.hex(), 16) 

def decode_cells(response):
  size  = bytes2int(response[1:2]) # chunk size
  ccnt  = size // 3
  cdata = { "count":ccnt, "voltages":{} } 
    
  for i in range(ccnt):
    ofs = 2 + i * 3
    if ofs + 3 <= len(response):
      cn = bytes2int(response[ofs:ofs + 1])
      cv = bytes2int(response[ofs + 1: ofs + 3]) / 1000
      cdata['voltages'][f"{cn}"] = cv
  response = response[size + 2:]  # skip data and descr  
  return cdata, response

def decode_str(response, sz = 4, encoding = "utf-8"):
    value = response[1:sz + 1]
    response = response[sz + 1:]
    return value.decode(encoding).rstrip("\x00"), response

def decode_hex(response, sz = 4):
    value = response[1:sz + 1]
    response = response[sz + 1:]
    return value.hex(), response

def decode_num(response, coef, sz = 1):
    value = bytes2int(response[1:sz + 1])
    response = response[sz + 1:]
    return round(value * coef, 3), response

def decode_num8(response, coef = 1):
  value, response = decode_num(response, coef, 1)
  return value, response

def decode_num16(response, coef = 1):
  value, response = decode_num(response, coef, 2)
  return value, response

def decode_num32(response, coef = 1):
  value, response = decode_num(response, coef, 4)
  return value, response

def decode_current(response):
  value, response = decode_num16(response, 1)
  if value & 32768:
     value = value & 32767
  else:
     value = -value
  value = round(value * 0.01, 2)
  return value, response

def decode_temp(response):
  temp = bytes2int(response[1:3])
  if temp > 100:
    temp = - (temp - 100) # frozen side
  response = response[3:]
  return temp, response

def decode_response(response):
  """
  Override the default get_responses as its different
  """
  now = datetime.now()
  packet = {}
  rlen = len(response)
  if rlen < 16:
    return "nope";
  stx = response[0:2].hex()
  if stx != "4e57": 
    return "unknown signature " + stx;
  pklen = bytes2int(response[2:4])
  bms_id = bytes2hex(response[4:8])
#  4e57012100000000060001
  packet['ts'] = now.strftime("%Y-%m-%d %H:%M:%S")
  packet['bms_id'] = bms_id
  packet['cells'] = {}
  packet['bms_params'] = {}
  response = response[11:]  # skip header
  while len(response) >= 5:
    code = bytes2int(response[0:1])
    bms_pv = False
    num_sz = 2
    coef = 1
    # scan config codes
    for bp in bms_params:
      if code == bp[0]:
        num_sz = bp [1]
        bms_pv = bp[2]
        coef = bp[3]
        break

    if code == 0x00:
        packet['complete'] = 1
        break
    elif code == 0x79:
      packet['cells'],response = decode_cells(response)
    elif code == 0x80:
      packet['MOS_temp'], response = decode_temp(response) 
    elif code == 0x81:
      packet['battery1_temp'], response = decode_temp(response) 
    elif code == 0x82:
      packet['battery2_temp'], response = decode_temp(response) 
    elif code == 0x83:
      packet['battery_voltage'], response = decode_num16(response, 0.01) 
    elif code == 0x84:
      packet['battery_current'], response = decode_current(response) 
    elif code == 0x85:
      packet['battery_SoC'], response = decode_num8(response) 
    elif code == 0x86:
      packet['temps_sensors'], response = decode_num8(response) 
    elif code == 0x87:
      packet['cycle_count'], response = decode_num16(response) 
    elif code == 0x89:
      packet['cycle_capacity'], response = decode_num32(response) 
    elif code == 0x8a:
      packet['total_battery_str'], response = decode_num16(response) 
    elif code == 0x8b:
      packet['battery_warn_msg'], response = decode_num16(response) 
    elif code == 0x8c:
      packet['battery_status_info'], response = decode_num16(response) 
    elif bms_pv:
      packet['bms_params'][bms_pv],response = decode_num(response, coef, num_sz)
    elif code == 0xb2:
        bms_pwd, response = decode_str(response, 10)
    elif code == 0xb4:
        packet['device_id'], response = decode_str(response, 8)
    elif code == 0xb5:
        packet['date_manufact'], response = decode_str(response, 4)
    elif code == 0xb7:
        packet['firmware_ver'], response = decode_str(response, 15)
    elif code == 0xba:
        packet['id_manufact'], response = decode_str(response, 24)
    else:
      packet['last_code'] = code
      packet['tail'] = response.hex()
      break
  

#  for defn in response_map:
#      size = defn[1]
#      item = response[:size]
#      responses.append(item)
#      response = response[size:]
#  if response:
#      responses.append(response)     
  return packet

with serial.serial_for_url(port, baud) as s:
    s.timeout = 1
    s.write_timeout = 1
    s.flushInput()
    s.flushOutput()
    # rqs = format_rqs(3, '\xb1')  # set low capacity alarm level to 30?
    # bytes_written = s.write(rqs)
    # print(f"  write request {rqs}, sent {bytes_written} bytes\n")
    rqs = read_all
    parser = argparse.ArgumentParser(description='Interact with JKBMS by RS485 protocol V2.5')
    parser.add_argument('--cmd', metavar='CMD', type=int, default=0, required=False,
                        help='1,2,3,5,6 - activate, write, read, update, read_all')
    parser.add_argument('--id', metavar='ID', type=int, default=0, required=False,
                        help="00, 0x79-0xba - parameter selector for r/w/u")
    parser.add_argument('--value', metavar='VALUE', type=str, default='', required=False,
                        help="HEX bytes string for rewrite parameter")
    parser.add_argument('--bms_id', metavar='bms_id', type=int, default=0, required=False,
                        help="BMS ID on bus, for selection (0 = all)")
    args = parser.parse_args()
    save_res = True
    if (args.cmd >= 1) and (args.cmd <= 6):
        save_res = False
        rqs = format_rqs(args.cmd, f"{args.id:02x}" + args.value, args.bms_id)
        seconds = time.localtime().tm_sec
        if seconds < 10:
            print("collision delay, wait...\n")
            time.sleep(10 - seconds)
    rqs_hex = rqs.hex()
    print(f"sending command {rqs}:{rqs_hex} ")
    bytes_written = s.write(rqs)
    print(f"wrote {bytes_written} bytes\n")
    accum = []

    for _ in range(7):
        #            response_line = s.readline().hex()
        response = s.read(size=300)
        response_line = response.hex()
        if len(response) == 0:
            if len(accum) >= 5:
                break
            s.write(rqs)
            continue
        print(f"Got response: {response_line}")
        dec = decode_response(response)
        print(f"decoded: {dec}")
        accum.append(dec)

    avg = {}
    avg_params = {'battery_voltage': 2, 'battery_current': 2}
    count = 0
    for list in accum:
        count += 1
        for key in list.keys():
            if count > 1:
                if key in avg_params.keys():
                    avg[key] += list[key]
            else:
                avg[key] = list[key]
    if count > 1:
        print(f" using {count} samples to calc. average values...\n")
        for key in avg_params.keys():
            avg[key] = round(avg[key] / count, avg_params[key])  # average samples

    if save_res:
        jss = json.dumps(avg)
        jss = jss.replace("\x00", ".")
        jss = jss.replace("\\u0000", ".")
        with open('/tmp/jkbms_last.json','w') as f:
            f.write(jss)
        with open('/tmp/jkbms_stats.json','a') as f:
            f.write(jss + "\n")
