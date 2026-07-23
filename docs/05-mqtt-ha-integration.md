[← Оглавление](README.md) · [Справочник скриптов](04-scripts-reference.md)

# MQTT / Home Assistant интеграция

Документ описывает, как `mqtt/mqtt-client.php` публикует данные BMS и инвертора в Home Assistant по протоколу MQTT Discovery, и как обратно принимаются команды переключателей.

## 1. Обзор потока

Схематично цикл работы `mqtt-client.php` выглядит так:

```
mqtt-client.php (запуск)
   │
   ├─ MqttConfigFactory формирует HA-discovery конфиги
   │  и публикует их в  homeassistant/.../config
   │                     │
   │                     ▼
   │        Home Assistant читает config-топики
   │        и создаёт сущности (sensor.*, switch.*)
   │
   ├─ главный цикл (пока минута < 59):
   │     publish_bms()      → homeassistant/sensor/solar_station_jkbms/state
   │                          homeassistant/switch/solar_station_jkbms/state
   │     publish_inverter() → homeassistant/sensor/solar_station_inv/state
   │                          homeassistant/switch/solar_station_inv/state
   │                     │
   │                     ▼
   │        Home Assistant обновляет значения сущностей
   │        каждые несколько секунд (пока работает цикл)
   │
   └─ для switch-сущностей HA, наоборот, публикует команду
      в  homeassistant/switch/solar_station_{jkbms|inv}/<value_key>/set
                     │
                     ▼
      on_switch_set() (подписан через MqttConfigFactory::publish())
                     │
                     ▼
      дозапись строки "<адрес>=>значение" в /root/inverter_cmd.lst
                     │
                     ▼
      sbin/pv1800_ctrl.py на очередном опросе инвертора вычитывает
      файл и применяет команду записью в Modbus-регистр инвертора
```

Важный нюанс: `on_switch_set()` умеет доводить до записи в регистр инвертора **не все** заявленные в HA переключатели — подробности в разделе 5.

## 2. Топики

Базовые пути заданы константами в начале `mqtt-client.php`:

| Константа      | Значение                              |
|-----------------|----------------------------------------|
| `SENSOR_PATH`   | `homeassistant/sensor/solar_station`   |
| `SWITCH_PATH`   | `homeassistant/switch/solar_station`   |

Полный список используемых топиков:

| Назначение | Топик |
|---|---|
| Discovery-конфиги датчиков BMS | `homeassistant/sensor/solar_station_jkbms/<value_key>/config` |
| Discovery-конфиги переключателей BMS | `homeassistant/switch/solar_station_jkbms/<value_key>/config` |
| Discovery-конфиги датчиков инвертора | `homeassistant/sensor/solar_station_inv/<value_key>/config` |
| Discovery-конфиги переключателей инвертора | `homeassistant/switch/solar_station_inv/<value_key>/config` |
| Состояние датчиков BMS | `homeassistant/sensor/solar_station_jkbms/state` (`SENSOR_PATH."_jkbms/state"`) |
| Состояние переключателей BMS | `homeassistant/switch/solar_station_jkbms/state` (`SWITCH_PATH."_jkbms/state"`) |
| Состояние датчиков инвертора | `homeassistant/sensor/solar_station_inv/state` (`SENSOR_PATH."_inv/state"`) |
| Состояние переключателей инвертора | `homeassistant/switch/solar_station_inv/state` (`SWITCH_PATH."_inv/state"`) |
| Команда переключателя «ongrid_sw» (регистр `20105`) | `homeassistant/switch/solar_station_inv/ongrid_sw/set` |
| Команда переключателя «dis_to_grid» (регистр `20108`) | `homeassistant/switch/solar_station_inv/dis_to_grid/set` |

Обратите внимание: все `state`-топики — общие на семейство устройства (BMS или инвертор), а не по одному на каждый датчик: `publish_bms()`/`publish_inverter()` кладут в один JSON сразу все значения, и каждая HA-сущность вытаскивает своё поле через `value_template`. А вот `command_topic` (см. раздел 3), наоборот, у каждого переключателя свой собственный.

## 3. HA discovery

Автоматическая регистрация сущностей в Home Assistant построена вокруг класса `MqttConfigFactory`:

