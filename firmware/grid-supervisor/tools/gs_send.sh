#!/bin/sh
# Отправка файла с командами консоли на плату по одной строке (запись в NVS занимает время)
# и вывод ответов платы. Файл — вывод команды `export` (комментарии # и пустые строки пропускаются).
# Использование: tools/gs_send.sh /dev/serial/by-id/usb-1a86_USB_Single_Serial_...-if00 gs-wifi.conf
PORT=${1:?usage: gs_send.sh <port> <file>}
FILE=${2:?usage: gs_send.sh <port> <file>}
# -hupcl: закрытие порта не дёргает DTR/RTS (иначе плата может перезагрузиться)
stty -F "$PORT" 115200 raw -echo -hupcl || exit 1
cat "$PORT" &
READER=$!
exec 3>"$PORT"                       # один раз открываем на запись — без лишних переключений линий
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in ''|'#'*) continue ;; esac
  printf '%s\r\n' "$line" >&3
  sleep 0.8
done < "$FILE"
sleep 3
kill "$READER" 2>/dev/null
