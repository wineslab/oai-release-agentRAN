from fastapi import FastAPI
from pydantic import BaseModel
from typing import List, Optional
import uvicorn
import time
import logging
import subprocess
import os

from telnet_utils import TelnetClient

# Constants
GNB_TMUX_SESSION = "gnb_session"
LUA_SCHED_PATH = "/OAI/pf_dl.lua"
LUA_SCHED_UL_PATH = "/OAI/pf_ul.lua"
OAI_INIT_SCRIPT = "/mnt/homefolder/OAI_scripts/initialize_oai.sh"
RESTART_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "restart_oai.sh")

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


# --- Pydantic models ---

class PowerControlCommand(BaseModel):
    uid: int
    snr_target: int
    reasoning: Optional[str] = None

class PowerControlRequest(BaseModel):
    commands: List[PowerControlCommand]

class SchedulerConfigRequest(BaseModel):
    fwa_value: int
    mtc_value: int
    reasoning: Optional[str] = None

class PrbBlockRequest(BaseModel):
    start_prb: int
    num_prbs: int

class PrbMaskSetRequest(BaseModel):
    mask: str

class PrbUeBlockRequest(BaseModel):
    uid: int
    start_prb: int
    num_prbs: int

class PrbUeMaskSetRequest(BaseModel):
    uid: int
    mask: str


# --- Telnet control layer ---

