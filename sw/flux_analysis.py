# %% [markdown]
# # AIRDOS03B — particle flux
# Reads a `.ulg` file, sums histogram counts and plots particle flux over time.

# %%
import struct
import pandas as pd
import matplotlib.pyplot as plt

try:
    from pyulog import ULog
except ImportError:
    raise ImportError("pyulog not found — install with: pip install pyulog")

# %%
ULOG_FILE        = 'log100.ulg'
CYCLE_DURATION_S = 10
N_HIST           = 64
HIST_MIN_CHANNEL = 6   # channels 0..(N-1) are ignored; set according to detector threshold

AIRDOS_PAYLOAD_TYPE = 4
SUB_START   = 0x03
SUB_STOP    = 0x04
SUB_HIST_LO = 0x05
SUB_HIST_HI = 0x06
SUB_EVENTS  = 0x07


class _Cycle:
    def __init__(self):
        self.start_systime = None
        self.stop = None          # (tm, tm_s100, stop_systime, events_total, events_sent)
        self.hist = [None] * 64
        self.events = {}
        self.chunk_total = None

    def complete(self):
        if self.start_systime is None or self.stop is None:
            return False
        if any(v is None for v in self.hist):
            return False
        if self.stop[4] > 0:
            if self.chunk_total is None or len(self.events) < self.chunk_total:
                return False
        return True


def _process_payload(payload, cycles, records):
    if not payload:
        return
    sub = payload[0]

    if sub == SUB_START:
        if len(payload) < 5:
            return
        _, count, start_systime = struct.unpack_from('<BHH', payload)
        cycles.setdefault(count, _Cycle()).start_systime = start_systime

    elif sub == SUB_STOP:
        fmt = '<BHIBHHH'
        if len(payload) < struct.calcsize(fmt):
            return
        _, count, tm, tm_s100, stop_systime, events_total, events_sent = \
            struct.unpack_from(fmt, payload)
        cycles.setdefault(count, _Cycle()).stop = \
            (tm, tm_s100, stop_systime, events_total, events_sent)

    elif sub == SUB_HIST_LO:
        fmt = '<BH32H'
        if len(payload) < struct.calcsize(fmt):
            return
        vals = struct.unpack_from(fmt, payload)
        cyc = cycles.setdefault(vals[1], _Cycle())
        for i, v in enumerate(vals[2:]):
            cyc.hist[i] = v

    elif sub == SUB_HIST_HI:
        fmt = '<BH32H'
        if len(payload) < struct.calcsize(fmt):
            return
        vals = struct.unpack_from(fmt, payload)
        cyc = cycles.setdefault(vals[1], _Cycle())
        for i, v in enumerate(vals[2:]):
            cyc.hist[32 + i] = v

    elif sub == SUB_EVENTS:
        hdr_fmt = '<BHBBBxxx'
        if len(payload) < struct.calcsize(hdr_fmt):
            return
        _, count, chunk_index, chunk_total, n_events = \
            struct.unpack_from(hdr_fmt, payload)
        cyc = cycles.setdefault(count, _Cycle())
        cyc.chunk_total = chunk_total
        pairs = struct.unpack_from(f'<{2 * n_events}H', payload, 9)
        cyc.events[chunk_index] = [(pairs[i * 2], pairs[i * 2 + 1]) for i in range(n_events)]

    for cnt in sorted(cnt for cnt, cyc in cycles.items() if cyc.complete()):
        cyc = cycles.pop(cnt)
        tm, tm_s100, _, events_total, _ = cyc.stop
        records.append({
            'count':        cnt,
            'tm_unix':      tm + tm_s100 / 100,
            'events_total': events_total,
            'hist_sum':     sum(cyc.hist[HIST_MIN_CHANNEL:]),
            'ch0':          cyc.hist[0],
        })


# %%
u = ULog(ULOG_FILE)
tunnel = next((d for d in u.data_list if d.name == 'mavlink_tunnel'), None)
if tunnel is None:
    raise RuntimeError("No 'mavlink_tunnel' topic found in the log file.")

data         = tunnel.data
payload_keys = [f'payload[{i}]' for i in range(128)]
cycles       = {}
records      = []

for i in range(len(data['timestamp'])):
    if int(data['payload_type'][i]) != AIRDOS_PAYLOAD_TYPE:
        continue
    plen    = int(data['payload_length'][i])
    payload = bytes(int(data[k][i]) for k in payload_keys[:plen])
    _process_payload(payload, cycles, records)

df = pd.DataFrame(records).sort_values('count').reset_index(drop=True)

df['total']     = df['hist_sum'] + df['events_total']
df['flux']      = df['total']        / CYCLE_DURATION_S
df['flux_low']  = df['hist_sum']     / CYCLE_DURATION_S
df['flux_high'] = df['events_total'] / CYCLE_DURATION_S
df['flux_ch0']  = df['ch0']          / CYCLE_DURATION_S

