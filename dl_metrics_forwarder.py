#!/usr/bin/env python3
"""
DL Metrics Forwarder - Reads metrics from pipe and forwards to InfluxDB.

Reads from /tmp/dl_metrics.pipe - JSON from pf_dl_pipe_logger.lua (scheduler metrics)

Usage:
    # Start forwarder (pipe is auto-created):
    python dl_metrics_forwarder.py --experiment "dl_test"
    # Creates: dl_test-0, dl_test-1, dl_test-2, etc.

    # Then start OAI with the pipe logger scheduler
"""

import os
import sys
import json
import asyncio
import signal
import argparse
import time
import re
import atexit
import subprocess
import requests
import threading
import queue
from pathlib import Path
from collections import deque
import aiohttp

# ============ CONFIGURATION ============
INFLUX_URL = os.environ.get("INFLUXDB_URL", "http://localhost:8086")
INFLUX_ORG = os.environ.get("INFLUXDB_ORG", "mwc")
INFLUX_BUCKET = os.environ.get("INFLUXDB_BUCKET", "mwc-live")
INFLUX_TOKEN = os.environ.get("INFLUXDB_TOKEN", "agentran-dev-token")

DEFAULT_PIPE_PATH = "/tmp/dl_metrics.pipe"
DEFAULT_BATCH_SIZE = 50
DEFAULT_FLUSH_INTERVAL = 0.1  # seconds

MEASUREMENT_DL_SCHED = "dl_scheduler"  # Per-UE scheduling decisions


def find_next_experiment_number(base_name: str) -> int:
    """Query InfluxDB to find the next available experiment number."""
    query = f'''
    import "influxdata/influxdb/schema"

    schema.tagValues(
        bucket: "{INFLUX_BUCKET}",
        tag: "experiment",
        predicate: (r) => r._measurement == "experiment_markers",
        start: -30d
    )
    '''

    url = f"{INFLUX_URL}/api/v2/query?org={INFLUX_ORG}"
    headers = {
        "Authorization": f"Token {INFLUX_TOKEN}",
        "Content-Type": "application/vnd.flux",
        "Accept": "application/csv",
    }

    try:
        resp = requests.post(url, headers=headers, data=query, timeout=10)
        if resp.status_code != 200:
            print(f"[Forwarder] Warning: Could not query existing experiments: {resp.status_code}")
            return 0

        existing = set()
        lines = resp.text.strip().split('\n')
        for line in lines:
            if line.startswith('#') or not line.strip():
                continue
            parts = line.split(',')
            if len(parts) >= 4:
                exp_name = parts[-1].strip()
                if exp_name == '_value':
                    continue
                existing.add(exp_name)

        pattern = re.compile(rf'^{re.escape(base_name)}-(\d+)$')
        used_numbers = set()

        for exp in existing:
            match = pattern.match(exp)
            if match:
                used_numbers.add(int(match.group(1)))

        n = 0
        while n in used_numbers:
            n += 1

        return n

    except requests.RequestException as e:
        print(f"[Forwarder] Warning: Could not connect to InfluxDB: {e}")
        return 0