class GnbControlAPI:
    def __init__(self, telnet_host='localhost', telnet_port=9090):
        self.telnet_client = TelnetClient(telnet_host, telnet_port)
        self._initialized = False

    async def _ensure_initialized(self):
        """Ensure telnet client is connected."""
        try:
            if not self._initialized or not self.telnet_client._is_connection_alive():
                self.telnet_client.connect()
                self._initialized = True
                logger.info("Telnet client initialized successfully")
        except Exception as e:
            logger.error(f"Failed to initialize: {e}")
            self._initialized = False
            raise

    # --- Power control ---

    async def send_commands(self, commands: List[dict]):
        """Send power control commands."""
        try:
            await self._ensure_initialized()

            results = []
            for cmd in commands:
                try:
                    self.telnet_client.ul_snr_target(cmd["uid"], cmd["snr_target"])
                    results.append({
                        "uid": cmd["uid"],
                        "snr_target": cmd["snr_target"],
                        "status": "success",
                        "reasoning": cmd.get("reasoning", "")
                    })
                except ConnectionError as e:
                    self._initialized = False
                    results.append({"uid": cmd["uid"], "status": "connection_error", "message": str(e)})
                    break
                except Exception as e:
                    results.append({"uid": cmd["uid"], "status": "error", "message": str(e)})

            return {
                "status": "completed",
                "timestamp": time.time(),
                "results": results,
                "summary": {
                    "total": len(results),
                    "successful": len([r for r in results if r["status"] == "success"]),
                    "failed": len([r for r in results if r["status"] != "success"])
                }
            }
        except Exception as e:
            self._initialized = False
            return {"status": "error", "message": str(e), "timestamp": time.time()}

    # --- Scheduler config ---

    async def update_scheduler_config(self, fwa_value: int, mtc_value: int, reasoning: str = ""):
        """Update scheduler throughput limits (Mbps, 0 = no limit).

        Sets Lua global variables in the running scheduler without reloading
        the script, preserving the pipe connection and all runtime state.
        """
        try:
            await self._ensure_initialized()

            self.telnet_client.ul_scheduler_config(fwa_value, mtc_value)
            logger.info(f"Updated scheduler config: FWA={fwa_value} Mbps, MTC={mtc_value} Mbps")

            return {
                "status": "success",
                "timestamp": time.time(),
                "config": {
                    "fwa_value": fwa_value,
                    "mtc_value": mtc_value,
                    "reasoning": reasoning
                }
            }
        except Exception as e:
            self._initialized = False
            return {"status": "error", "message": str(e), "timestamp": time.time()}

    # --- PRB mask ---

    async def prbmask_block(self, start_prb: int, num_prbs: int):
        """Block a range of PRBs."""
        try:
            await self._ensure_initialized()
            self.telnet_client.prbmask_block(start_prb, num_prbs)
            return {
                "status": "success",
                "message": f"Blocked PRBs [{start_prb}, {start_prb + num_prbs})",
                "start_prb": start_prb,
                "num_prbs": num_prbs,
                "timestamp": time.time()
            }
        except Exception as e:
            self._initialized = False
            return {"status": "error", "message": str(e), "timestamp": time.time()}

    async def prbmask_unblock(self, start_prb: int, num_prbs: int):
        """Unblock a range of PRBs."""
        try:
            await self._ensure_initialized()
            self.telnet_client.prbmask_unblock(start_prb, num_prbs)
            return {
                "status": "success",
                "message": f"Unblocked PRBs [{start_prb}, {start_prb + num_prbs})",
                "start_prb": start_prb,
                "num_prbs": num_prbs,
                "timestamp": time.time()
            }
        except Exception as e:
            self._initialized = False
            return {"status": "error", "message": str(e), "timestamp": time.time()}

    async def prbmask_get(self):
        """Get current PRB mask."""
        try:
            await self._ensure_initialized()
            result = self.telnet_client.prbmask_get()
            return {"status": "success", "raw_response": result, "timestamp": time.time()}
        except Exception as e:
            self._initialized = False
            return {"status": "error", "message": str(e), "timestamp": time.time()}

    async def prbmask_set(self, mask: str):
        """Set full PRB mask ('.' = allowed, 'X' = blocked)."""
        try:
            await self._ensure_initialized()
            self.telnet_client.prbmask_set(mask)
            blocked = mask.count('X')
            return {
                "status": "success",
                "message": f"PRB mask set: {blocked}/{len(mask)} PRBs blocked",
                "mask_length": len(mask),
                "blocked_count": blocked,
                "timestamp": time.time()
            }
        except Exception as e:
            self._initialized = False
            return {"status": "error", "message": str(e), "timestamp": time.time()}

    async def prbmask_clear(self):
        """Clear PRB mask (unblock all)."""
        try:
            await self._ensure_initialized()
            self.telnet_client.prbmask_clear()
            return {"status": "success", "message": "PRB mask cleared (all PRBs allowed)", "timestamp": time.time()}
        except Exception as e:
            self._initialized = False
            return {"status": "error", "message": str(e), "timestamp": time.time()}

    # --- Per-UE PRB mask ---

    async def prbmask_ue_block(self, uid: int, start_prb: int, num_prbs: int):
        """Block a range of PRBs for a specific UE (by uid)."""
        try:
            await self._ensure_initialized()
            self.telnet_client.prbmask_ue_block(uid, start_prb, num_prbs)
            return {
                "status": "success",
                "message": f"UE uid {uid}: blocked PRBs [{start_prb}, {start_prb + num_prbs})",
                "uid": uid, "start_prb": start_prb, "num_prbs": num_prbs,
                "timestamp": time.time()
            }
        except Exception as e:
            self._initialized = False
            return {"status": "error", "message": str(e), "timestamp": time.time()}

    async def prbmask_ue_unblock(self, uid: int, start_prb: int, num_prbs: int):
        """Unblock a range of PRBs for a specific UE (by uid)."""
        try:
            await self._ensure_initialized()
            self.telnet_client.prbmask_ue_unblock(uid, start_prb, num_prbs)
            return {
                "status": "success",
                "message": f"UE uid {uid}: unblocked PRBs [{start_prb}, {start_prb + num_prbs})",
                "uid": uid, "start_prb": start_prb, "num_prbs": num_prbs,
                "timestamp": time.time()
            }
        except Exception as e:
            self._initialized = False
            return {"status": "error", "message": str(e), "timestamp": time.time()}

    async def prbmask_ue_set(self, uid: int, mask: str):
        """Set full per-UE PRB mask."""
        try:
            await self._ensure_initialized()
            self.telnet_client.prbmask_ue_apply(uid, mask)
            blocked = mask.count('X')
            return {
                "status": "success",
                "message": f"UE uid {uid}: PRB mask set, {blocked}/{len(mask)} PRBs blocked",
                "uid": uid, "mask_length": len(mask), "blocked_count": blocked,
                "timestamp": time.time()
            }
        except Exception as e:
            self._initialized = False
            return {"status": "error", "message": str(e), "timestamp": time.time()}

    async def prbmask_ue_show(self, uid: int = None):
        """Get per-UE PRB mask."""
        try:
            await self._ensure_initialized()
            result = self.telnet_client.prbmask_ue_show(uid)
            return {"status": "success", "raw_response": result, "timestamp": time.time()}
        except Exception as e:
            self._initialized = False
            return {"status": "error", "message": str(e), "timestamp": time.time()}

    async def prbmask_ue_clear(self, uid: int):
        """Clear per-UE PRB mask."""
        try:
            await self._ensure_initialized()
            self.telnet_client.prbmask_ue_clear(uid)
            return {"status": "success", "message": f"UE uid {uid}: PRB mask cleared", "uid": uid, "timestamp": time.time()}
        except Exception as e:
            self._initialized = False
            return {"status": "error", "message": str(e), "timestamp": time.time()}


# --- Helper functions ---

def check_tmux_session_exists(session_name: str) -> bool:
    try:
        result = subprocess.run(["tmux", "has-session", "-t", session_name], capture_output=True, text=True)
        return result.returncode == 0
    except Exception:
        return False

