"""Receive real MQTT messages and preserve timestamps, outages and raw payloads."""

import argparse
from datetime import datetime, timedelta, timezone
import getpass
import json
from pathlib import Path
import threading
import time
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, help='LAN IPv4 of the broker PC')
    parser.add_argument('--port', type=int, default=1883)
    parser.add_argument('--username', default='sv23110186')
    parser.add_argument('--topic', default='sv23110186/node01/telemetry')
    parser.add_argument('--seconds', type=float, default=620,
                        help='Observation time after first SUBACK; 0 = until Ctrl+C')
    parser.add_argument('--out', type=Path, default=Path('log_10min.txt'))
    args = parser.parse_args()
    if args.seconds < 0:
        parser.error('--seconds must be >= 0')
    if args.out.exists():
        parser.error('Output already exists; choose another name to preserve evidence.')
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        parser.error('Install dependencies: python -m pip install -r scripts/requirements.txt')

    password = getpass.getpass('MQTT password (hidden): ')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    lock = threading.Lock()
    ready = threading.Event()
    failed = threading.Event()
    stats = {'received': 0, 'retained': 0, 'disconnects': 0,
             'first_message_at': None, 'last_message_at': None}
    timing = {'subscribed_at': None}
    zone = timezone(timedelta(hours=7))
    with args.out.open('x', encoding='utf-8') as output:
        def emit(event, **fields):
            row = {'received_at': datetime.now(zone).isoformat(timespec='milliseconds'),
                   'event': event, **fields}
            line = json.dumps(row, ensure_ascii=False)
            with lock:
                output.write(line + '\n')
                output.flush()
            print(line, flush=True)

        def on_connect(client, userdata, flags, reason_code, properties):
            emit('connect', reason=str(reason_code))
            if reason_code.is_failure:
                failed.set()
                return
            result, _ = client.subscribe(args.topic, qos=0)
            if result != mqtt.MQTT_ERR_SUCCESS:
                emit('subscribe_failed', code=int(result))
                failed.set()

        def on_subscribe(client, userdata, mid, reason_codes, properties):
            if any(code.is_failure for code in reason_codes):
                emit('subscribe_rejected', reasons=[str(code) for code in reason_codes])
                failed.set()
                return
            if timing['subscribed_at'] is None:
                timing['subscribed_at'] = time.monotonic()
            emit('subscribed', topic=args.topic)
            ready.set()

        def on_disconnect(client, userdata, flags, reason_code, properties):
            if reason_code.is_failure:
                stats['disconnects'] += 1
            emit('disconnect', reason=str(reason_code))

        def on_message(client, userdata, message):
            stats['received'] += 1
            if message.retain:
                stats['retained'] += 1
            now = datetime.now(zone).isoformat(timespec='milliseconds')
            if stats['first_message_at'] is None:
                stats['first_message_at'] = now
            stats['last_message_at'] = now
            emit('message', topic=message.topic, qos=message.qos,
                 retained=message.retain,
                 payload=message.payload.decode('utf-8', errors='replace'))

        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id='th4-observer-' + uuid.uuid4().hex[:10],
                             protocol=mqtt.MQTTv311)
        client.username_pw_set(args.username, password)
        client.reconnect_delay_set(min_delay=1, max_delay=5)
        client.on_connect = on_connect
        client.on_subscribe = on_subscribe
        client.on_disconnect = on_disconnect
        client.on_message = on_message
        emit('session', broker=args.host, port=args.port, topic=args.topic,
             target_seconds=args.seconds)
        client.connect_async(args.host, args.port, keepalive=15)
        client.loop_start()
        stopped_by_user = False
        try:
            deadline = time.monotonic() + 30
            while not ready.wait(0.1):
                if failed.is_set() or time.monotonic() >= deadline:
                    emit('error', detail='No subscription within 30 seconds; check network/auth.')
                    failed.set()
                    break
            while ready.is_set() and not failed.is_set():
                elapsed = time.monotonic() - timing['subscribed_at']
                if args.seconds and elapsed >= args.seconds:
                    break
                time.sleep(0.1)
        except KeyboardInterrupt:
            stopped_by_user = True
        finally:
            elapsed = (time.monotonic() - timing['subscribed_at']
                       if timing['subscribed_at'] is not None else 0)
            client.disconnect()
            client.loop_stop()
            emit('summary', elapsed_seconds=round(elapsed, 3),
                 stopped_by_user=stopped_by_user, **stats)
        if failed.is_set() or stats['received'] == 0:
            raise SystemExit(1)


if __name__ == '__main__':
    main()
