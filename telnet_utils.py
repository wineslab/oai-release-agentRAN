import pexpect
import time
import json
import re
from typing import Optional, Dict, List, Any
import logging

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


class TelnetClient:
    def __init__(self, host='localhost', port=9090, timeout=5, retry_count=3):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.retry_count = retry_count
        self.connection = None
        self._connected = False
        
    def _is_connection_alive(self) -> bool:
        """Check if the connection is still alive."""
        if not self.connection or not self._connected:
            return False
            
        try:
            # Send a simple command to check connection
            self.connection.sendline('')
            self.connection.expect(pexpect.TIMEOUT, timeout=0.5)
            return True
        except (pexpect.EOF, pexpect.TIMEOUT):
            return False
    
    def connect(self, force=False):
        """Connect to telnet server with reconnection support."""
        if force or not self._is_connection_alive():
            # Close existing connection if any
            self.disconnect()
            
            try:
                logger.info(f"Connecting to telnet {self.host}:{self.port}")
                self.connection = pexpect.spawn(f'telnet {self.host} {self.port}', timeout=self.timeout)
                self.connection.expect(pexpect.TIMEOUT, timeout=1)  # Wait for connection
                self._connected = True
                logger.info("Telnet connection established")
            except Exception as e:
                logger.error(f"Failed to connect: {e}")
                self._connected = False
                raise
    
    def _execute_command(self, command: str, expect_response: bool = True) -> Optional[str]:
        """Execute a command with automatic reconnection on failure."""
        for attempt in range(self.retry_count):
            try:
                # Ensure connection is alive
                if not self._is_connection_alive():
                    logger.warning("Connection lost, attempting to reconnect...")
                    self.connect(force=True)
                
                # Send command
                self.connection.sendline(command)
                
                if expect_response:
                    self.connection.expect(pexpect.TIMEOUT, timeout=0.5)
                    return self.connection.before.decode('utf-8')
                return None
                
            except pexpect.EOF:
                logger.error(f"EOF encountered on attempt {attempt + 1}/{self.retry_count}")
                self._connected = False
                if attempt < self.retry_count - 1:
                    time.sleep(1)  # Brief delay before retry
                    continue
                raise ConnectionError(f"Connection lost after {self.retry_count} attempts")
            except Exception as e:
                logger.error(f"Command execution failed: {e}")
                if attempt < self.retry_count - 1:
                    time.sleep(1)
                    continue
                raise
        
        raise ConnectionError(f"Failed to execute command after {self.retry_count} attempts")
    
    def ul_snr_target(self, uid, target_snrx10):
        """Send ul snr_target command with reconnection support."""
        command = f'ul snr_target {uid} {target_snrx10}'
        return self._execute_command(command)
    
    def ul_dump_metrics(self):
        """Send ul dump_metrics command with reconnection support."""
        return self._execute_command('ul dump_metrics')
    
    def ul_dump_filename(self, filename):
        """Send ul dump_filename command with reconnection support."""
        command = f'ul dump_filename {filename}'
        return self._execute_command(command)
    
    def disconnect(self):
        """Close the connection."""
        if self.connection:
            try:
                self.connection.close()
            except:
                pass
            self.connection = None
            self._connected = False
            logger.info("Telnet connection closed")

    def ul_load_scheduler(self, config_file_path: str):
        """Load scheduler configuration from file (full script swap)."""
        command = f"ul load_scheduler {config_file_path}"
        return self._execute_command(command)

    def ul_scheduler_config(self, fwa_max_throughput: int, mtc_max_throughput: int):
        """Set UL per-class throughput limits as Lua globals (Mbps, 0 = no limit)."""
        command = f"ul scheduler_config {fwa_max_throughput} {mtc_max_throughput}"
        return self._execute_command(command)

    def dl_load_scheduler(self, config_file_path: str):
        """Load DL scheduler configuration from file (full script swap)."""
        command = f"dl load_scheduler {config_file_path}"
        return self._execute_command(command)

    def dl_scheduler_config(self, fwa_max_throughput: int, mtc_max_throughput: int):
        """Set DL per-class throughput limits as Lua globals (Mbps, 0 = no limit)."""
        command = f"dl scheduler_config {fwa_max_throughput} {mtc_max_throughput}"
        return self._execute_command(command)

    def prbmask_block(self, start_prb: int, num_prbs: int):
        """Block a range of PRBs."""
        command = f"prbmask blockrb {start_prb} {num_prbs}"
        return self._execute_command(command)

    def prbmask_unblock(self, start_prb: int, num_prbs: int):
        """Unblock a range of PRBs."""
        command = f"prbmask unblockrb {start_prb} {num_prbs}"
        return self._execute_command(command)

    def prbmask_get(self):
        """Get current PRB mask state."""
        return self._execute_command("prbmask show")

    def prbmask_set(self, mask_string: str):
        """Set the full PRB mask ('.' = allowed, 'X' = blocked)."""
        command = f"prbmask apply {mask_string}"
        return self._execute_command(command)

    def prbmask_clear(self):
        """Clear PRB mask (unblock all PRBs)."""
        return self._execute_command("prbmask clear")

    # --- Per-UE PRB mask ---

    def prbmask_ue_block(self, uid: int, start_prb: int, num_prbs: int):
        """Block a range of PRBs for a specific UE (by uid)."""
        command = f"prbmask ueblock {uid} {start_prb} {num_prbs}"
        return self._execute_command(command)

    def prbmask_ue_unblock(self, uid: int, start_prb: int, num_prbs: int):
        """Unblock a range of PRBs for a specific UE (by uid)."""
        command = f"prbmask ueunblock {uid} {start_prb} {num_prbs}"
        return self._execute_command(command)

    def prbmask_ue_apply(self, uid: int, mask_string: str):
        """Set the full per-UE PRB mask ('.' = allowed, 'X' = blocked)."""
        command = f"prbmask ueapply {uid} {mask_string}"
        return self._execute_command(command)

    def prbmask_ue_show(self, uid: int = None):
        """Get per-UE PRB mask state. If uid is None, show all UEs."""
        if uid is not None:
            command = f"prbmask ueshow {uid}"
        else:
            command = "prbmask ueshow"
        return self._execute_command(command)

    def prbmask_ue_clear(self, uid: int):
        """Clear per-UE PRB mask (unblock all PRBs for this UE)."""
        command = f"prbmask ueclear {uid}"
        return self._execute_command(command)