def check_gnb_running() -> dict:
    try:
        if not check_tmux_session_exists(GNB_TMUX_SESSION):
            return {"running": False, "details": "tmux session not found"}

        result = subprocess.run(
            ["tmux", "list-panes", "-t", GNB_TMUX_SESSION, "-F", "#{pane_current_command}"],
            capture_output=True, text=True
        )

        if result.returncode != 0:
            return {"running": False, "details": "failed to get session info"}

        commands = result.stdout.strip().split('\n')
        if all(cmd.strip() in ['bash', 'sh', ''] for cmd in commands):
            return {"running": False, "details": "session exists but no active processes"}

        return {"running": True, "details": f"active processes: {commands}"}
    except Exception as e:
        return {"running": False, "details": f"error checking status: {str(e)}"}


# --- FastAPI app ---

app = FastAPI(title="5G gNB Control API", version="2.0.0")
gnb_control = GnbControlAPI()


@app.post("/api/v1/power-control")
async def send_power_control(request: PowerControlRequest):
    """Send power control commands to adjust SNR targets."""
    commands = [cmd.dict() for cmd in request.commands]
    return await gnb_control.send_commands(commands)

@app.post("/api/v1/scheduler-config")
async def update_scheduler_config(request: SchedulerConfigRequest):
    """Update scheduler configuration with new FWA and MTC throughput limits."""
    return await gnb_control.update_scheduler_config(
        fwa_value=request.fwa_value,
        mtc_value=request.mtc_value,
        reasoning=request.reasoning or ""
    )

@app.post("/api/v1/prbmask/block")
async def prbmask_block(request: PrbBlockRequest):
    """Block a range of PRBs."""
    return await gnb_control.prbmask_block(start_prb=request.start_prb, num_prbs=request.num_prbs)

@app.post("/api/v1/prbmask/unblock")
async def prbmask_unblock(request: PrbBlockRequest):
    """Unblock a range of PRBs."""
    return await gnb_control.prbmask_unblock(start_prb=request.start_prb, num_prbs=request.num_prbs)

@app.get("/api/v1/prbmask")
async def prbmask_get():
    """Get current PRB mask state."""
    return await gnb_control.prbmask_get()

@app.post("/api/v1/prbmask/set")
async def prbmask_set(request: PrbMaskSetRequest):
    """Set the full PRB mask ('.' = allowed, 'X' = blocked)."""
    return await gnb_control.prbmask_set(mask=request.mask)

@app.delete("/api/v1/prbmask")
async def prbmask_clear():
    """Clear PRB mask (unblock all PRBs)."""
    return await gnb_control.prbmask_clear()

# --- Per-UE PRB mask endpoints ---

@app.post("/api/v1/prbmask/ue/block")
async def prbmask_ue_block(request: PrbUeBlockRequest):
    """Block a range of PRBs for a specific UE (by uid)."""
    return await gnb_control.prbmask_ue_block(uid=request.uid, start_prb=request.start_prb, num_prbs=request.num_prbs)

@app.post("/api/v1/prbmask/ue/unblock")
async def prbmask_ue_unblock(request: PrbUeBlockRequest):
    """Unblock a range of PRBs for a specific UE (by uid)."""
    return await gnb_control.prbmask_ue_unblock(uid=request.uid, start_prb=request.start_prb, num_prbs=request.num_prbs)

@app.post("/api/v1/prbmask/ue/set")
async def prbmask_ue_set(request: PrbUeMaskSetRequest):
    """Set the full per-UE PRB mask ('.' = allowed, 'X' = blocked)."""
    return await gnb_control.prbmask_ue_set(uid=request.uid, mask=request.mask)

@app.get("/api/v1/prbmask/ue/{uid}")
async def prbmask_ue_show(uid: int):
    """Get a specific UE's PRB mask (by uid)."""
    return await gnb_control.prbmask_ue_show(uid=uid)

@app.delete("/api/v1/prbmask/ue/{uid}")
async def prbmask_ue_clear(uid: int):
    """Clear a specific UE's PRB mask (by uid)."""
    return await gnb_control.prbmask_ue_clear(uid=uid)

@app.get("/api/v1/health")
async def health_check():
    """Basic health check."""
    return {"status": "healthy", "timestamp": time.time()}

@app.get("/api/v1/telnet-health")
async def telnet_health():
    """Check telnet connection health."""
    try:
        is_alive = gnb_control.telnet_client._is_connection_alive()
        return {
            "status": "healthy" if is_alive else "disconnected",
            "connected": is_alive,
            "host": gnb_control.telnet_client.host,
            "port": gnb_control.telnet_client.port,
            "timestamp": time.time()
        }
    except Exception as e:
        return {"status": "error", "message": str(e), "timestamp": time.time()}

