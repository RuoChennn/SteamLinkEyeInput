"""Read-only status + crop IPC capture. Does not bind OSC or touch SteamVR memory."""
import argparse
import ctypes as c
import json
import math
from pathlib import Path
import struct
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--seconds', type=float, default=30)
parser.add_argument('--out', type=Path, default=Path('build/gaze-precision/baseline.json'))
args = parser.parse_args()
kernel = c.WinDLL('kernel32', use_last_error=True)
kernel.OpenFileMappingW.argtypes = [c.c_uint32, c.c_int, c.c_wchar_p]
kernel.OpenFileMappingW.restype = c.c_void_p
kernel.MapViewOfFile.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32, c.c_uint32, c.c_size_t]
kernel.MapViewOfFile.restype = c.c_void_p
kernel.UnmapViewOfFile.argtypes = [c.c_void_p]
kernel.CloseHandle.argtypes = [c.c_void_p]
kernel.GetTickCount64.restype = c.c_uint64
samples, handle, view, mapped_pid = [], None, None, None
start = time.monotonic()
try:
    while time.monotonic()-start < args.seconds:
        try:
            status = json.loads((args.root/'steamlinkeyeinput/status.json').read_text())
        except (OSError, ValueError):
            time.sleep(.02)
            continue
        pid = status.get('pid')
        if pid != mapped_pid:
            if view: kernel.UnmapViewOfFile(view)
            if handle: kernel.CloseHandle(handle)
            handle = kernel.OpenFileMappingW(4, False, f'Local\\SteamLinkEyeInput.CropCenter.{pid}')
            view = kernel.MapViewOfFile(handle, 4, 0, 0, 80) if handle else None
            mapped_pid = pid
        sample = dict(t=time.monotonic()-start, age_ms=status.get('data_age_ms'),
                      accepted=status.get('accepted_points'), point=status.get('point'),
                      input=status.get('input'), angles=status.get('angles'),
                      raw=status.get('raw'), channel_changes=status.get('channel_changes'),
                      channel_updates=status.get('channel_updates'), channel_age_ms=status.get('channel_age_ms'),
                      status_tick=status.get('status_tick'))
        sample['direction_gain'] = status.get('direction_gain')
        sample['offset_degrees'] = [status.get('offset_yaw'), status.get('offset_pitch')]
        sample['status_file_age_ms'] = max(0, (time.time()-(args.root/'steamlinkeyeinput/status.json').stat().st_mtime)*1000)
        sample['running'] = status.get('running', False)
        if view:
            data = c.string_at(view, 80)
            seq = struct.unpack_from('<I', data, 16)[0]
            seq_after = c.c_uint32.from_address(view+16).value
            magic, version, size, publisher = struct.unpack_from('<4I', data)
            if magic == 0x45594544 and version == 1 and size == 80 and publisher == pid and not seq & 1 and seq == seq_after:
                tick, stamp, valid = struct.unpack_from('<QQI', data, 24)
                uv = struct.unpack_from('<4f', data, 44)
                now_tick = kernel.GetTickCount64()
                if valid == 1 and tick <= now_tick <= tick+150 and all(math.isfinite(x) and 0 <= x <= 1 for x in uv):
                    sample.update(crop_uv=uv, crop_tick=tick, native_stamp=stamp)
        samples.append(sample)
        time.sleep(.02)
finally:
    if view: kernel.UnmapViewOfFile(view)
    if handle: kernel.CloseHandle(handle)
args.out.parent.mkdir(parents=True, exist_ok=True)
args.out.write_text(json.dumps(dict(duration=time.monotonic()-start, samples=samples), indent=2))
valid = [s for s in samples if s['running'] and s['status_file_age_ms'] < 1500 and s.get('age_ms') is not None and 0 <= s['age_ms'] <= 150]
crops = [s['crop_uv'] for s in samples if 'crop_uv' in s]
print(json.dumps(dict(samples=len(samples), fresh_status_samples=len(valid), crop_samples=len(crops),
                     distinct_status_points=len({tuple(s['point']) for s in valid if s['point']}),
                     distinct_crop_centers=len(set(crops)),
                     crop_ranges=[(min(v[i] for v in crops), max(v[i] for v in crops)) for i in range(4)] if crops else [],
                     out=str(args.out)), indent=2))