- `load($proto_file)` читает JSON-шаблон конфигурации из файла (`bms_sensor.json`, `bms_switch.json`, `inverter_sensor.json`, `inverter_sw.json`) в `$this->proto` и запоминает "группу" (`sensor`/`switch`) по имени файла.
- `set_device($device_class, $unit)` проставляет `device_class` в прототип и `device->identifiers`, а также подбирает `unit_of_measurement` по встроенной таблице соответствий (`current` → `A`, `energy` → `kWh`, `frequency` → `Hz`, `power` → `W`, `temperature` → `°C`, `state_of_charge` → `%`, `voltage` → `V`); единица явно переданная в вызове используется, если для класса нет записи в таблице.
- `add($value_key, $value_name, $dev_props)` клонирует прототип под конкретный ключ значения:
  - `value_template` формируется как `{{ value_json.<value_key> }}` — именно так HA достаёт нужное поле из общего JSON в `state`-топике;
  - `name` — либо явно переданное, либо `value_key` с заменой `_` на пробел;
  - `unique_id` строится как `<префикс><md5(strtolower($dev_id.$value_key))>`, где префикс — `device_class`, если он задан, иначе имя группы (`sensor`/`switch`). Это гарантирует уникальность ID сущности в рамках всей инсталляции HA даже при совпадении коротких имён у разных устройств;
  - если в прототипе присутствует `command_topic` (то есть это switch-конфиг), он переписывается функцией `str_ireplace`, подставляя `value_key` в путь: `_bms/set` → `_bms/$value_key/set`, `_inv/set` → `_inv/$value_key/set`.

Переписывание `command_topic` необходимо, потому что шаблонный JSON (`bms_switch.json`, `inverter_sw.json`) содержит **один и тот же** `command_topic` (`.../solar_station_jkbms/set` или `.../solar_station_inv/set`) для всех переключателей данного устройства. Если оставить его как есть, все switch-сущности семейства подписывались бы на один и тот же топик, и `on_switch_set()` не мог бы понять, какой именно регистр нужно переключить — сообщение пришло бы без указания, к какому value_key оно относится. Подставляя `value_key` в путь, каждая сущность получает собственный уникальный `command_topic` (`.../ongrid_sw/set`, `.../dis_to_grid/set`, `.../charge_MOS_sw/set` и т.д.), и `on_switch_set()` определяет нужный регистр прямо по топику входящего сообщения.

Пример шаблона `mqtt/bms_switch.json` (до подстановки value_key):

```json
{
    "command_topic":"homeassistant/switch/solar_station_jkbms/set",
    "state_topic":"homeassistant/switch/solar_station_jkbms/state",
    "unique_id":"switch_bla_bla",
    "name":"device_switch",
    "device":{
       "identifiers":["jkbms"],
       "name":"Solar Station / BMS control",
       "manufacturer": "Ji Kong",
       "model": "JK-PB2A16S20P",
       "serial_number": "130024",
       "hw_version": "1",
       "configuration_url": "https://localhost"
    }
}
```

После `->add('charge_MOS_sw')` итоговый конфиг, который улетит в `homeassistant/switch/solar_station_jkbms/charge_mos_sw/config`, получит `command_topic = "homeassistant/switch/solar_station_jkbms/charge_MOS_sw/set"`, `value_template = "{{ value_json.charge_MOS_sw }}"` и свой `unique_id` на основе `md5`.

`MqttConfigFactory::publish($mqtt)` в конце делает две вещи: подписывается на все `command_topic` из накопленных конфигов (вызывая `on_switch_set()` через анонимную обёртку), а затем публикует сами JSON-конфиги по путям `<root>/<value_key>/config` (в нижнем регистре), где `$root` — это глобальная переменная, которую main-скрипт переставляет перед каждым вызовом `publish()` (`SENSOR_PATH.'_jkbms'`, `SWITCH_PATH.'_jkbms'`, `SENSOR_PATH.'_inv'`, `SWITCH_PATH.'_inv'`).

## 4. Публикуемые датчики

**`publish_bms()`** берёт снимок `/tmp/jkbms_last.json` (если он не устарел более чем на `OUTDATED_TRESH` минут и ещё не публиковался) и шлёт два JSON:

- в `SENSOR_PATH."_jkbms/state"`: `MOS_temperature`, `battery_T1`, `battery_T2`, `battery_current`, `battery_voltage`, `min_cell_volt`, `avg_cell_volt`, `max_cell_volt`, `state_of_charge`;
- в `SWITCH_PATH."_jkbms/state"`: `balance_sw`, `charge_MOS_sw`, `discharge_MOS_sw` (текстовые `ON`/`OFF`, подставляются из `$sws_desc`).

**`publish_inverter()`** берёт `/tmp/inverter_last.json` (плюс `/tmp/inverter_accum.json` для накопительных счётчиков) и шлёт в `SENSOR_PATH."_inv/state"` набор полей, сгруппированный по смыслу:

| Группа | Примеры ключей |
|---|---|
| Напряжения | `battery_v`, `bus_v`, `inverter_v`, `grid_v`, `PV_voltage` |
| Токи | `control_c`, `inverter_c`, `load_c`, `grid_c`, `charger_c` |
| Мощности | `inverter_p`, `load_p`, `grid_p`, `load_pp` |
| Частоты | `inverter_f`, `grid_f` |
| Коэффициент мощности | `inverter_PF`, `load_PF`, `grid_PF` (публикуются только если значение `> 0`) |
| Температуры зарядного контроллера | `PV_radiator`, `PV_external`, `AC_radiator`, `DC_radiator`, `transformer` (публикуются только если значение `!= 0`) |
| Состояния (enum) | `pvc_workstate`, `chg_state`, `MPPT_state` |
| Состояния реле | `inv_relay`, `grid_relay`, `N_relay`, `DC_relay`, `E_relay` |
| Накопительные счётчики энергии | `accu_chg_power`, `accu_dis_power`, `accu_buy_power`, `accu_sell_power`, `accu_load_power`, `accu_self_use`, `accu_PV_sell_p`, `accu_grid_chg`, `accu_gen_power` |

