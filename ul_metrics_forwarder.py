#!/usr/bin/env python3
"""
UL Metrics Forwarder - Reads metrics from pipes and forwards to InfluxDB.

Reads from two pipes:
  1. /tmp/ul_metrics.pipe - JSON from pf_ul_pipe_logger.lua (scheduler metrics)
  2. /tmp/cir.pipe - Binary from C code (time-domain Channel Impulse Response)

Usage:
    # Start forwarder (pipes are auto-created):
    python ul_metrics_forwarder.py --experiment "fading_test"
    # Creates: fading_test-0, fading_test-1, fading_test-2, etc.

    # With CIR (Channel Impulse Response) logging from C code:
    python ul_metrics_forwarder.py --experiment "fading_test" --log-cir

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
import struct
import base64
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

DEFAULT_PIPE_PATH = "/tmp/ul_metrics.pipe"
DEFAULT_CIR_PIPE_PATH = "/tmp/cir.pipe"  # C-based time-domain CIR
DEFAULT_BATCH_SIZE = 50
DEFAULT_FLUSH_INTERVAL = 0.1  # seconds

# Measurements to create
MEASUREMENT_UL_SCHED = "ul_scheduler"  # Per-UE scheduling decisions
MEASUREMENT_UL_CIR = "ul_cir"          # Time-domain CIR (from C code)

# CIR binary header format: rnti(uint16), frame(uint32), slot(uint16), ofdm_size(uint32)
CIR_HEADER_SIZE = 12
CIR_HEADER_FORMAT = '<HIHI'  # Little-endian: H=uint16, I=uint32


def find_next_experiment_number(base_name: str) -> int:
    """Query InfluxDB to find the next available experiment number."""
    # Flux query to find all experiments matching the pattern
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

        # Parse CSV response to find existing experiment names
        existing = set()
        lines = resp.text.strip().split('\n')
        for line in lines:
            # Skip annotation lines starting with # and empty lines
            if line.startswith('#') or not line.strip():
                continue
            # CSV format: ,result,table,_value (first column is empty)
            parts = line.split(',')
            if len(parts) >= 4:
                exp_name = parts[-1].strip()
                # Skip header row
                if exp_name == '_value':
                    continue
                existing.add(exp_name)

        # Find the next available number for this base name
        pattern = re.compile(rf'^{re.escape(base_name)}-(\d+)$')
        used_numbers = set()

        for exp in existing:
            match = pattern.match(exp)
            if match:
                used_numbers.add(int(match.group(1)))

        # Find first unused number
        n = 0
        while n in used_numbers:
            n += 1

        return n

    except requests.RequestException as e:
        print(f"[Forwarder] Warning: Could not connect to InfluxDB: {e}")
        return 0


class ULMetricsForwarder:
    def __init__(self, pipe_path: str, batch_size: int, flush_interval: float,
                 experiment: str = None, log_cir: bool = False,
                 sample_rate: int = 1, cir_pipe_path: str = None,
                 restart_timeout: int = 0):
        self.pipe_path = pipe_path
        self.cir_pipe_path = cir_pipe_path
        self.batch_size = batch_size
        self.flush_interval = flush_interval
        self.experiment = experiment
        self.log_cir = log_cir
        self.sample_rate = sample_rate  # Log every Nth event
        self.restart_timeout = restart_timeout  # seconds, 0 = disabled

        self.buffer = deque()
        self.running = True
        self.event_id = 0  # Incrementing event counter
        self.sample_counter = 0
        self.cir_sample_counter = 0

        # Watchdog: track last time we received data from OAI
        self.last_data_time = time.time()

        # Thread-safe queues for decoupling pipe readers from InfluxDB writer
        # This prevents InfluxDB latency from causing pipe reader to fall behind
        self.msg_queue = queue.Queue(maxsize=100000)  # ~100K messages buffer
        self.cir_queue = queue.Queue(maxsize=10000)   # ~10K CIR samples buffer

        # Reader threads
        self._pipe_reader_thread = None
        self._cir_reader_thread = None

        self.stats = {
            "messages_received": 0,
            "cir_received": 0,
            "points_generated": 0,
            "batches_sent": 0,
            "points_sent": 0,
            "errors": 0,
            "queue_drops": 0,
            "cir_queue_drops": 0,
        }
        self._session = None

    def escape_tag(self, value: str) -> str:
        """Escape special characters for InfluxDB tag values."""
        return str(value).replace(" ", "\\ ").replace(",", "\\,").replace("=", "\\=")

    def _ensure_pipe(self, pipe_path: str, name: str):
        """Create pipe if it doesn't exist."""
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
        """Initialize pipes, HTTP session, and start reader threads."""
        # Main scheduler pipe
        self._ensure_pipe(self.pipe_path, "scheduler")

        # CIR pipe (from C code)
        if self.log_cir and self.cir_pipe_path:
            self._ensure_pipe(self.cir_pipe_path, "cir")

        self._session = aiohttp.ClientSession(
            headers={
                "Authorization": f"Token {INFLUX_TOKEN}",
                "Content-Type": "text/plain",
            }
        )

        print(f"[Forwarder] InfluxDB: {INFLUX_URL}")
        print(f"[Forwarder] Experiment: {self.experiment or '(none)'}")
        print(f"[Forwarder] CIR logging: {self.log_cir}")
        if self.log_cir and self.cir_pipe_path:
            print(f"[Forwarder] CIR pipe: {self.cir_pipe_path}")
        print(f"[Forwarder] Sample rate: 1/{self.sample_rate}")
        print(f"[Forwarder] Message queue size: {self.msg_queue.maxsize}")

        # Start reader threads (they run independently of asyncio)
        self._pipe_reader_thread = threading.Thread(
            target=self._pipe_reader_thread_func,
            name="PipeReader",
            daemon=True
        )
        self._pipe_reader_thread.start()

        if self.log_cir and self.cir_pipe_path:
            self._cir_reader_thread = threading.Thread(
                target=self._cir_reader_thread_func,
                name="CIRReader",
                daemon=True
            )
            self._cir_reader_thread.start()

        if self.experiment:
            await self.write_marker("start")
            print(f"[Forwarder] Experiment '{self.experiment}' STARTED")

    async def write_marker(self, event: str):
        """Write experiment start/end marker."""
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
        """Convert JSON message to InfluxDB line protocol."""
        lines = []

        ts_ns = data.get('ts', int(time.time() * 1e9))
        frame = data.get('f', 0)
        slot = data.get('s', 0)
        mask = data.get('mask', '')
        pin = data.get('pin', None)       # 1=recompute, 0=replay (pinned scheduler only)
        pin_id = data.get('pin_id', None) # Pin window ID (pinned scheduler only)
        ues = data.get('ues', [])

        # Increment event ID for each scheduler invocation
        self.event_id += 1
        event_id = self.event_id

        exp_tag = f",experiment={self.escape_tag(self.experiment)}" if self.experiment else ""

        for ue in ues:
            rnti = ue.get('r', 0)
            uid = ue.get('uid', 0)
            ue_type = ue.get('t', 0)

            # Main scheduler decision measurement
            # Tags: rnti, uid, ue_type, fiveQI, experiment
            # Fields: all metrics + event_id
            five_qi = ue.get('qi', 0)
            tags = f"rnti={rnti},uid={uid},ue_type={ue_type},fiveQI={five_qi}{exp_tag}"

            fields = [
                f"event_id={event_id}i",
                f"frame={frame}i",
                f"slot={slot}i",
                f"pending_bytes={ue.get('pb', 0)}i",
                f"throughput={ue.get('tp', 0.0)}",
                f"bler={ue.get('bl', 0.0)}",
                f"pusch_snrx10={ue.get('snr', 0)}i",
                f"target_snrx10={ue.get('tsnr', 0)}i",
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
            ]

            if pin is not None:
                fields.append(f"pin={pin}i")
                fields.append(f"pin_id={pin_id}i")

            if mask:
                blocked_prbs = mask.count('X')
                fields.append(f'prb_mask="{mask}"')
                fields.append(f"blocked_prbs={blocked_prbs}i")

            line = f"{MEASUREMENT_UL_SCHED},{tags} {','.join(fields)} {ts_ns}"
            lines.append(line)

        return lines

    def process_message(self, json_str: str):
        """Process a JSON message from the scheduler."""
        try:
            data = json.loads(json_str)
        except json.JSONDecodeError as e:
            print(f"[Forwarder] JSON error: {e}")
            return

        self.stats["messages_received"] += 1
        self.last_data_time = time.time()

        # Apply sample rate
        self.sample_counter += 1
        if self.sample_counter < self.sample_rate:
            return
        self.sample_counter = 0

        # Convert to InfluxDB lines
        lines = self.json_to_influx_lines(data)
        self.stats["points_generated"] += len(lines)

        # Add to buffer
        for line in lines:
            self.buffer.append(line)

    def process_cir_message(self, rnti: int, frame: int, slot: int,
                            ofdm_size: int, cir_data: bytes):
        """Process a binary CIR message from the C-based CIR pipe.

        CIR data is array of c16_t (int16 real, int16 imag) - stored as base64 blob.
        """
        self.stats["cir_received"] += 1

        # Apply sample rate
        self.cir_sample_counter += 1
        if self.cir_sample_counter < self.sample_rate:
            return
        self.cir_sample_counter = 0

        ts_ns = int(time.time() * 1e9)
        exp_tag = f",experiment={self.escape_tag(self.experiment)}" if self.experiment else ""

        # Encode CIR data as base64
        cir_blob = base64.b64encode(cir_data).decode('ascii')

        # Create single InfluxDB line with CIR blob
        tags = f"rnti={rnti}{exp_tag}"
        fields = f"frame={frame}i,slot={slot}i,ofdm_size={ofdm_size}i,cir_blob=\"{cir_blob}\""
        line = f"{MEASUREMENT_UL_CIR},{tags} {fields} {ts_ns}"
        self.buffer.append(line)
        self.stats["points_generated"] += 1

    async def flush_to_influx(self):
        """Send buffered metrics to InfluxDB."""
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
            # Re-queue for retry
            for line in reversed(lines):
                self.buffer.appendleft(line)

    async def periodic_flush(self):
        """Periodically flush buffer."""
        while self.running:
            await asyncio.sleep(self.flush_interval)
            if self.buffer:
                await self.flush_to_influx()

    def _pipe_reader_thread_func(self):
        """Threaded pipe reader - runs in dedicated thread for maximum throughput.

        This thread reads from the pipe as fast as possible and queues messages
        for the asyncio event loop to process. This decouples pipe reading from
        InfluxDB write latency.
        """
        print(f"[Forwarder] Pipe reader thread started, waiting on {self.pipe_path}...")

        while self.running:
            try:
                # Blocking open - waits for writer
                fd = os.open(self.pipe_path, os.O_RDONLY)
                print("[Forwarder] Scheduler connected!")

                line_buffer = b""

                while self.running:
                    try:
                        # Single kernel read - returns immediately with available data
                        chunk = os.read(fd, 262144)
                        if chunk:
                            line_buffer += chunk
                            # Process complete lines
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
                            # EOF - writer closed pipe
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
        """Async task that drains the message queue and processes messages."""
        while self.running:
            # Process messages in batches for efficiency
            processed = 0
            try:
                while processed < 1000:  # Process up to 1000 per iteration
                    try:
                        line = self.msg_queue.get_nowait()
                        self.process_message(line)
                        processed += 1
                    except queue.Empty:
                        break

                # Flush if batch size reached
                if len(self.buffer) >= self.batch_size:
                    await self.flush_to_influx()

            except Exception as e:
                print(f"[Forwarder] Error processing messages: {e}")

            # Small yield to allow other tasks
            if processed == 0:
                await asyncio.sleep(0.001)
            else:
                await asyncio.sleep(0)  # Just yield

    def _cir_reader_thread_func(self):
        """Threaded CIR pipe reader - runs in dedicated thread.

        Binary format:
          Header (12 bytes): rnti(uint16), frame(uint32), slot(uint16), ofdm_size(uint32)
          Data: ofdm_size * 4 bytes (array of c16_t: int16 real, int16 imag)
        """
        if not self.cir_pipe_path:
            return

        print(f"[Forwarder] CIR reader thread started, waiting on {self.cir_pipe_path}...")

        while self.running:
            try:
                # Blocking open - waits for writer
                fd = os.open(self.cir_pipe_path, os.O_RDONLY)
                pipe_file = os.fdopen(fd, 'rb', buffering=1024*1024)  # 1MB buffer
                print("[Forwarder] CIR pipe connected!")

                data_buffer = b""

                while self.running:
                    try:
                        chunk = pipe_file.read(262144)  # 256KB chunks
                        if chunk:
                            data_buffer += chunk

                            # Process complete messages
                            while len(data_buffer) >= CIR_HEADER_SIZE:
                                rnti, frame, slot, ofdm_size = struct.unpack(
                                    CIR_HEADER_FORMAT, data_buffer[:CIR_HEADER_SIZE])

                                data_size = ofdm_size * 4
                                total_size = CIR_HEADER_SIZE + data_size

                                if len(data_buffer) < total_size:
                                    break

                                cir_data = data_buffer[CIR_HEADER_SIZE:total_size]
                                data_buffer = data_buffer[total_size:]

                                # Queue for async processing (tuple of parsed data)
                                try:
                                    self.cir_queue.put_nowait((rnti, frame, slot, ofdm_size, cir_data))
                                except queue.Full:
                                    self.stats["cir_queue_drops"] += 1
                                    if self.stats["cir_queue_drops"] % 1000 == 1:
                                        print(f"[Forwarder] WARNING: CIR queue full, dropped {self.stats['cir_queue_drops']} samples")
                        else:
                            # EOF
                            break
                    except IOError as e:
                        print(f"[Forwarder] CIR pipe read error: {e}")
                        break

                pipe_file.close()

            except OSError as e:
                if self.running:
                    time.sleep(1.0)

        print("[Forwarder] CIR reader thread exiting")

    async def process_cir_queue(self):
        """Async task that drains the CIR queue and processes samples."""
        if not self.log_cir:
            return

        while self.running:
            processed = 0
            try:
                while processed < 100:  # Process up to 100 per iteration
                    try:
                        rnti, frame, slot, ofdm_size, cir_data = self.cir_queue.get_nowait()
                        self.process_cir_message(rnti, frame, slot, ofdm_size, cir_data)
                        processed += 1
                    except queue.Empty:
                        break

                if len(self.buffer) >= self.batch_size:
                    await self.flush_to_influx()

            except Exception as e:
                print(f"[Forwarder] Error processing CIR: {e}")

            if processed == 0:
                await asyncio.sleep(0.001)
            else:
                await asyncio.sleep(0)

    async def print_stats(self):
        """Print periodic statistics and write heartbeat markers."""
        while self.running:
            await asyncio.sleep(10)
            cir_str = f", cir={self.stats['cir_received']}" if self.log_cir else ""
            drops_str = ""
            if self.stats['queue_drops'] > 0:
                drops_str = f", DROPPED={self.stats['queue_drops']}"
            if self.stats['cir_queue_drops'] > 0:
                drops_str += f", CIR_DROPPED={self.stats['cir_queue_drops']}"

            print(f"[Forwarder] msgs={self.stats['messages_received']}{cir_str}, "
                  f"points={self.stats['points_sent']}, "
                  f"batches={self.stats['batches_sent']}, "
                  f"errors={self.stats['errors']}, "
                  f"event_id={self.event_id}, "
                  f"queued={self.msg_queue.qsize()}, "
                  f"buffered={len(self.buffer)}{drops_str}")

            # Write heartbeat marker every 10 seconds so we know when forwarder was last alive
            await self.write_marker("heartbeat")

    async def watchdog(self):
        """Monitor data flow and restart OAI if no data received for restart_timeout seconds."""
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
                # Reset timer so we don't fire again immediately — give OAI time to come back
                self.last_data_time = time.time()

    async def cleanup(self):
        """Cleanup on exit."""
        # Drain remaining messages from queues
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

        remaining_cir = 0
        while not self.cir_queue.empty():
            try:
                rnti, frame, slot, ofdm_size, cir_data = self.cir_queue.get_nowait()
                self.process_cir_message(rnti, frame, slot, ofdm_size, cir_data)
                remaining_cir += 1
            except queue.Empty:
                break
        if remaining_cir:
            print(f"[Forwarder] Processed {remaining_cir} remaining queued CIR samples")

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
        if self.stats['queue_drops'] > 0 or self.stats['cir_queue_drops'] > 0:
            print(f"[Forwarder] WARNING: Data was dropped due to queue overflow!")

    async def run(self):
        """Main run loop."""
        await self.setup()
        try:
            tasks = [
                self.process_message_queue(),  # Drains msg_queue from reader thread
                self.periodic_flush(),
                self.print_stats(),
                self.watchdog(),
            ]
            # Add CIR queue processor if enabled
            if self.log_cir and self.cir_pipe_path:
                tasks.append(self.process_cir_queue())

            await asyncio.gather(*tasks)
        finally:
            await self.cleanup()

    def stop(self):
        """Signal stop."""
        print("\n[Forwarder] Shutting down...")
        self.running = False