@app.post("/api/v1/gnb/start")
async def start_gnb():
    """Start the gNB process in tmux session."""
    try:
        if check_tmux_session_exists(GNB_TMUX_SESSION):
            subprocess.run(["tmux", "kill-session", "-t", GNB_TMUX_SESSION], capture_output=True)
            time.sleep(1)

        command = f"export LUA_SCHED={LUA_SCHED_PATH} && export LUA_SCHED_UL={LUA_SCHED_UL_PATH} && {OAI_INIT_SCRIPT}"
        result = subprocess.run([
            "tmux", "new-session", "-d", "-s", GNB_TMUX_SESSION, "-c", "/mnt/homefolder",
            "bash", "-c", command
        ], capture_output=True, text=True)

        if result.returncode != 0:
            return {"status": "error", "message": f"Failed to create tmux session: {result.stderr}", "timestamp": time.time()}

        subprocess.run(["tmux", "pipe-pane", "-t", GNB_TMUX_SESSION, "-o", "cat >> /tmp/tmux.logs"], capture_output=True)

        time.sleep(2)
        if not check_tmux_session_exists(GNB_TMUX_SESSION):
            return {"status": "error", "message": "tmux session died immediately after creation", "timestamp": time.time()}

        return {"status": "success", "message": "gNB started successfully", "session_name": GNB_TMUX_SESSION, "timestamp": time.time()}
    except Exception as e:
        return {"status": "error", "message": str(e), "timestamp": time.time()}

@app.post("/api/v1/gnb/stop")
async def stop_gnb():
    """Stop the gNB process."""
    try:
        if not check_tmux_session_exists(GNB_TMUX_SESSION):
            return {"status": "success", "message": "gNB was not running", "timestamp": time.time()}

        subprocess.run(["tmux", "kill-session", "-t", GNB_TMUX_SESSION], capture_output=True, text=True)
        time.sleep(1)

        if check_tmux_session_exists(GNB_TMUX_SESSION):
            return {"status": "error", "message": "tmux session still exists after kill", "timestamp": time.time()}

        return {"status": "success", "message": "gNB stopped successfully", "timestamp": time.time()}
    except Exception as e:
        return {"status": "error", "message": str(e), "timestamp": time.time()}

@app.post("/api/v1/gnb/restart")
async def restart_gnb():
    """Restart the gNB by running restart_oai.sh."""
    try:
        if not os.path.isfile(RESTART_SCRIPT):
            return {"status": "error", "message": f"restart_oai.sh not found at {RESTART_SCRIPT}", "timestamp": time.time()}

        result = subprocess.run(
            ["bash", RESTART_SCRIPT],
            capture_output=True, text=True, timeout=120,
            cwd=os.path.dirname(RESTART_SCRIPT),
        )

        if result.returncode != 0:
            return {
                "status": "error",
                "message": f"restart_oai.sh failed (exit {result.returncode}): {result.stderr.strip()}",
                "stdout": result.stdout,
                "timestamp": time.time(),
            }

        gnb_control._initialized = False
        return {"status": "success", "message": "gNB restarted successfully", "stdout": result.stdout, "timestamp": time.time()}
    except subprocess.TimeoutExpired:
        return {"status": "error", "message": "restart_oai.sh timed out (120s)", "timestamp": time.time()}
    except Exception as e:
        return {"status": "error", "message": str(e), "timestamp": time.time()}

@app.get("/api/v1/gnb/status")
async def get_gnb_status():
    """Check if gNB is running."""
    try:
        status_info = check_gnb_running()
        return {
            "status": "success",
            "running": status_info["running"],
            "details": status_info["details"],
            "timestamp": time.time()
        }
    except Exception as e:
        return {"status": "error", "message": str(e), "timestamp": time.time()}

@app.get("/api/v1/gnb/logs")
async def get_gnb_logs():
    """Get recent logs from gNB tmux session."""
    try:
        if not check_tmux_session_exists(GNB_TMUX_SESSION):
            return {"status": "error", "message": "gNB tmux session not found", "timestamp": time.time()}

        result = subprocess.run(["tmux", "capture-pane", "-t", GNB_TMUX_SESSION, "-p"], capture_output=True, text=True)
        if result.returncode != 0:
            return {"status": "error", "message": f"Failed to capture pane: {result.stderr}", "timestamp": time.time()}

        return {"status": "success", "logs": result.stdout, "timestamp": time.time()}
    except Exception as e:
        return {"status": "error", "message": str(e), "timestamp": time.time()}

@app.get("/")
async def root():
    return {"message": "5G gNB Control API", "version": "2.0.0", "docs": "/docs"}


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