if df['tm_unix'].max() > 1e8:
    time_axis = pd.to_datetime(df['tm_unix'], unit='s', utc=True)
    xlabel    = 'Time (UTC)'
    bar_width = pd.Timedelta('9s')
else:
    time_axis = df['count'] * CYCLE_DURATION_S
    xlabel    = 'Relative time [s]'
    bar_width = 9

# --- GPS time offset (ULog monotonic → Unix) ---
_gps_offset = None
_gps_d = next((d for d in u.data_list if d.name == 'vehicle_gps_position'), None)
if _gps_d is not None:
    _gps_offset = next(
        (utc / 1e6 - ts / 1e6
         for utc, ts in zip(_gps_d.data['time_utc_usec'], _gps_d.data['timestamp'])
         if utc > 1e12),
        None)

def _ulog_to_time(ts_us):
    unix = ts_us / 1e6 + _gps_offset
    if df['tm_unix'].max() > 1e8:
        return pd.to_datetime(unix, unit='s', utc=True)
    return unix - unix[0]

# --- altitude ---
_alt_time = _alt_m = None
_baro_d = next((d for d in u.data_list if d.name == 'vehicle_air_data'), None)
if _baro_d is not None and _gps_offset is not None:
    _alt_time = _ulog_to_time(_baro_d.data['timestamp'])
    _alt_m    = _baro_d.data['baro_alt_meter']

# --- vibrations (primary IMU) ---
_vib_time = _vib_accel = _vib_gyro = None
_primary_id = int(next(d for d in u.data_list
                       if d.name == 'sensors_status_imu'
                       ).data['accel_device_id_primary'][0])
_imu_st = next((d for d in u.data_list
                if d.name == 'vehicle_imu_status'
                and int(d.data['accel_device_id'][0]) == _primary_id), None)
if _imu_st is not None and _gps_offset is not None:
    _vib_time  = _ulog_to_time(_imu_st.data['timestamp'])
    _vib_accel = _imu_st.data['accel_vibration_metric']
    _vib_gyro  = _imu_st.data['gyro_vibration_metric']

# --- external temperature (hygrometer) ---
_hygro_time = _hygro_temp = None
_hygro_d = next((d for d in u.data_list if d.name == 'sensor_hygrometer'), None)
if _hygro_d is not None and _gps_offset is not None:
    _hygro_time = _ulog_to_time(_hygro_d.data['timestamp'])
    _hygro_temp = _hygro_d.data['temperature']

print(f"Loaded {len(df)} cycles from {ULOG_FILE}")
print(df[['count', 'hist_sum', 'events_total', 'total', 'flux']].head(10))

# %%
ALT_COLOR = 'forestgreen'

fig, axes = plt.subplots(3, 1, figsize=(14, 12), sharex=True)
fig.suptitle('AIRDOS03B — particle flux', fontsize=14)

ax = axes[0]
ax.scatter(time_axis, df['flux_low'],  label=f'Histogram (ch {HIST_MIN_CHANNEL}–63)', color='steelblue', s=8)
ax.scatter(time_axis, df['flux_high'], label='Events (ch ≥ 64)',                       color='tomato',    s=8, alpha=0.8)
ax.scatter(time_axis, df['flux_ch0'],  label='Channel 0',                              color='gold',      s=8, alpha=0.8)
ax.set_ylabel('Flux [particles / s]')
ax.set_ylim(0, 6)
ax.grid(axis='y', alpha=0.4)
ax.set_title('Particle flux per cycle (10 s)')

