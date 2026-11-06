"""
Claude usage bridge
Polls your claude.ai usage and pushes it to the SmallTV display on your network.

  python bridge.py          run forever (use pythonw.exe to run without a window)
  python bridge.py --test   fetch once, print everything, push once, exit

Settings live in config.json next to this file (copy config.example.json).
"""
import json
import logging
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from logging.handlers import RotatingFileHandler
from pathlib import Path

from curl_cffi import requests as creq

HERE = Path(__file__).resolve().parent
CONFIG_PATH = HERE / "config.json"
PUSH_EVERY = 60          # seconds between pushes to the display
BACKOFF_ON_AUTH = 900    # wait 15 min before retrying after an auth failure

log = logging.getLogger("bridge")


class BridgeError(Exception):
    def __init__(self, code, detail=""):
        super().__init__(f"{code}: {detail}")
        self.code = code  # "auth", "cf", "net", "api"


def load_config():
    cfg = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    for key in ("session_key", "push_token"):
        if not cfg.get(key) or cfg[key].startswith("PASTE"):
            sys.exit(f"Set '{key}' in {CONFIG_PATH}")
    cfg.setdefault("display", "smalltv.local")
    cfg["poll_seconds"] = max(int(cfg.get("poll_seconds", 180)), 120)
    return cfg


class Claude:
    def __init__(self, session_key, org_id=None):
        self.s = creq.Session(impersonate="chrome")
        self.s.cookies.set("sessionKey", session_key, domain="claude.ai")
        self.org_id = org_id

    def get(self, path):
        try:
            r = self.s.get(
                "https://claude.ai" + path,
                headers={"Accept": "application/json", "Referer": "https://claude.ai/settings/usage"},
                timeout=20,
            )
        except Exception as e:
            raise BridgeError("net", str(e))
        body = r.text or ""
        if r.status_code in (401, 403):
            if "Just a moment" in body or "cf-" in body[:2000].lower() and "<html" in body[:200].lower():
                raise BridgeError("cf", f"HTTP {r.status_code} (Cloudflare)")
            raise BridgeError("auth", f"HTTP {r.status_code}: {body[:200]}")
        if r.status_code != 200:
            raise BridgeError("api", f"HTTP {r.status_code}: {body[:200]}")
        try:
            return r.json()
        except Exception:
            raise BridgeError("cf", "non-JSON response (probably a bot check page)")

    def find_org(self):
        orgs = self.get("/api/organizations")
        if not orgs:
            raise BridgeError("api", "no organizations on this account")
        chat = [o for o in orgs if "chat" in (o.get("capabilities") or [])]
        org = (chat or orgs)[0]
        log.info("Using organisation %s (%s)", org.get("name"), org.get("uuid"))
        return org["uuid"]

    def usage(self):
        if not self.org_id:
            self.org_id = self.find_org()
        return self.get(f"/api/organizations/{self.org_id}/usage")


def parse_time(s):
    if not s:
        return None
    s = s.replace("Z", "+00:00")
    # trim fractional seconds to 6 digits for older Pythons
    if "." in s:
        head, rest = s.split(".", 1)
        frac = "".join(ch for ch in rest if ch.isdigit())
        tz = rest[len(frac):]
        s = f"{head}.{frac[:6]}{tz}"
    dt = datetime.fromisoformat(s)
    return dt if dt.tzinfo else dt.replace(tzinfo=timezone.utc)


def parse_usage(data):
    """Return {'s': (pct, reset_dt), 'w': (pct, reset_dt)} from the usage response."""
    out = {}
    for key, short in (("five_hour", "s"), ("seven_day", "w")):
        block = data.get(key) or {}
        util = block.get("utilization")
        pct = 0 if util is None else int(round(float(util)))
        out[short] = (max(0, min(pct, 100)), parse_time(block.get("resets_at")))
    if not data.get("five_hour") and not data.get("seven_day"):
        raise BridgeError("api", "unexpected usage format: " + json.dumps(data)[:300])
    return out


def push(cfg, parsed, err=""):
    fields = {"err": err}
    if parsed:
        now = datetime.now(timezone.utc)
        for short, (pct, reset) in parsed.items():
            secs = int((reset - now).total_seconds()) if reset else -1
            if reset and secs < 0:
                secs = 0
            fields[short] = pct
            fields[short + "r"] = secs
    req = urllib.request.Request(
        f"http://{cfg['display']}/u",
        data=urllib.parse.urlencode(fields).encode(),
        headers={"X-Token": cfg["push_token"]},
        method="POST",
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # LAN only, no proxy
    with opener.open(req, timeout=8) as r:
        return r.status


def setup_logging(to_console):
    log.setLevel(logging.INFO)
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s", "%Y-%m-%d %H:%M:%S")
    fh = RotatingFileHandler(HERE / "bridge.log", maxBytes=200_000, backupCount=1, encoding="utf-8")
    fh.setFormatter(fmt)
    log.addHandler(fh)
    if to_console and sys.stdout:
        ch = logging.StreamHandler(sys.stdout)
        ch.setFormatter(fmt)
        log.addHandler(ch)


def run_test(cfg):
    claude = Claude(cfg["session_key"], cfg.get("org_id"))
    print("Fetching usage from claude.ai ...")
    try:
        raw = claude.usage()
    except BridgeError as e:
        print(f"FAILED ({e.code}): {e}")
        return 1
    print("Raw response:\n" + json.dumps(raw, indent=2))
    parsed = parse_usage(raw)
    for k, name in (("s", "Session"), ("w", "Weekly")):
        pct, reset = parsed[k]
        print(f"{name}: {pct}% used, resets {reset.astimezone() if reset else 'n/a'}")
    print(f"Pushing to http://{cfg['display']}/u ...")
    try:
        print("Display answered HTTP", push(cfg, parsed))
    except Exception as e:
        print("Push FAILED:", e)
        return 1
    return 0


def run_forever(cfg):
    claude = Claude(cfg["session_key"], cfg.get("org_id"))
    parsed, err = None, ""
    next_fetch = 0.0
    log.info("Bridge started, display=%s, poll=%ss", cfg["display"], cfg["poll_seconds"])
    while True:
        now = time.time()
        if now >= next_fetch:
            try:
                parsed = parse_usage(claude.usage())
                err = ""
                next_fetch = now + cfg["poll_seconds"]
                log.info("Usage: session %s%%, weekly %s%%", parsed["s"][0], parsed["w"][0])
            except BridgeError as e:
                err = e.code
                log.warning("Fetch failed: %s", e)
                if e.code == "api":
                    claude.org_id = cfg.get("org_id")  # re-discover org next time
                next_fetch = now + (BACKOFF_ON_AUTH if e.code == "auth" else cfg["poll_seconds"])
            except Exception as e:
                err = "api"
                log.exception("Unexpected error: %s", e)
                next_fetch = now + cfg["poll_seconds"]
        try:
            push(cfg, parsed, err)
        except Exception as e:
            log.info("Display not reachable (%s)", e)
        time.sleep(PUSH_EVERY)


def main():
    test = "--test" in sys.argv
    setup_logging(to_console=test or sys.stdout is not None)
    cfg = load_config()
    if test:
        sys.exit(run_test(cfg))
    run_forever(cfg)


if __name__ == "__main__":
    main()