class DLMetricsForwarder:
    def __init__(self, pipe_path: str, batch_size: int, flush_interval: float,
                 experiment: str = None, sample_rate: int = 1,
                 restart_timeout: int = 0):
        self.pipe_path = pipe_path
        self.batch_size = batch_size
        self.flush_interval = flush_interval
        self.experiment = experiment
        self.sample_rate = sample_rate
        self.restart_timeout = restart_timeout

        self.buffer = deque()
        self.running = True
        self.event_id = 0
        self.sample_counter = 0

        self.last_data_time = time.time()

        self.msg_queue = queue.Queue(maxsize=100000)

        self._pipe_reader_thread = None

        self.stats = {
            "messages_received": 0,
            "points_generated": 0,
            "batches_sent": 0,
            "points_sent": 0,
            "errors": 0,
            "queue_drops": 0,
        }
        self._session = None

    def escape_tag(self, value: str) -> str:
        return str(value).replace(" ", "\\ ").replace(",", "\\,").replace("=", "\\=")

    def _ensure_pipe(self, pipe_path: str, name: str):
        pipe = Path(pipe_path)
        if pipe.exists():
            if pipe.is_fifo():
                print(f"[Forwarder] Using existing {name} pipe: {pipe_path}")
            else:
                pipe.unlink()
                os.mkfifo(pipe_path)
                print(f"[Forwarder] Created {name} pipe: {pipe_path}")
        else:
            os.mkfifo(pipe_path)
            print(f"[Forwarder] Created {name} pipe: {pipe_path}")

    async def setup(self):
        self._ensure_pipe(self.pipe_path, "scheduler")

        self._session = aiohttp.ClientSession(
            headers={
                "Authorization": f"Token {INFLUX_TOKEN}",
                "Content-Type": "text/plain",
            }
        )

        print(f"[Forwarder] InfluxDB: {INFLUX_URL}")
        print(f"[Forwarder] Experiment: {self.experiment or '(none)'}")
        print(f"[Forwarder] Sample rate: 1/{self.sample_rate}")
        print(f"[Forwarder] Message queue size: {self.msg_queue.maxsize}")

        self._pipe_reader_thread = threading.Thread(
            target=self._pipe_reader_thread_func,
            name="PipeReader",
            daemon=True
        )
        self._pipe_reader_thread.start()

        if self.experiment:
            await self.write_marker("start")
            print(f"[Forwarder] Experiment '{self.experiment}' STARTED")

    async def write_marker(self, event: str):
        if not self.experiment or not self._session:
            return

        ts_ns = int(time.time() * 1e9)
        exp = self.escape_tag(self.experiment)
        line = f'experiment_markers,experiment={exp},event={event} value=1i {ts_ns}'

        url = f"{INFLUX_URL}/api/v2/write?org={INFLUX_ORG}&bucket={INFLUX_BUCKET}&precision=ns"
        try:
            async with self._session.post(url, data=line) as resp:
                if resp.status != 204:
                    print(f"[Forwarder] Marker write failed: {resp.status}")
        except Exception as e:
            print(f"[Forwarder] Marker error: {e}")

    def json_to_influx_lines(self, data: dict) -> list:
        lines = []

        ts_ns = data.get('ts', int(time.time() * 1e9))
        frame = data.get('f', 0)
        slot = data.get('s', 0)
        mask = data.get('mask', '')
        ues = data.get('ues', [])

        self.event_id += 1
        event_id = self.event_id

        exp_tag = f",experiment={self.escape_tag(self.experiment)}" if self.experiment else ""

        for ue in ues:
            rnti = ue.get('r', 0)
            uid = ue.get('uid', 0)
            ue_type = ue.get('t', 0)
            five_qi = ue.get('qi', 0)

            tags = f"rnti={rnti},uid={uid},ue_type={ue_type},fiveQI={five_qi}{exp_tag}"

            fields = [
                f"event_id={event_id}i",
                f"frame={frame}i",
                f"slot={slot}i",
                f"pending_bytes={ue.get('pb', 0)}i",
                f"throughput={ue.get('tp', 0.0)}",
                f"bler={ue.get('bl', 0.0)}",
                f"cqi={ue.get('cqi', 0)}i",
                f"dl_rsrp={ue.get('rsrp', 0)}i",
                f"previous_mcs={ue.get('mcs_p', 0)}i",
                f"required_rbs={ue.get('rbs_req', 0)}i",
                f"required_mcs={ue.get('mcs_req', 0)}i",
                f"allocated_rbs={ue.get('rbs_out', 0)}i",
                f"allocated_mcs={ue.get('mcs_out', 0)}i",
                f"allocated_rb_start={ue.get('rb_st', 0)}i",
                f"bwp_start={ue.get('bwp_st', 0)}i",
                f"bwp_size={ue.get('bwp_sz', 0)}i",
                f"hol_delay_us={ue.get('hol', 0)}i",
            ]

            if mask:
                blocked_prbs = mask.count('X')
                fields.append(f'prb_mask="{mask}"')
                fields.append(f"blocked_prbs={blocked_prbs}i")

            line = f"{MEASUREMENT_DL_SCHED},{tags} {','.join(fields)} {ts_ns}"
            lines.append(line)

        return lines

    def process_message(self, json_str: str):
        try:
            data = json.loads(json_str)
        except json.JSONDecodeError as e:
            print(f"[Forwarder] JSON error: {e}")
            return

        self.stats["messages_received"] += 1
        self.last_data_time = time.time()

        self.sample_counter += 1
        if self.sample_counter < self.sample_rate:
            return
        self.sample_counter = 0

        lines = self.json_to_influx_lines(data)
        self.stats["points_generated"] += len(lines)

        for line in lines:
            self.buffer.append(line)

    async def flush_to_influx(self):
        if not self.buffer:
            return

        lines = []
        while self.buffer:
            lines.append(self.buffer.popleft())

        payload = "\n".join(lines)
        url = f"{INFLUX_URL}/api/v2/write?org={INFLUX_ORG}&bucket={INFLUX_BUCKET}&precision=ns"

        try:
            async with self._session.post(url, data=payload) as resp:
                if resp.status == 204:
                    self.stats["batches_sent"] += 1
                    self.stats["points_sent"] += len(lines)
                else:
                    error_text = await resp.text()
                    print(f"[Forwarder] InfluxDB error {resp.status}: {error_text[:200]}")
                    self.stats["errors"] += 1
        except aiohttp.ClientError as e:
            print(f"[Forwarder] Connection error: {e}")
            self.stats["errors"] += 1
            for line in reversed(lines):
                self.buffer.appendleft(line)

    async def periodic_flush(self):
        while self.running:
            await asyncio.sleep(self.flush_interval)
            if self.buffer:
                await self.flush_to_influx()

    def _pipe_reader_thread_func(self):
        print(f"[Forwarder] Pipe reader thread started, waiting on {self.pipe_path}...")

        while self.running:
            try:
                fd = os.open(self.pipe_path, os.O_RDONLY)
                print("[Forwarder] Scheduler connected!")

                line_buffer = b""

                while self.running:
                    try:
                        chunk = os.read(fd, 262144)
                        if chunk:
                            line_buffer += chunk
                            while b'\n' in line_buffer:
                                line, line_buffer = line_buffer.split(b'\n', 1)
                                line = line.strip()
                                if line:
                                    try:
                                        self.msg_queue.put_nowait(line.decode('utf-8'))
                                    except queue.Full:
                                        self.stats["queue_drops"] += 1
                                        if self.stats["queue_drops"] % 1000 == 1:
                                            print(f"[Forwarder] WARNING: Queue full, dropped {self.stats['queue_drops']} messages")
                        else:
                            break
                    except IOError as e:
                        print(f"[Forwarder] Pipe read error: {e}")
                        break

                os.close(fd)

            except OSError as e:
                if self.running:
                    print(f"[Forwarder] Pipe error: {e}, reconnecting in 100ms...")
                    time.sleep(0.1)

        print("[Forwarder] Pipe reader thread exiting")

    async def process_message_queue(self):
        while self.running:
            processed = 0
            try:
                while processed < 1000:
                    try:
                        line = self.msg_queue.get_nowait()
                        self.process_message(line)
                        processed += 1
                    except queue.Empty:
                        break

                if len(self.buffer) >= self.batch_size:
                    await self.flush_to_influx()

            except Exception as e:
                print(f"[Forwarder] Error processing messages: {e}")

            if processed == 0:
                await asyncio.sleep(0.001)
            else:
                await asyncio.sleep(0)

    async def print_stats(self):
        while self.running:
            await asyncio.sleep(10)
            drops_str = ""
            if self.stats['queue_drops'] > 0:
                drops_str = f", DROPPED={self.stats['queue_drops']}"

            print(f"[Forwarder] msgs={self.stats['messages_received']}, "
                  f"points={self.stats['points_sent']}, "
                  f"batches={self.stats['batches_sent']}, "
                  f"errors={self.stats['errors']}, "
                  f"event_id={self.event_id}, "
                  f"queued={self.msg_queue.qsize()}, "
                  f"buffered={len(self.buffer)}{drops_str}")

            await self.write_marker("heartbeat")

    async def watchdog(self):
        if self.restart_timeout <= 0:
            return

        script = Path(__file__).parent / "restart_oai.sh"
        if not script.exists():
            print(f"[Watchdog] WARNING: {script} not found, watchdog disabled")
            return

        print(f"[Watchdog] Active: will restart OAI after {self.restart_timeout}s of silence")

        while self.running:
            await asyncio.sleep(10)
            elapsed = time.time() - self.last_data_time
            if elapsed >= self.restart_timeout:
                print(f"[Watchdog] No data for {elapsed:.0f}s (threshold {self.restart_timeout}s), restarting OAI...")
                try:
                    subprocess.run([str(script)], check=True, timeout=30)
                    print("[Watchdog] restart_oai.sh completed")
                except subprocess.CalledProcessError as e:
                    print(f"[Watchdog] restart_oai.sh failed: {e}")
                except subprocess.TimeoutExpired:
                    print("[Watchdog] restart_oai.sh timed out")
                self.last_data_time = time.time()

    async def cleanup(self):
        remaining_msgs = 0
        while not self.msg_queue.empty():
            try:
                line = self.msg_queue.get_nowait()
                self.process_message(line)
                remaining_msgs += 1
            except queue.Empty:
                break
        if remaining_msgs:
            print(f"[Forwarder] Processed {remaining_msgs} remaining queued messages")

        if self.buffer:
            print(f"[Forwarder] Flushing {len(self.buffer)} remaining points...")
            await self.flush_to_influx()

        if self.experiment and self._session:
            await self.write_marker("end")
            print(f"[Forwarder] Experiment '{self.experiment}' ENDED")

        if self._session:
            await self._session.close()

        print(f"[Forwarder] Final stats: {self.stats}")
        print(f"[Forwarder] Total events logged: {self.event_id}")
        if self.stats['queue_drops'] > 0:
            print(f"[Forwarder] WARNING: Data was dropped due to queue overflow!")

    async def run(self):
        await self.setup()
        try:
            tasks = [
                self.process_message_queue(),
                self.periodic_flush(),
                self.print_stats(),
                self.watchdog(),
            ]
            await asyncio.gather(*tasks)
        finally:
            await self.cleanup()

    def stop(self):
        print("\n[Forwarder] Shutting down...")
        self.running = False


