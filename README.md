# pyLabHub

**pyLabHub** is a modular framework for laboratory data acquisition, hardware control, and experiment management.  
Its design revolves around three key components: a **central hub** for data streaming and persistence, **adapters** that bridge diverse hardware into a unified interface, and **connectors** that integrate with external programs such as Igor Pro, Python GUIs, or custom clients.

The core principle of pyLabHub is **data integrity and reproducibility**. It isolates raw experiment data from downstream analysis, ensuring the original record remains uncompromised. At the same time, it provides flexible tools for visualization, real‑time interaction, and automation through scripts that respond dynamically to streaming data. This separation of concerns allows experiments to be both reproducible and openly shareable.

---

## 🎯 Target Use Cases

- **Lab automation**: coordinate instruments and sensors in real‑time experiments
- **Real‑time sensing & control**: stream measurements while dynamically adjusting hardware
- **Data archiving**: capture raw experiment data in open, long‑term formats
- **Collaborative research**: share data and analyses without compromising original records
- **Custom interfaces**: integrate with Igor Pro, Jupyter notebooks, or new GUIs tailored to specific experiments

---

## ✨ Features

- **Central Data Hub**: low-latency pub/sub bus with persistence
- **Adapters**: unified drivers for sensors, actuators, DAQs, and instruments
- **Connectors**: bridges to existing software (Igor Pro, Python, Jupyter, GUIs)
- **Persistence**: save experiments in scientific formats (HDF5, Parquet, Zarr, CSV)
- **Extensible**: add new hardware or software integrations with minimal effort
- **Data Integrity**: isolate raw data from analysis to ensure experiments remain uncompromised
- **Reproducibility**: keep data open and shareable while preserving the original experimental record

---

## 📦 Data Persistence Strategy

pyLabHub will use a hybrid approach to balance efficiency, openness, and reproducibility:

- **Zarr** for multidimensional array streams (e.g., waveforms, images). Data is chunked by time and channel, compressed (e.g., Blosc + Zstd), and stored in a directory layout that works locally and in cloud object stores.
- **Parquet** for event logs, metadata, and control records. Appended in time-sliced files that are easy to query with pandas/Arrow/DuckDB.
- **HDF5 export** provided for compatibility with existing scientific tools, while keeping Zarr/Parquet as the native formats.

This layout ensures raw data is stored separately from analysis, remains uncompromised, and is shareable in open, well‑supported formats.

### Example directory layout
```
runs/
  run-2025-09-18-001/
    arrays/
      raw_adc.zarr/
      camera_frames.zarr/
    events/
      control-20250918-120000.parquet
      telemetry-20250918-120000.parquet
    manifest.yaml   # metadata, checksums, provenance
```

### Run manifest (`manifest.yaml`) — Draft schema
```yaml
schema_version: 0.1
run:
  id: run-2025-09-18-001
  created_at: "2025-09-18T12:00:00Z"
  investigator: "Alex Researcher"
  organization: "Example University"
  award_number: null  # e.g., NSF-XXXXXX
  description: "NI-DAQ step response test"
identifiers:
  dataset_pid: null     # DOI/ARK assigned on publish
  orcids: []            # contributor ORCIDs
  ror: null             # institution ROR
acquisition:
  timebase:
    source_clock_hz: 1e9
    hub_clock_offset_s: 0.000123
  instruments:
    - adapter_id: "hm-ni6321-01"
      driver: "ni6321"
      version: "0.1.0"
      calibration: "cal/ni6321-2025-09-01.yaml"
  streams:
    - name: raw_adc
      kind: array
      format: zarr
      path: arrays/raw_adc.zarr
      dtype: int16
      dims: [time, channel]
      sample_rate_hz: 200000
      channels: 4
      units: V
      chunking: {time: 2000000, channel: 4}
      compression: blosc-zstd
      t_start: "2025-09-18T12:00:00.000000Z"
      t_end:   "2025-09-18T12:10:00.000000Z"
      sha256: "<tree-hash-of-zarr>"
    - name: control
      kind: events
      format: parquet
      path: events/control-20250918-120000.parquet
      rows: 15234
      sha256: "<file-hash>"
provenance:
  software:
    pylabhub: "0.1.0"
    python: "3.13.0"
    git_commit: "abcdef1"
    container_digest: "sha256:..."
    conda_lock: "sha256:..."
  recipe:
    name: "laser_scan_v1"
    parameters:
      rate_hz: 200000
      gain_db: 20
integrity:
  manifest_sha256: "<self-hash>"
  size_bytes_total: 123456789
  checks:
    last_verified_at: "2025-09-18T12:15:00Z"
    ok: true
sharing:
  access: open           # open|embargoed|controlled
  embargo_until: null
  license: CC-BY-4.0
  notes: null
```