ax2 = axes[1]
window = max(1, len(df) // 20)
df['flux_smooth'] = df['flux'].rolling(window, center=True, min_periods=1).mean()
ax2.plot(time_axis, df['flux'],        color='gray',  alpha=0.4, linewidth=0.8, label='Raw data')
ax2.plot(time_axis, df['flux_smooth'], color='navy',             linewidth=1.8, label=f'Moving average ({window} cycles)')
ax2.set_ylim(0, 6)
ax2.set_ylabel('Flux [particles / s]')
ax2.grid(alpha=0.4)
ax2.set_title('Smoothed flux')

ax3 = axes[2]
VIB_COLOR  = 'darkorange'
TEMP_COLOR = 'mediumpurple'
if _vib_time is not None:
    ax3.plot(_vib_time, _vib_accel, color=VIB_COLOR,    linewidth=1.0, label='Accel. vibration')
    ax3.plot(_vib_time, _vib_gyro,  color='sandybrown', linewidth=0.8, alpha=0.7, label='Gyro vibration')
ax3.set_ylabel('Vibration [m/s²]', color=VIB_COLOR)
ax3.tick_params(axis='y', labelcolor=VIB_COLOR)
ax3.grid(alpha=0.3)
ax3.set_title('Vibration and temperature')
ax3.set_xlabel(xlabel)
if _hygro_time is not None:
    ax3_temp = ax3.twinx()
    ax3_temp.plot(_hygro_time, _hygro_temp, color=TEMP_COLOR, linewidth=1.2, label='Temperature (hygro)')
    ax3_temp.set_ylabel('Temperature [°C]', color=TEMP_COLOR)
    ax3_temp.tick_params(axis='y', labelcolor=TEMP_COLOR)
    h3, l3 = ax3.get_legend_handles_labels()
    ht, lt = ax3_temp.get_legend_handles_labels()
    ax3.legend(h3 + ht, l3 + lt, loc='upper right')
else:
    ax3.legend(loc='upper right')

# secondary altitude axis on both flux subplots
if _alt_time is not None:
    ax_alt0 = axes[0].twinx()
    ax_alt1 = axes[1].twinx()
    ax_alt1.sharey(ax_alt0)
    for _ax_alt in (ax_alt0, ax_alt1):
        _ax_alt.plot(_alt_time, _alt_m / 1000, color=ALT_COLOR, linewidth=1.2,
                     alpha=0.75, label='Altitude (baro)')
        _ax_alt.set_ylabel('Altitude [km]', color=ALT_COLOR)
        _ax_alt.tick_params(axis='y', labelcolor=ALT_COLOR)
    ax_alt1.set_ylabel('')
    handles0, labels0 = axes[0].get_legend_handles_labels()
    h_alt, l_alt = ax_alt0.get_legend_handles_labels()
    axes[0].legend(handles0 + h_alt, labels0 + l_alt, loc='upper left')
    handles1, labels1 = axes[1].get_legend_handles_labels()
    axes[1].legend(handles1, labels1, loc='upper left')
else:
    axes[0].legend(loc='upper right')
    axes[1].legend(loc='upper right')

plt.tight_layout()
plt.savefig('flux_graph.png', dpi=150)
plt.show()
print('Saved as flux_graph.png')

# %%
print('=== Flux statistics [particles/s] ===')
print(df['flux'].describe().to_string())
print(f"\nTotal detected: {df['total'].sum()} particles in {len(df) * CYCLE_DURATION_S} s")

# %%
import numpy as np

T_MIN, T_MAX = 2500, 12500

# relative time: from GPS unix timestamp if valid, otherwise from cycle number
if df['tm_unix'].max() > 1e8:
    df['t_rel'] = df['tm_unix'] - df['tm_unix'].iloc[0]
    _t_rel_baro_ref = df['tm_unix'].iloc[0]
else:
    df['t_rel'] = df['count'] * CYCLE_DURATION_S
    _t_rel_baro_ref = None

if _baro_d is not None and _gps_offset is not None:
    if _t_rel_baro_ref is not None:
        _baro_t_rel = _baro_d.data['timestamp'] / 1e6 + _gps_offset - _t_rel_baro_ref
    else:
        # baro time relative to boot, AIRDOS time relative to first cycle —
        # align via ULog timestamp of the first mavlink_tunnel message
        _airdos_first_ulog = tunnel.data['timestamp'][0] / 1e6
        _baro_t_rel = _baro_d.data['timestamp'] / 1e6 - _airdos_first_ulog
    df['alt_m'] = np.interp(df['t_rel'], _baro_t_rel, _baro_d.data['baro_alt_meter'])

df_win = df[(df['t_rel'] >= T_MIN) & (df['t_rel'] <= T_MAX)]

print(f"=== Smoothed flux maxima in window {T_MIN}–{T_MAX} s ===")
top = df_win.nlargest(10, 'flux_smooth')[
    ['t_rel', 'flux_smooth'] + (['alt_m'] if 'alt_m' in df_win.columns else [])
].copy()
if 'alt_m' in top.columns:
    top['alt_km'] = top['alt_m'] / 1000
    top = top.drop(columns='alt_m')
top = top.rename(columns={'t_rel': 't_rel [s]', 'flux_smooth': 'flux_smooth [p/s]', 'alt_km': 'altitude [km]'})
print(top.to_string(index=False, float_format='%.2f'))

# %%
if 'alt_m' in df.columns:
    fig2, ax = plt.subplots(figsize=(8, 7))

    ax.scatter(df['flux'],        df['alt_m'] / 1000, color='gray',  alpha=0.25, s=10, label='Raw data')
    ax.scatter(df['flux_smooth'], df['alt_m'] / 1000, color='navy',  alpha=0.6,  s=14, label='Smoothed flux')

    # mean per 1 km altitude bin
    df['alt_bin'] = (df['alt_m'] / 1000).round(0)
    binned = df.groupby('alt_bin')['flux_smooth'].mean()
    ax.plot(binned.values, binned.index, color='tomato', linewidth=2, label='Mean per 1 km bin')

    ax.set_xlabel('Flux [particles / s]')
    ax.set_ylabel('Altitude [km]')
    ax.set_title('Flux vs. altitude')
    ax.set_xlim(0, 6)
    ax.legend()
    ax.grid(alpha=0.4)

    plt.tight_layout()
    plt.savefig('flux_vs_altitude.png', dpi=150)
    plt.show()
    print('Saved as flux_vs_altitude.png')
else:
    print('Altitude not available (no GPS offset).')

# %%