def write_end_marker_sync(experiment: str):
    if not experiment:
        return

    ts_ns = int(time.time() * 1e9)
    exp = experiment.replace(" ", "\\ ").replace(",", "\\,").replace("=", "\\=")
    line = f'experiment_markers,experiment={exp},event=end value=1i {ts_ns}'

    url = f"{INFLUX_URL}/api/v2/write?org={INFLUX_ORG}&bucket={INFLUX_BUCKET}&precision=ns"
    headers = {
        "Authorization": f"Token {INFLUX_TOKEN}",
        "Content-Type": "text/plain",
    }

    try:
        resp = requests.post(url, headers=headers, data=line, timeout=5)
        if resp.status_code == 204:
            print(f"[Forwarder] Experiment '{experiment}' END marker written (atexit)")
        else:
            print(f"[Forwarder] Failed to write end marker: {resp.status_code}")
    except Exception as e:
        print(f"[Forwarder] Error writing end marker: {e}")


_experiment_name = None


async def main():
    global _experiment_name
    parser = argparse.ArgumentParser(description="DL Metrics Forwarder to InfluxDB")
    parser.add_argument("--pipe", default=DEFAULT_PIPE_PATH,
                        help=f"Path to scheduler pipe (default: {DEFAULT_PIPE_PATH})")
    parser.add_argument("--experiment", "-e", required=True,
                        help="Base experiment name (auto-increments: test -> test-0, test-1, ...)")
    parser.add_argument("--batch-size", type=int, default=DEFAULT_BATCH_SIZE,
                        help=f"Batch size for writes (default: {DEFAULT_BATCH_SIZE})")
    parser.add_argument("--flush-interval", type=float, default=DEFAULT_FLUSH_INTERVAL,
                        help=f"Flush interval in seconds (default: {DEFAULT_FLUSH_INTERVAL})")
    parser.add_argument("--sample-rate", type=int, default=1,
                        help="Log every Nth scheduler event (default: 1 = all)")
    parser.add_argument("--restart-timeout", type=int, default=180,
                        help="Restart OAI if no data received for this many seconds (default: 180, 0 = disabled)")

    args = parser.parse_args()

    base_experiment = args.experiment
    print(f"[Forwarder] Looking for next available experiment number for '{base_experiment}'...")
    next_num = find_next_experiment_number(base_experiment)
    experiment_name = f"{base_experiment}-{next_num}"
    print(f"[Forwarder] Using experiment tag: {experiment_name}")

    _experiment_name = experiment_name
    atexit.register(write_end_marker_sync, experiment_name)

    forwarder = DLMetricsForwarder(
        args.pipe,
        args.batch_size,
        args.flush_interval,
        experiment_name,
        args.sample_rate,
        args.restart_timeout,
    )

    loop = asyncio.get_event_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, forwarder.stop)

    await forwarder.run()


if __name__ == "__main__":
    asyncio.run(main())