**Field notes**
- `schema_version` lets the manifest evolve without breaking readers.
- `streams` includes both array and event stores with paths, shapes, rates, and fixity (`sha256`).
- `identifiers` & `sharing` hold PIDs/access when published; they can be `null` during acquisition.
- Timestamps (`t_start`, `t_end`) and `timebase` make time alignment explicit across devices.
- `provenance.software` and `recipe` capture the environment and parameters for reproducibility.

---

## 🛠 Roadmap

- **MVP Hub**: establish the central data hub with pub/sub and basic persistence
- **First Adapter**: implement a mock hardware adapter and one real device driver (e.g., DAQ)
- **First Connector**: create a connector for Igor Pro or a simple Python GUI
- **Persistence v1**: add support for saving runs in open formats (Parquet/Zarr)
- **Safety & Control**: introduce scheduling, interlocks, and safe‑stop routines
- **Cluster Ready**: enable multi‑host operation with secure connections

---

## 🔏 Additional Features to Integrate/Consider (NSF/NIH‑aligned)

*These are post‑MVP enhancements. The current design (raw/analysis separation, Zarr/Parquet layout, run manifests) is intended to make these easy to add later.*

- **Persistent Identifiers & Rich Metadata**: extend `manifest.yaml` to include dataset DOI/ARK, award/grant numbers, ORCID for contributors, ROR for institution, data license (e.g., CC‑BY/CC0), instrument calibration, software versions, git commit, environment hash, and units/sampling info.
- **Repository Export Tooling**: CLI to package a run (Zarr/Parquet + manifest) for deposit to discipline‑specific or generalist repositories; include mappings to common schemas (e.g., DataCite/schema.org) and generate checksums/fixity.
- **Access Tiers & Privacy Controls**: per‑run flags for `open | embargoed | controlled`, embargo dates, and optional de‑identification workflow with consent/use‑restriction notes (for human/regulated data).
- **Provenance Capture**: record analysis lineage (inputs → transforms → outputs) using a lightweight model (e.g., W3C PROV/RO‑Crate); store pipeline configs, container/image digests, and exact Python environment (lockfile) alongside outputs.
- **Data Integrity & Retention**: periodic fixity checks (hash manifests), retention schedules, and optional replication to secondary storage/object store.
- **DMP Boilerplate Generator**: render a Data Management & Sharing Plan from project settings (formats, repositories, timelines, access level, license) for use in NSF/NIH submissions.
- **CITATION & Codemeta**: include `CITATION.cff` for software citation and optional `codemeta.json` for machine‑readable project metadata.

---

## ⚙️ Configuration Preview (post‑MVP)

A small, human‑readable **`config.yaml`** will hold settings that enable NSF/NIH‑aligned exports without changing the storage core. Example:

```yaml
project:
  name: pylabhub-demo
  organization: Example University
  ror: ""              # Research Organization Registry ID (optional)
  award_number: "NSF-XXXXXXX"  # or NIH grant
  contacts:
    - name: Alex Researcher
      orcid: "0000-0000-0000-0000"

storage:
  base_dir: ./runs
  formats:
    arrays: zarr
    events: parquet
  compression:
    zarr_codec: blosc-zstd
    zarr_level: 5

identifiers:
  dataset_pid_scheme: doi   # doi | ark | none
  dataset_pid: null         # filled on publish

sharing:
  repository:
    target: zenodo          # or domain-specific repo
    community: example          # optional
    access: open            # open | embargoed | controlled
    embargo_until: null     # e.g., 2026-01-01
    license: CC-BY-4.0

provenance:
  record_git_commit: true
  record_conda_lock: true
  record_container_digest: true
  analysis_lineage: prov    # enable PROV/RO-Crate style tracking

export:
  hdf5: true                # provide HDF5 export package alongside native store
  include_checksums: true
```

This keeps **raw/analysis separation** intact while making it straightforward to: assign PIDs, export to repositories, control access tiers/embargo, and capture provenance.

---

## 📂 Project Structure

```
pylabhub/
├── hub/          # central data hub
├── adapters/     # hardware I/O interfaces
├── connectors/   # integration with external software
├── examples/     # sample scripts and configs
├── docs/         # documentation
├── tests/        # unit + integration tests
```

---

## 🚀 Getting Started

Clone the repository:

```bash
git clone https://github.com/YOUR-USERNAME/pylabhub.git
cd pylabhub
```

Install in development mode:

```bash
pip install -e .
```

Run the simple loopback example:

```bash
python examples/simple_loopback.py
```

---

## 🤝 Contributing

At this stage, the focus is on building a working framework. Once the core hub, a first adapter, and a connector are stable, contribution guidelines will be added here. Until then, feedback and suggestions are welcome through issues.

---

## 📖 Documentation

Work in progress — see the [`docs/`](docs/) folder for drafts.  
Future docs will cover APIs, schema design, and hardware integration guides.

---

## 📜 License

This project is licensed under the [BSD 3-Clause License](LICENSE).  
© 2025 Quan Qing, Arizona State University