Отдельно, если загружен `/tmp/inverter_cfg.json` (`$inv_cfg`), в `SWITCH_PATH."_inv/state"` публикуется состояние трёх переключателей: `ongrid_sw`, `offgrid_sw`, `dis_to_grid`.

Есть небольшой защитный приём: значения с `power` в имени ключа, если они равны `0`, раз в 10 минут (`$minute % 10 == 0`) подменяются на `0.1` — чтобы графики в Grafana не «залипали» на плоской нулевой линии из-за особенностей интерполяции.

Полный и точный список полей (включая формулы вычисления вроде `avg_cell_volt` или `accu_gen_power`) проще смотреть непосредственно в коде `publish_bms()`/`publish_inverter()` в `mqtt/mqtt-client.php` — здесь приведён обзор по категориям, а не построчная копия.

## 5. Приём команд от Home Assistant

Функция `on_switch_set(string $topic, string $message, bool $retained)` — единственный обработчик входящих MQTT-команд. Она:

1. Вырезает из имени топика префиксы `SWITCH_PATH."_inv/"`, `SWITCH_PATH."_bms"` и суффикс `/set`, получая «голый» `value_key` (`$id`).
2. Ищет `$id` в жёстко заданной карте адресов Modbus-регистров:
   ```php
   $addrs = ['ongrid_sw' => 20105, 'dis_to_grid' => 20108];
   ```
3. Если `$id` найден — переводит сообщение (`ON`/`OFF`) в `1`/`0` и дописывает строку `"<адрес>=>значение\n"` в `/root/inverter_cmd.lst` через `file_put_contents(..., FILE_APPEND)`. Эту очередь дальше вычитывает и применяет к инвертору `sbin/pv1800_ctrl.py`.
4. Если `$id` не найден, но содержит подстроку `_MOS_` (то есть это один из BMS-переключателей `charge_MOS_sw` / `discharge_MOS_sw`) — код доходит до `// TODO: command to BMS` и делает `return;`.

Таким образом, **реально сегодня отрабатывают только два переключателя** — `ongrid_sw` (регистр `20105`) и `dis_to_grid` (регистр `20108`) на стороне инвертора.

Переключатели BMS (`balance_sw`, `charge_MOS_sw`, `discharge_MOS_sw`) при этом **полноценно заявлены в HA discovery** — `bms_switch.json` через `MqttConfigFactory` публикует для них рабочие конфиги, они появляются в интерфейсе Home Assistant как обычные переключатели и реагируют на клики. Но их обработчик в `on_switch_set()` — заглушка: команда молча теряется, никакой записи на BMS не происходит и никакой ошибки пользователю не показывается.

**Это разрыв в UX, который стоит иметь в виду**: пользователь Home Assistant может переключить `balance_sw`/`charge_MOS_sw`/`discharge_MOS_sw` в интерфейсе, увидеть, что тумблер вроде бы сработал (retain/optimistic-обновление на стороне HA), но реального эффекта на оборудовании не будет — ни ошибки, ни отката состояния, просто тишина. До реализации `// TODO: command to BMS` эти три переключателя в интерфейсе HA лучше считать нерабочими декорациями.

## 6. Уже исправленные и известные проблемы

В рамках спринта правок в `on_switch_set()` были устранены две проблемы:

- значение `$val` для команды теперь всегда приводится к строкам `"1"`/`"0"` (раньше при передаче `false`/булева `OFF` в файл команд могла попасть пустая строка вместо `0`);
- запись в `/root/inverter_cmd.lst` теперь выполняется с флагом `FILE_APPEND`, то есть дозаписывается, а не затирает файл целиком.

При этом остаётся несогласованность режима записи между `mqtt-client.php` и `sbin/batmon.php`: `batmon.php` по-прежнему пишет в `/root/inverter_cmd.lst` обычным `file_put_contents()` без дозаписи, то есть перезаписывает файл целиком, и при совпадении по времени с MQTT-командой от Home Assistant одна из двух команд может потеряться. Подробнее это уже описано в [справочнике скриптов](04-scripts-reference.md), здесь не дублируется.

## 7. Куда дальше

Всё внешнее управление инвертором и BMS через Home Assistant сегодня проходит без какой-либо авторизации, кроме учётных данных самого MQTT-брокера, и без разделения прав между источниками команд (`mqtt-client.php` и `batmon.php` пишут в одну и ту же общую очередь). Как ограничить и упорядочить это внешнее управление — см. [Безопасность и планы по авторизации](08-security-roadmap.md).

---

[← Справочник скриптов](04-scripts-reference.md) · [Оглавление](README.md) · [Регистры Modbus →](06-modbus-registers.md)
