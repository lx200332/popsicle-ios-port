#!/usr/bin/env python3
"""Parse EDK2 firmware volumes from a raw Qualcomm UEFI image.

Handles:
  - FV header discovery via '_FVH' signature
  - FFS file iteration (incl. large-file attribute)
  - Section iteration (incl. extended size)
  - EFI_SECTION_COMPRESSION (type 0 = none, 1 = Tiano)
  - EFI_SECTION_GUID_DEFINED with LZMA (Qualcomm's usual choice)
  - Recursion into nested EFI_SECTION_FIRMWARE_VOLUME_IMAGE

Emits every EFI_SECTION_PE32 payload plus a JSON manifest with the FFS GUID,
file type, section path and best-effort driver name.
"""
import sys
import json
import lzma
import struct
import hashlib
from pathlib import Path

GUID_EFI_LZMA = bytes.fromhex("98584EEE14395A429D6EDC7BD79403CF")  # EE4E5898-3914-4259-9D6E-DC7BD79403CF
GUID_EFI_CRC32 = bytes.fromhex("E91F301DBE79354391C2D23C0DB7A2B8")  # 1D301FE9-BE79-4353-91C2-D23C0DB7A2B8
GUID_EFI_TIANO = bytes.fromhex("AD8012A31E4895418E127F4E1F39C0A0")  # A31280AD-481E-4195-8E12-7F4E1F39C0A0

SEC_PE32 = 0x10
SEC_PIC = 0x11
SEC_TE = 0x12
SEC_COMPRESSION = 0x01
SEC_GUID_DEFINED = 0x02
SEC_FV_IMAGE = 0x17
SEC_RAW = 0x19
SEC_USER_INTERFACE = 0x15

FILE_TYPES = {
    0x01: "FREEFORM", 0x02: "SEC", 0x03: "PEI_CORE", 0x04: "DXE_CORE",
    0x05: "PEIM", 0x06: "DRIVER", 0x07: "COMBINED_PEIM_DRIVER",
    0x08: "APPLICATION", 0x09: "SMM", 0x0A: "FIRMWARE_VOLUME_IMAGE",
    0x0B: "COMBINED_SMM_DXE", 0x0C: "SMM_CORE", 0x0D: "MM_STANDALONE",
    0x0E: "MM_CORE_STANDALONE", 0xF0: "PAD", 0xFF: "FFS_PAD",
}


def guid_str(b: bytes) -> str:
    if len(b) < 16:
        return ""
    d1, d2, d3 = struct.unpack_from("<IHH", b, 0)
    return (f"{d1:08X}-{d2:04X}-{d3:04X}-"
            f"{b[8]:02X}{b[9]:02X}-"
            f"{''.join(f'{x:02X}' for x in b[10:16])}")


