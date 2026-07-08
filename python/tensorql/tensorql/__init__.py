"""TensorQL snapshot reader for skibidi-ql.

The public class intentionally yields already-batched records so PyTorch's
DataLoader can run with ``batch_size=None`` and avoid per-row Python object
traffic in the hot path.
"""

from __future__ import annotations

import json
import math
import os
import struct
from pathlib import Path
from typing import Any, Dict, Iterable, Iterator, List, Optional, Tuple

try:  # PyTorch is optional for tests and lightweight inspection.
    import torch
    from torch.utils.data import IterableDataset
except Exception:  # pragma: no cover - depends on user env
    torch = None

    class IterableDataset:  # type: ignore[no-redef]
        pass


PAGE_SIZE = 4096
PAGE_MAGIC = 0x534B5047


def _fnv1a64(text: str) -> int:
    value = 1469598103934665603
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def _mix64(value: int) -> int:
    value ^= value >> 33
    value = (value * 0xFF51AFD7ED558CCD) & 0xFFFFFFFFFFFFFFFF
    value ^= value >> 33
    value = (value * 0xC4CEB9FE1A85EC53) & 0xFFFFFFFFFFFFFFFF
    value ^= value >> 33
    return value & 0xFFFFFFFFFFFFFFFF


def _snapshot_hash(seed: int, epoch: int, key: str) -> int:
    return _mix64(seed ^ ((epoch * 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF) ^ _fnv1a64(key))


def _catalog_path(db_path: os.PathLike[str] | str) -> Path:
    root = Path(db_path)
    if root.is_file():
        return root
    native = root / "catalog.json"
    if native.exists():
        return native
    legacy = root / ".skibidi_catalog.json"
    if legacy.exists():
        return legacy
    raise FileNotFoundError(f"no skibidi catalog found under {root}")


def _read_u16(page: bytes, offset: int) -> int:
    return struct.unpack_from("<H", page, offset)[0]


def _decode_tuple(payload: bytes) -> List[Any]:
    offset = 0
    count = struct.unpack_from("<H", payload, offset)[0]
    offset += 2
    values: List[Any] = []
    for _ in range(count):
        tag = payload[offset]
        offset += 1
        if tag == 0:  # NULL
            values.append(None)
        elif tag == 1:  # INTEGER
            values.append(struct.unpack_from("<q", payload, offset)[0])
            offset += 8
        elif tag == 2:  # REAL
            values.append(struct.unpack_from("<d", payload, offset)[0])
            offset += 8
        elif tag == 3:  # TEXT
            length = struct.unpack_from("<I", payload, offset)[0]
            offset += 4
            values.append(payload[offset : offset + length].decode("utf-8"))
            offset += length
        elif tag == 4:  # BOOLEAN
            values.append(payload[offset] != 0)
            offset += 1
        elif tag == 5:  # BLOB
            length = struct.unpack_from("<I", payload, offset)[0]
            offset += 4
            values.append(payload[offset : offset + length])
            offset += length
        else:
            raise ValueError(f"unknown tuple field tag {tag}")
    return values


class TensorQLDataset(IterableDataset):
    """Already-batched iterable over a skibidi-ql training snapshot.

    Example:

    ```python
    from tensorql import TensorQLDataset
    from torch.utils.data import DataLoader

    ds = TensorQLDataset("my_native_db", dataset="train_v1", batch_size=256)
    loader = DataLoader(ds, batch_size=None, num_workers=4)
    ```
    """

    def __init__(
        self,
        db_path: os.PathLike[str] | str,
        *,
        dataset: str,
        batch_size: int = 256,
        split: str = "train",
        epoch: int = 0,
        rank: int = 0,
        world_size: int = 1,
    ) -> None:
        if batch_size <= 0:
            raise ValueError("batch_size must be positive")
        if world_size <= 0:
            raise ValueError("world_size must be positive")
        if rank < 0 or rank >= world_size:
            raise ValueError("rank must be in [0, world_size)")

        self.catalog_file = _catalog_path(db_path)
        self.root = self.catalog_file.parent
        with self.catalog_file.open("r", encoding="utf-8") as handle:
            self.catalog = json.load(handle)
        self.snapshot = self.catalog["snapshots"][dataset]
        self.table = self.catalog["tables"][self.snapshot["source_table"]]
        self.dataset = dataset
        self.batch_size = batch_size
        self.split = split
        self.epoch = epoch
        self.rank = rank
        self.world_size = world_size

        self.columns = [column["name"] for column in self.table["columns"]]
        self.column_index = {name: i for i, name in enumerate(self.columns)}
        self.features = [feature["name"] for feature in self.snapshot["features"]]
        self.feature_specs = {
            feature["name"]: feature.get("spec", "")
            for feature in self.snapshot["features"]
        }
        self.label = self.snapshot["label"]["name"]
        rows = [row for row in self.snapshot["rows"] if row["split"] == split]
        seed = int(self.snapshot["seed"])
        rows.sort(key=lambda row: (_snapshot_hash(seed, epoch, row["rowid"]), row["rowid"]))
        self.rows = [
            row for index, row in enumerate(rows)
            if index % world_size == rank
        ]
        self.heap_path = self.root / "tables" / f"{self.snapshot['source_table']}.heap"

    def __len__(self) -> int:
        return math.ceil(len(self.rows) / self.batch_size)

    def __iter__(self) -> Iterator[Dict[str, Any]]:
        rows = self.rows
        if torch is not None:
            worker = torch.utils.data.get_worker_info()
            if worker is not None:
                rows = rows[worker.id :: worker.num_workers]

        for start in range(0, len(rows), self.batch_size):
            yield self._batch(rows[start : start + self.batch_size])

    def explain_batch(self, batch: int) -> Dict[str, Any]:
        start = batch * self.batch_size
        rows = self.rows[start : start + self.batch_size]
        return {
            "dataset": self.dataset,
            "batch": batch,
            "samples": len(rows),
            "source_rows": [row["rowid"] for row in rows],
            "split": self.split,
            "seed": int(self.snapshot["seed"]),
            "epoch": self.epoch,
            "rank": self.rank,
            "world_size": self.world_size,
        }

    def _batch(self, rows: List[Dict[str, str]]) -> Dict[str, Any]:
        decoded = [self._read_row(row["rowid"]) for row in rows]
        features = {
            name: [record[self.column_index[name]] for record in decoded]
            for name in self.features
        }
        labels = [record[self.column_index[self.label]] for record in decoded]
        return {
            "features": {
                name: self._maybe_tensor(name, values)
                for name, values in features.items()
            },
            "label": self._maybe_tensor(self.label, labels),
            "rowid": [row["rowid"] for row in rows],
            "snapshot": self.dataset,
            "split": self.split,
            "epoch": self.epoch,
            "rank": self.rank,
        }

    def _maybe_tensor(self, name: str, values: List[Any]) -> Any:
        if torch is None:
            return values
        if not values:
            return torch.tensor([])
        if all(value is None or isinstance(value, bool) for value in values):
            return torch.tensor([0 if value is None else int(value) for value in values], dtype=torch.int64)
        if all(value is None or isinstance(value, int) for value in values):
            return torch.tensor([0 if value is None else value for value in values], dtype=torch.int64)
        if all(value is None or isinstance(value, (int, float)) for value in values):
            return torch.tensor([0.0 if value is None else float(value) for value in values], dtype=torch.float32)
        return values

    def _read_row(self, rowid: str) -> List[Any]:
        page_number, slot_number = (int(part) for part in rowid.split(":", 1))
        with self.heap_path.open("rb") as handle:
            handle.seek(page_number * PAGE_SIZE)
            page = handle.read(PAGE_SIZE)
        if len(page) != PAGE_SIZE:
            raise ValueError(f"page {page_number} is missing from {self.heap_path}")
        magic = struct.unpack_from("<I", page, 0)[0]
        if magic != PAGE_MAGIC:
            raise ValueError(f"page {page_number} is not a skibidi heap page")
        slot_count = _read_u16(page, 4)
        if slot_number >= slot_count:
            raise ValueError(f"slot {slot_number} is out of range on page {page_number}")
        slot_offset = 16 + slot_number * 6
        row_offset = _read_u16(page, slot_offset)
        row_length = _read_u16(page, slot_offset + 2)
        live = _read_u16(page, slot_offset + 4)
        if live == 0:
            raise ValueError(f"row {rowid} was deleted")
        return _decode_tuple(page[row_offset : row_offset + row_length])


__all__ = ["TensorQLDataset"]