def write_end_marker_sync(experiment: str):
    """Synchronous end marker writer for atexit - works even without async loop."""
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


# Global for atexit
_experiment_name = None


async def main():
    global _experiment_name
    parser = argparse.ArgumentParser(description="UL Metrics Forwarder to InfluxDB")
    parser.add_argument("--pipe", default=DEFAULT_PIPE_PATH,
                        help=f"Path to scheduler pipe (default: {DEFAULT_PIPE_PATH})")
    parser.add_argument("--cir-pipe", default=DEFAULT_CIR_PIPE_PATH,
                        help=f"Path to CIR pipe (default: {DEFAULT_CIR_PIPE_PATH})")
    parser.add_argument("--experiment", "-e", required=True,
                        help="Base experiment name (auto-increments: test -> test-0, test-1, ...)")
    parser.add_argument("--batch-size", type=int, default=DEFAULT_BATCH_SIZE,
                        help=f"Batch size for writes (default: {DEFAULT_BATCH_SIZE})")
    parser.add_argument("--flush-interval", type=float, default=DEFAULT_FLUSH_INTERVAL,
                        help=f"Flush interval in seconds (default: {DEFAULT_FLUSH_INTERVAL})")
    parser.add_argument("--log-cir", action="store_true",
                        help="Also log time-domain CIR from C pipe (base64-encoded blob)")
    parser.add_argument("--sample-rate", type=int, default=1,
                        help="Log every Nth scheduler event (default: 1 = all)")
    parser.add_argument("--restart-timeout", type=int, default=180,
                        help="Restart OAI if no data received for this many seconds (default: 180, 0 = disabled)")

    args = parser.parse_args()

    # Find next available experiment number
    base_experiment = args.experiment
    print(f"[Forwarder] Looking for next available experiment number for '{base_experiment}'...")
    next_num = find_next_experiment_number(base_experiment)
    experiment_name = f"{base_experiment}-{next_num}"
    print(f"[Forwarder] Using experiment tag: {experiment_name}")

    # Register atexit handler for cleanup (works even if async cleanup fails)
    _experiment_name = experiment_name
    atexit.register(write_end_marker_sync, experiment_name)

    forwarder = ULMetricsForwarder(
        args.pipe,
        args.batch_size,
        args.flush_interval,
        experiment_name,
        args.log_cir,
        args.sample_rate,
        args.cir_pipe if args.log_cir else None,
        args.restart_timeout,
    )

    loop = asyncio.get_event_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, forwarder.stop)

    await forwarder.run()


if __name__ == "__main__":
    asyncio.run(main())