class Extractor:
    def __init__(self, outdir: Path):
        self.outdir = outdir
        self.outdir.mkdir(parents=True, exist_ok=True)
        self.records = []
        self.stats = {"fv": 0, "ffs": 0, "sections": 0, "lzma": 0,
                      "tiano": 0, "pe32": 0, "errors": []}

    @staticmethod
    def plausible_ffs(data: bytes, off: int, end: int) -> bool:
        """An FFS header is well-formed if type/attribute/size are sane."""
        if off + 24 > end:
            return False
        if data[off:off + 16] == b'\xff' * 16:
            return False
        ftype = data[off + 18]
        attrib = data[off + 19]
        size = data[off + 20] | (data[off + 21] << 8) | (data[off + 22] << 16)
        if ftype in (0x00, 0xFF) or attrib & 0xFC:
            return False
        if size < 24 or off + size > end:
            return False
        return True

    def resync(self, data: bytes, off: int, end: int) -> int:
        """Advance to the first well-formed 8-byte-aligned FFS header."""
        off = (off + 7) & ~7
        while off + 24 <= end:
            if self.plausible_ffs(data, off, end):
                return off
            off += 8
        return off

    # ---------- FV ----------
    def parse_fv(self, data: bytes, base: int, source: str, depth: int = 0):
        if depth > 4:
            return
        hdr = base
        # EFI_FIRMWARE_VOLUME_HEADER: ZeroVector[16] @0,
        # FileSystemGuid[16] @0x10, FvLength @0x20, Signature @0x28,
        # Attributes @0x2C, HeaderLength @0x30, Checksum @0x32, ...
        if data[hdr + 0x28:hdr + 0x2C] != b'_FVH':
            return
        fv_len = struct.unpack_from("<Q", data, hdr + 0x20)[0]
        hdr_len = struct.unpack_from("<H", data, hdr + 0x30)[0]
        if fv_len == 0 or hdr_len < 0x38 or hdr + hdr_len > len(data):
            return
        fv_end = min(hdr + fv_len, len(data))
        self.stats["fv"] += 1
        # UEFI spec: FFS files begin after the extended header when present.
        ext_off = struct.unpack_from("<H", data, hdr + 0x34)[0]
        start = hdr + hdr_len
        if ext_off:
            eh = hdr + ext_off
            if eh + 20 <= len(data):
                ext_size = struct.unpack_from("<I", data, eh + 16)[0]
                cand = eh + ext_size
                if 20 <= ext_size <= 0x1000 and cand > start:
                    start = cand
        # Resync onto the first plausible FFS header (free space / padding may
        # precede it, and Qualcomm images do not always honour HeaderLength).
        start = self.resync(data, start, fv_end)
        off = start
        while off + 24 <= fv_end:
            # Alignment / free space
            state = data[off + 23]
            ftype = data[off + 18]
            if state == 0xFF or ftype == 0xFF:
                # free space: advance to next 8-byte boundary, stop of this FV
                off = (off + 8) & ~7
                if data[off:off + 24] == b'\xff' * 24 or ftype == 0xFF:
                    break
                continue
            guid = data[off:off + 16]
            attrs = data[off + 19]
            size = data[off + 20] | (data[off + 21] << 8) | (data[off + 22] << 16)
            if attrs & 0x01:  # FFS_ATTRIB_LARGE_FILE
                if off + 32 > fv_end:
                    break
                size = struct.unpack_from("<Q", data, off + 24)[0]
                fhdr = 32
            else:
                fhdr = 24
            if size < fhdr or off + size > fv_end:
                break
            self.stats["ffs"] += 1
            self.parse_sections(
                data[off + fhdr:off + size],
                guid_str(guid), FILE_TYPES.get(ftype, f"T{ftype:02X}"), source, depth,
            )
            off = (off + size + 7) & ~7
            off = self.resync(data, off, fv_end)

    # ---------- sections ----------
    def parse_sections(self, blob: bytes, ffs_guid: str, ffs_type: str,
                       source: str, depth: int, path: str = ""):
        off = 0
        while off + 4 <= len(blob):
            size = blob[off] | (blob[off + 1] << 8) | (blob[off + 2] << 16)
            stype = blob[off + 3]
            hdr = 4
            if size == 0xFFFFFF:
                if off + 8 > len(blob):
                    break
                size = struct.unpack_from("<I", blob, off + 4)[0]
                hdr = 8
            if size < hdr or off + size > len(blob):
                break
            self.stats["sections"] += 1
            body = blob[off + hdr:off + size]
            sub = f"{path}/{stype:02X}"

            if stype in (SEC_PE32, SEC_TE, SEC_PIC):
                sha = hashlib.sha256(body).hexdigest()[:16]
                name = f"{ffs_guid}_{sha}.efi"
                (self.outdir / name).write_bytes(body)
                self.stats["pe32"] += 1
                self.records.append({
                    "file": name, "ffs_guid": ffs_guid, "ffs_type": ffs_type,
                    "section": hex(stype), "size": len(body), "source": source,
                    "path": sub, "name": self.guess_name(body),
                })

            elif stype == SEC_GUID_DEFINED and len(body) >= 20:
                g = body[0:16]
                hdr_len = struct.unpack_from("<H", body, 16)[0]
                payload = body[hdr_len:]
                if g == GUID_EFI_LZMA:
                    self.stats["lzma"] += 1
                    dec = self.lzma_alone(payload)
                    if dec:
                        self.parse_sections(dec, ffs_guid, ffs_type, source, depth, sub + "L")
                    else:
                        self.stats["errors"].append(f"LZMA fail {ffs_guid}")
                elif g == GUID_EFI_CRC32:
                    self.parse_sections(payload, ffs_guid, ffs_type, source, depth, sub + "C")
                else:
                    # unknown GUID: try raw LZMA then pass through
                    dec = self.lzma_alone(payload)
                    if dec:
                        self.stats["lzma"] += 1
                        self.parse_sections(dec, ffs_guid, ffs_type, source, depth, sub + "?")

            elif stype == SEC_COMPRESSION and len(body) >= 5:
                ctype = body[4]
                payload = body[5:]
                if ctype == 0:
                    self.parse_sections(payload, ffs_guid, ffs_type, source, depth, sub)
                elif ctype == 1:
                    if payload.count(b'\x5d\x00\x00') or payload[:5] == b'\x5d\x00\x00\x01\x00':
                        dec = self.lzma_alone(payload)
                        if dec:
                            self.stats["lzma"] += 1
                            self.parse_sections(dec, ffs_guid, ffs_type, source, depth, sub + "L")
                    else:
                        self.stats["tiano"] += 1

            elif stype == SEC_FV_IMAGE:
                if body[0x28:0x2C] == b'_FVH':
                    self.parse_fv(body, 0, source, depth + 1)

            off = (off + size + 3) & ~3

    @staticmethod
    def lzma_alone(payload: bytes):
        try:
            return lzma.decompress(payload, format=lzma.FORMAT_ALONE)
        except Exception:
            return None

    @staticmethod
    def guess_name(pe: bytes) -> str:
        """Best-effort driver name from UTF-16 strings in the image."""
        try:
            txt = pe.decode("utf-16-le", errors="ignore")
        except Exception:
            return ""
        for key in (".pdb", ".efi", ".dll"):
            i = txt.find(key)
            while i > 0:
                # walk back to the start of the path/filename token
                j = i
                while j > 0 and (txt[j - 1].isalnum() or txt[j - 1] in "._-\\/"):
                    j -= 1
                tok = txt[j:i + len(key)]
                if len(tok) > 6:
                    base = tok.replace("\\", "/").rsplit("/", 1)[-1]
                    return base.split(".")[0]
                i = txt.find(key, i + 1)
        return ""


def main():
    if len(sys.argv) < 3:
        print("usage: fvparse.py <outdir> <image> [image...]")
        return 1
    ex = Extractor(Path(sys.argv[1]))
    for img in sys.argv[2:]:
        p = Path(img)
        data = p.read_bytes()
        print(f"\n=== {p.name}: {len(data)} bytes")
        pos = 0
        found = 0
        while True:
            pos = data.find(b'_FVH', pos)
            if pos < 0:
                break
            base = pos - 0x28
            if base >= 0:
                before = len(ex.records)
                ex.parse_fv(data, base, p.name)
                if len(ex.records) > before:
                    found += 1
            pos += 4
        print(f"    FV roots parsed: {found}")

    (ex.outdir / "manifest.json").write_text(json.dumps(
        {"stats": ex.stats, "records": ex.records}, indent=2))
    print("\n=== stats ===")
    for k, v in ex.stats.items():
        if k != "errors":
            print(f"  {k}: {v}")
    if ex.stats["errors"]:
        print(f"  errors: {len(ex.stats['errors'])}")
        for e in ex.stats["errors"][:5]:
            print(f"    {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
