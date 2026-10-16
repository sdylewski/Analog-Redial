[Home](../README.md) · [4 · Set up & use](setup.md)

# Home Assistant

> **Experimental.** The board side has been tested against a Mosquitto
> broker (discovery messages, state, events, the ring command, switching it
> on and off). Nobody has run any of this against a real Home Assistant yet,
> and the YAML below is untested. Expect to adjust it. Reports welcome.

There's no add-on or integration to install. The board already speaks plain
HTTP, so Home Assistant talks to it with built-in pieces:

| Direction | How | Good for |
|---|---|---|
| HA reads the board | [`rest`](https://www.home-assistant.io/integrations/rest/) sensor polling `/api/status` | Line state, last caller, Wi-Fi, uptime |
| HA tells the board | [`rest_command`](https://www.home-assistant.io/integrations/rest_command/) | Ring a phone, fire a pin output, quiet hours |
| The board tells HA | A **webhook**: the board POSTs each event the moment it happens | "Someone dialed 42", "the phone is ringing", "new message" |
| Both, with no YAML | **MQTT** with auto-discovery: the board shows up in Home Assistant as a device | Everything above, as ready-made entities (§4) |

The examples assume the board is `phone.local` and Home Assistant is
`homeassistant.local:8123`. Use IP addresses if `.local` names don't work on
your network.

## 1. Events from the board (webhook)

On the board: **Settings → Home Assistant → Webhook** (turn Home Assistant on at the top of that tab first). Enter

```
http://homeassistant.local:8123/api/webhook/redial-phone
```

and press **Send test event**. Change `redial-phone` to something hard to
guess: anyone on your network who knows the ID can trigger the automation.
Only `http://` works; the board doesn't do HTTPS here.

Every event is a POST with a small JSON body:

```json
{"board":"phone","event":"dial","line":0,"detail":"42","time":1760371200}
```

| `event` | When | `detail` |
|---|---|---|
| `offhook` | A handset is lifted | |
| `onhook` | A handset is hung up | |
| `dial` | A dial code the board knows was dialed | The digits |
| `wrong_number` | Digits that match no code | The digits |
| `ring` | A phone starts ringing | The caller: `name <number>`, `line N`, or `Bluetooth <number>` |
| `answering_machine` | The answering machine picked up | `VoIP` or `other line` |
| `recording` | A recording was saved | `message`, `voicemail` or `greeting`, then the file name |
| `test` | The **Send test event** button | |

`line` counts from 0. `time` is Unix seconds, or 0 if the board hasn't set
its clock yet.

In Home Assistant, an automation with a webhook trigger receives them. Two
examples, a dial code that turns on a light and a notification for a new
message:

```yaml
# automations.yaml
- alias: "Rotary phone: dial 42 for the porch light"
  triggers:
    - trigger: webhook
      webhook_id: redial-phone
      allowed_methods: [POST]
      local_only: true
  conditions:
    - condition: template
      value_template: >
        {{ trigger.json.event == 'dial' and trigger.json.detail == '42' }}
  actions:
    - action: light.toggle
      target:
        entity_id: light.porch

- alias: "Rotary phone: new message"
  triggers:
    - trigger: webhook
      webhook_id: redial-phone
      allowed_methods: [POST]
      local_only: true
  conditions:
    - condition: template
      value_template: >
        {{ trigger.json.event == 'recording' and not trigger.json.detail.startswith('greeting') }}
  actions:
    - action: notify.notify
      data:
        message: "New message on the rotary phone ({{ trigger.json.detail }})"
```

For a code that exists only for Home Assistant, add it under Settings → Dial
Codes → **Home Assistant codes**: dialing it plays the OK tone and sends the
`dial` event, and nothing else happens on the board. Its **Test** button sends
the event without dialing. Every other dial code still does its own job as
well as sending `dial`.

## 2. Reading the board (REST sensor)

`GET /api/status` needs no password. One poll fills several sensors:

```yaml
# configuration.yaml
rest:
  - resource: http://phone.local/api/status
    scan_interval: 10
    sensor:
      - name: "Rotary phone line 1"
        value_template: "{{ value_json.lines[0].state }}"
        json_attributes_path: "$.lines[0]"
        json_attributes: [last_number, caller_name, caller_number]
      - name: "Rotary phone Wi-Fi signal"
        value_template: "{{ value_json.wifi_rssi }}"
        unit_of_measurement: "dBm"
        device_class: signal_strength
      - name: "Rotary phone uptime"
        value_template: "{{ value_json.uptime_s }}"
        unit_of_measurement: "s"
        device_class: duration
      - name: "Rotary phone free memory"
        value_template: "{{ value_json.heap_free }}"
        unit_of_measurement: "B"
      - name: "Rotary phone new messages"
        value_template: "{{ value_json.new_messages }}"
    binary_sensor:
      - name: "Rotary phone line 1 in use"
        value_template: "{{ value_json.lines[0].state != 'IDLE' }}"
```

A line's `state` is one of `IDLE`, `DIALTONE`, `DIALING`, `RINGBACK`,
`RINGING`, `INCALL`, `BUSY`, `REORDER`, `PLAYING`, `RECORDING`,
`OFFHOOK_ALERT`, `PLAYING_RECORD_PROMPT`, `RECORD_BEEP`, `BT_PASSTHROUGH`.
On a two-line board, copy the line sensors with `lines[1]`.

Polling every 10 s misses short events. For anything that should trigger
an automation, use the webhook.

## 3. Controlling the board (rest_command)

If you've set a web password, keep the `username`/`password` lines (the
username can be anything). Otherwise delete them.

```yaml
# configuration.yaml
rest_command:
  # Ring a phone like a doorbell: line 0 or 1, 1-15 seconds. Never rings a
  # handset that's already lifted, or a line in a call. Ignores quiet hours
  # (it's the Diagnostics bench ring): add a time condition if you need one.
  redial_ring:
    url: http://phone.local/test/ring
    method: POST
    content_type: application/x-www-form-urlencoded
    payload: "line={{ line | default(0) }}&secs={{ secs | default(4) }}"
    username: admin
    password: !secret redial_password

  # Fire a pin output (Settings -> Dial Codes -> Pin outputs) without
  # dialing: e.g. code 7 opens the door relay.
  redial_pin:
    url: "http://phone.local/pin_action_test/{{ code }}"
    method: POST
    username: admin
    password: !secret redial_password

  # Set "New messages" back to 0 (opening the Recordings page does too).
  redial_messages_heard:
    url: http://phone.local/messages_seen
    method: POST
    username: admin
    password: !secret redial_password

  # Quiet hours on or off (calls go to the answering machine instead of ringing).
  redial_quiet_hours:
    url: http://phone.local/quiet_hours
    method: POST
    content_type: application/x-www-form-urlencoded
    payload: >-
      {{ 'enabled=1&' if on else '' }}start={{ start | default('22:00') }}&end={{ end | default('07:00') }}
    username: admin
    password: !secret redial_password
```

Used from an automation:

```yaml
- alias: "Doorbell rings the rotary phone"
  triggers:
    - trigger: state
      entity_id: binary_sensor.doorbell
      to: "on"
  actions:
    - action: rest_command.redial_ring
      data: { line: 0, secs: 3 }
```

The board answers these with a redirect to its settings page; Home
Assistant treats that as success.

## 4. MQTT with auto-discovery

Settings → Home Assistant → **MQTT**. Tick **Enabled**, enter the
broker (`homeassistant.local`, or `192.168.1.5:1883` for another port), the
broker's user and password, and Save. It connects within a few seconds. Untick
it and Save to switch it off again; nothing is allocated while it's off and
there is no reboot either way.

You need an MQTT broker; in Home Assistant that's the **Mosquitto broker**
add-on plus the MQTT integration. The board then appears under
Settings → Devices as one device, with:

| Entity | What it is |
|---|---|
| **Line 1** (and Line 2) | Sensor: `IDLE`, `DIALTONE`, `RINGING`, `INCALL` and so on. Attributes: last number, caller name, caller number |
| **Ring line 1** (and 2) | Button: rings the phone for 4 seconds, if it's on-hook and idle. Silent during quiet hours unless you untick **Ring buttons stay silent during quiet hours** on the MQTT card |
| **Phone event** | Event entity: `offhook`, `onhook`, `dial`, `wrong_number`, `ring`, `answering_machine`, `recording`, with the line and the digits or caller |
| **New messages**, **Message waiting** | How many messages (dial 211 and answering machine) arrived since someone last opened the Recordings page, and whether that's more than zero. Greetings don't count |
| **Mark messages heard** | Button: sets the count back to zero without opening the Recordings page |
| **Pin output buttons** | One per pin dial code (Settings → Dial Codes → Pin outputs): press to fire it. Home Assistant codes don't get a button; they come in as `dial` events |
| **Wi-Fi signal**, **Free memory** | Diagnostics, every 60 s |

The topics are `redial/<id>/...`, where `<id>` is `redial_` plus the last six
hex digits of the board's MAC address. Commands arrive on
`redial/<id>/cmd/ring/<line>` (payload: seconds, 1 to 15),
`redial/<id>/cmd/pin/<code>` and `redial/<id>/cmd/messages_seen`. Pin buttons
are announced when the board connects, so save the MQTT card again after
adding or removing a pin code.

**Costs.** The MQTT code adds about 50 KB to the firmware image. While
connected it holds about 21 KB of the board's free memory (around 72 KB when
idle), and gives most of it back when switched off. Leave it off if you don't
use it.

**Safety.** Anyone who can publish to the broker can ring the phone and fire
your pin outputs, so give the broker a password and don't expose it to the
internet. Only plain `mqtt://` is supported, not TLS: the encrypted
connection needs about 35 KB more memory than the board can spare.

## Limits

- **Plain HTTP, both ways.** The password and webhook ID cross your network
  unencrypted, the same as the web interface. Keep both on a trusted LAN.
- **Events are fire-and-forget.** The board queues up to 8 and drops the
  rest; if Home Assistant is down, they're lost, not retried.
  `/api/status` shows `webhook_last_status`: the HTTP status of the last
  post, or a negative number if it never got an answer.
- **MQTT and the webhook are independent.** Use either or both; they send
  the same events.

---

**Back to: [4 · Set up & use](setup.md)**