def parse_metrics_file(filename: str) -> Dict[str, Any]:
    """Parse the metrics file handling duplicate keys."""
    try:
        with open(filename, "r") as f:
            content = f.read()
    except FileNotFoundError:
        logger.error(f"Metrics file not found: {filename}")
        return {"timestamp": None, "metrics": []}
    except Exception as e:
        logger.error(f"Error reading metrics file: {e}")
        return {"timestamp": None, "metrics": []}
    
    # Parse the JSON structure manually to handle duplicate keys
    
    # Extract timestamp
    timestamp_match = re.search(r'"timestamp":\s*(\d+)', content)
    timestamp = int(timestamp_match.group(1)) if timestamp_match else None
    
    # Extract all metric entries
    metrics_list = []
    
    # Find all metric blocks (between curly braces after an RNTI key)
    metric_pattern = r'"([0-9a-fA-F]+)":\s*\{([^}]+)\}'
    
    for match in re.finditer(metric_pattern, content):
        rnti = match.group(1)
        metric_content = match.group(2)
        
        # Parse individual metric values
        metric = {"rnti": rnti}
        
        # Extract all key-value pairs from the metric
        for field_match in re.finditer(r'"(\w+)":\s*([0-9.]+|"[^"]*")', metric_content):
            key = field_match.group(1)
            value = field_match.group(2)
            
            # Remove quotes and convert to appropriate type
            if value.startswith('"'):
                metric[key] = value.strip('"')
            elif '.' in value:
                metric[key] = float(value)
            else:
                metric[key] = int(value)
        
        metrics_list.append(metric)
    
    return {
        "timestamp": timestamp,
        "metrics": metrics_list
    }


