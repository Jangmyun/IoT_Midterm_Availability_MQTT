# Test Scenarios

`test/` is now split by purpose instead of keeping every case inside one large shell script.

## Layout

- `lib/`: shared helpers for UUID generation, MQTT publish/subscribe, process control, and payload builders
- `publish/`: topic-level smoke tests for the web client and message contract
- `edge-cases/`: malformed or regression-oriented payload tests
- `integration/`: real `core_broker` and `edge_broker` process tests against a broker
- `test_pub.sh`: small dispatcher so the old one-command workflow still works

## Quick Start

Local broker:

```bash
./test/test_pub.sh topology
./test/test_pub.sh all
./test/test_pub.sh duplicate_event
./test/test_pub.sh integration_bootstrap
```

Remote broker or non-localhost topology:

```bash
MQTT_HOST=192.168.0.40 MQTT_PORT=1883 \
CORE_A_IP=192.168.0.40 CORE_B_IP=192.168.0.41 \
NODE_1_IP=192.168.0.51 NODE_2_IP=192.168.0.52 \
./test/test_pub.sh topology
```

Run an integration case against a remote MQTT broker:

```bash
MQTT_HOST=192.168.0.40 MQTT_PORT=1883 \
EDGE_CORE_HOST=192.168.0.40 EDGE_CORE_PORT=1883 \
BUILD_DIR=./build \
./test/test_pub.sh integration_ping_pong
```

If your broker requires auth, export `MQTT_USERNAME` and `MQTT_PASSWORD`.

## Automated Cases

| ID | Script | Source | What it validates | Current repo status |
| --- | --- | --- | --- | --- |
| PUB-01 | `publish/01_topology.sh` | M-04, FR-11 | Retained topology payload for graph/cards | usable now |
| PUB-02 | `publish/02_event_intrusion.sh` | D-01 | High-priority intrusion event rendering | usable now |
| PUB-03 | `publish/03_event_motion.sh` | D-02 | Building-scoped motion event rendering | usable now |
| PUB-04 | `publish/04_event_door_forced.sh` | D-02 | Door-forced event type coverage | usable now |
| PUB-05 | `publish/05_node_down.sh` | A-01 | Node offline alert banner | usable now |
| PUB-06 | `publish/06_node_up.sh` | A-02 | Node recovery alert banner | usable now |
| PUB-07 | `publish/07_core_switch.sh` | A-03, FR-14 | Client failover banner trigger | usable now |
| PUB-08 | `publish/08_core_lwt.sh` | W-01 | Synthetic core-down notification for client | usable now |
| EC-01 | `edge-cases/01_duplicate_event.sh` | FR-02 | Duplicate `msg_id` handling on the client | usable now |
| EC-02 | `edge-cases/02_stale_topology.sh` | FR-09 | Client-side stale topology guard while connected | usable now |
| EC-03 | `edge-cases/03_invalid_event_uuid.sh` | parser contract | Invalid UUID should be dropped | usable now |
| EC-04 | `edge-cases/04_missing_priority.sh` | optional field contract | Missing priority should remain acceptable | usable now |
| INT-01 | `integration/01_core_edge_bootstrap.sh` | 5.1, 5.3 | Core connect, topology publish, edge registration publish | usable now |
| INT-02 | `integration/02_edge_ping_pong.sh` | 5.6, M-01, M-02 | Real edge ping/pong response path | usable now |
| INT-03 | `integration/03_core_lwt_payload.sh` | 5.4, FR-04 | Core abnormal exit emits W-01 with backup endpoint | usable now |
| INT-04 | `integration/04_edge_lwt_emission.sh` | 5.5, W-02 | Edge abnormal exit emits node LWT | usable now |

## Planned But Not Fully Automatable Yet

The PRD covers several scenarios that the current codebase still lists as incomplete in `PRD.md` section 14.2. Those should stay in the test plan, but the repo snapshot does not yet implement enough behavior for a passing automated test.

- `FR-03`: End-to-end `CCTV -> Edge -> Core -> Client` event forwarding through the real edge path
- `FR-05`: Backup core failover after receiving `W-01`
- `FR-06`: Core-side `campus/alert/node_down/<id>` publish triggered by real edge/node LWT
- `FR-07`: Store-and-forward queue flush after reconnect
- `FR-08`: RTT measurement plus relay-node choice

Recommended future folders when those features land:

- `integration/failover/`
- `integration/store-forward/`
- `integration/relay/`

## Useful Env Vars

- `MQTT_HOST`, `MQTT_PORT`: broker target for `mosquitto_pub` and `mosquitto_sub`
- `CORE_A_IP`, `CORE_B_IP`, `NODE_1_IP`, `NODE_2_IP`: values embedded inside published topology payloads
- `BUILD_DIR`: default binary lookup path for integration tests
- `CORE_BINARY`, `EDGE_BINARY`: explicit binary paths if you do not use the default build layout
- `EDGE_NODE_PORT`: local edge port argument for `edge_broker`
- `BACKUP_CORE_ID`, `BACKUP_CORE_IP`, `BACKUP_CORE_PORT`: core LWT integration-test values

## Notes

- `publish/99_clear_topology.sh` clears the retained topology topic if you want a clean broker state.
- `integration/*` scripts create a temporary run directory and clean it on exit. If a script fails, it prints the relevant log tail before exiting.
- `test/test_pub.sh list` prints the available case names.