def organize_metrics_by_ue(metrics_list: List[Dict[str, Any]]) -> Dict[str, List[Dict[str, Any]]]:
    """
    Organize metrics by RNTI and sort them temporally.
    Handles frame number wraparound.
    """
    # Group metrics by RNTI
    ue_metrics = {}
    
    for metric in metrics_list:
        rnti = metric['rnti']
        if rnti not in ue_metrics:
            ue_metrics[rnti] = []
        ue_metrics[rnti].append(metric)
    
    # Sort metrics for each UE temporally
    for rnti, metrics in ue_metrics.items():
        if not metrics:
            continue
            
        # Detect if there's a wraparound
        frames = [m.get('frame', 0) for m in metrics]
        max_frame = max(frames)
        min_frame = min(frames)
        
        # If there's a large gap, we likely have a wraparound
        # Assuming frame numbers are in a reasonable range (e.g., 0-1023 for LTE)
        has_wraparound = (max_frame - min_frame) > 500
        
        if has_wraparound:
            # Define a threshold to separate "before wrap" and "after wrap"
            threshold = (max_frame + min_frame) // 2
            
            def sort_key(metric):
                frame = metric.get('frame', 0)
                slot = metric.get('slot', 0)
                
                # If frame is small (after wrap), add a large offset
                if frame < threshold:
                    adjusted_frame = frame + 10000  # Large offset to put after wrap
                else:
                    adjusted_frame = frame
                
                # Combine frame and slot for sorting
                return (adjusted_frame, slot)
        else:
            # No wraparound, simple sorting
            def sort_key(metric):
                return (metric.get('frame', 0), metric.get('slot', 0))
        
        # Sort the metrics
        ue_metrics[rnti] = sorted(metrics, key=sort_key)
    
    return ue_metrics

# Example usage
if __name__ == "__main__":
    client = TelnetClient()
    
    try:
        fname = "/tmp/test.txt"
        client.connect()
        client.ul_dump_filename(fname)

        while True:
            time.sleep(1)
            client.ul_dump_metrics()
            
            try:
                # Parse the metrics file
                metrics_dict = parse_metrics_file(fname)
                
                if metrics_dict["metrics"]:
                    print("Total metrics count:", len(metrics_dict["metrics"]))
                    
                    # Filter out metrics where fiveQI is 0
                    filtered_metrics = [
                        metric for metric in metrics_dict["metrics"] 
                        if metric.get("fiveQI", 0) != 0
                    ]
                    
                    print("Filtered metrics count:", len(filtered_metrics))
                    
                    # Organize by UE
                    ue_metrics = organize_metrics_by_ue(filtered_metrics)
                    
                    print(f"\nNumber of UEs: {len(ue_metrics)}")
                    
                    # Print organized metrics
                    for rnti, metrics in ue_metrics.items():
                        print(f"\n{'='*50}")
                        print(f"UE RNTI: {rnti}")
                        print(f"Number of metrics: {len(metrics)}")
                        print(f"{'='*50}")
                        
                        for i, metric in enumerate(metrics):
                            print(f"\n  [{i+1}] Frame: {metric.get('frame', 'N/A')}, Slot: {metric.get('slot', 'N/A')}")
                            print(f"      SNR: {metric.get('snr', 'N/A')}, MCS: {metric.get('mcs', 'N/A')}")
                            print(f"      Throughput: {metric.get('throughput', 'N/A')} Mbps")
                            print(f"      BLER: {metric.get('bler', 'N/A')}")
                            print(f"      Pending bytes: {metric.get('pending_bytes', 'N/A')}")
                            print(f"      5QI: {metric.get('fiveQI', 'N/A')}")
                    
                    # Store the organized metrics back in the dict
                    metrics_dict["metrics_by_ue"] = ue_metrics
                    
            except Exception as e:
                print(f"Error parsing metrics: {e}")
                import traceback
                traceback.print_exc()
                
            break
        
    finally:
        client.disconnect()