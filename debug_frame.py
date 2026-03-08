# ulogger_decode.py
from elftools.elf.elffile import ELFFile
import struct
from typing import List, Tuple, Optional

# Type tags (must match firmware)
LT_I32, LT_U32, LT_I64, LT_U64, LT_PTR, LT_CHAR, LT_DBL, LT_STR = 1,2,3,4,5,6,7,8

class AXFStringResolver:
    """Resolves C string literals by VMA address from an AXF/ELF image."""
    def __init__(self, axf_path: str):
        self._f = open(axf_path, 'rb')
        self._elf = ELFFile(self._f)
        self._alloc_sections = [s for s in self._elf.iter_sections()
                                if int(s.header['sh_flags']) & 0x2]  # SHF_ALLOC

    def read_c_string_at(self, addr: int, maxlen: int = 4096) -> str:
        for sec in self._alloc_sections:
            sh_addr = int(sec.header['sh_addr'])
            sh_size = int(sec.header['sh_size'])
            if sh_addr <= addr < sh_addr + sh_size:
                off = addr - sh_addr
                data = sec.data()
                end = data.find(b'\x00', off, min(len(data), off + maxlen))
                if end == -1:
                    end = min(len(data), off + maxlen)
                return data[off:end].decode('utf-8', errors='replace')
        raise KeyError(f"address 0x{addr:08X} not found in AXF alloc sections")

def _read_le_uint(b: bytes) -> int:
    """Little-endian unsigned int from 1/2/4/8 bytes."""
    n = len(b)
    if n == 1: return b[0]
    if n == 2: return struct.unpack('<H', b)[0]
    if n == 4: return struct.unpack('<I', b)[0]
    if n == 8: return struct.unpack('<Q', b)[0]
    raise ValueError(f"unsupported integer width {n}")

def _sign_extend(value: int, width_bytes: int, signed: bool) -> int:
    if not signed:  # zero-extend already done
        return value
    bits = width_bytes * 8
    sign_bit = 1 << (bits - 1)
    mask = (1 << bits) - 1
    v = value & mask
    return (v ^ sign_bit) - sign_bit

def _pythonize_printf_fmt(cfmt: str) -> str:
    """
    Convert C printf format slightly for Python %-format:
      - Strip length modifiers l/ll
      - Map %u -> %d
      - Map %p -> %s (pointers are pre-formatted as hex strings)
      - Leave %x/%X/%o/%s/%c/%f/%g/%e as-is
    """
    # crude but effective for typical embedded formats
    cfmt = cfmt.replace('%ll', '%').replace('%l', '%')
    cfmt = cfmt.replace('%u', '%d')
    cfmt = cfmt.replace('%p', '%s')  # Pointers are formatted as strings by _format_pointer()
    return cfmt

def parse_ulogger_frame(frame: bytes) -> Tuple[int, int, int, List[int], List[int], List[Optional[int]], List[str]]:
    """
    Parse one ulogger frame (your new C layout).

    Layout (all LE unless noted):
      u8   module_level    # [level:3 bits][module:5 bits], module = bit_position(debug_module)
      u32  fmt_addr
      u8   nargs
      u8   packed_types[nargs]  # 4 bits type + 4 bits size (optimized from separate arrays)
      u8   vals[...]       # concatenated, LE per-arg, with sizes[i] bytes each; no bytes for sizes[i]==0
      [optional if any %s]:
        u16  str_count
        u16  strblob_len
        u8   str_blob[strblob_len]   # repeated: [u16 len][bytes...]

    Returns:
      (module, level, fmt_addr, types, sizes, vals, strings)
      - vals: list of integers (or None for LT_STR entries)
      - strings: decoded strings in the order they appear in the format
    """
    off = 0
    if len(frame) < 1+4+1:
        raise ValueError("frame too short")

    module_level = frame[off]; off += 1
    # Unpack: [level:3 bits][module:5 bits]
    module = module_level & 0x1F
    level  = (module_level >> 5) & 0x07
    fmt_addr = struct.unpack_from('<I', frame, off)[0]; off += 4
    nargs = frame[off]; off += 1

    if len(frame) < off + nargs:
        raise ValueError("truncated packed types")
    
    # Unpack the combined type+size bytes
    types = []
    sizes = []
    for i in range(nargs):
        packed = frame[off + i]
        type_val = (packed >> 4) & 0x0F
        size_val = packed & 0x0F
        types.append(type_val)
        sizes.append(size_val)
    off += nargs

    # Read concatenated values according to sizes[]
    vals: List[Optional[int]] = []
    for i in range(nargs):
        sz = sizes[i]
        t  = types[i]
        if sz == 0:
            # LT_STR carries no inline value
            vals.append(None)
            continue
        if len(frame) < off + sz:
            raise ValueError("truncated value payload")
        raw = frame[off:off+sz]; off += sz
        if t == LT_DBL and sz == 8:
            # store raw int; we'll reinterpret when formatting
            vals.append(_read_le_uint(raw))
        else:
            vals.append(_read_le_uint(raw))

    # Optional string section only present if any %s were captured (C code omits otherwise)
    strings: List[str] = []
    remaining = len(frame) - off
    if remaining >= 4:
        str_count, strblob_len = struct.unpack_from('<HH', frame, off); off += 4
        if len(frame) < off + strblob_len:
            raise ValueError("truncated string blob")
        blob = memoryview(frame)[off:off+strblob_len]
        off += strblob_len

        boff = 0
        for _ in range(str_count):
            if boff + 2 > len(blob): break
            slen = struct.unpack_from('<H', blob, boff)[0]; boff += 2
            sbytes = bytes(blob[boff:boff+slen]); boff += slen
            try:
                strings.append(sbytes.decode('utf-8'))
            except UnicodeDecodeError:
                strings.append(sbytes.decode('latin-1', errors='replace'))

    # Any leftover bytes are ignored (could indicate concatenated frames upstream)
    return module, level, fmt_addr, types, sizes, vals, strings

def _format_pointer(val: int, nbytes: int) -> str:
    # width: 2 hex chars per byte (e.g., 4B -> 8 hex, 8B -> 16 hex)
    width = max(2, nbytes * 2)
    return '0x' + format(val & ((1 << (8*nbytes)) - 1), f'0{width}X')

def reconstruct_log(axf_path: str, frame: bytes) -> Tuple[str, dict]:
    """
    Returns (rendered_message, meta) where meta contains decoded fields
    (module, level, fmt_addr, args, raw_types, raw_sizes).
    """
    module, level, fmt_addr, types, sizes, vals, strings = parse_ulogger_frame(frame)

    resolver = AXFStringResolver(axf_path)
    cfmt = resolver.read_c_string_at(fmt_addr)
    pyfmt = _pythonize_printf_fmt(cfmt)

    # Merge value and string streams into Python args
    args = []
    vi = 0
    si = 0
    for t, sz, v in zip(types, sizes, vals):
        if t == LT_STR:
            # consume in-order string
            s = strings[si] if si < len(strings) else "(missing)"
            args.append(s); si += 1
            continue

        # Integers / ptr / char / double
        if t == LT_DBL:
            if sz != 8 or v is None:
                args.append(float('nan'))
            else:
                args.append(struct.unpack('<d', struct.pack('<Q', v))[0])
        elif t == LT_PTR:
            iv = 0 if v is None else int(v)
            args.append(_format_pointer(iv, max(1, sz)))
        elif t == LT_CHAR:
            iv = 0 if v is None else int(v & 0xFF)
            args.append(chr(iv) if 32 <= iv < 127 else iv)
        elif t in (LT_I32, LT_I64):
            iv_u = 0 if v is None else int(v)
            args.append(_sign_extend(iv_u, max(1, sz), signed=True))
        elif t in (LT_U32, LT_U64):
            iv_u = 0 if v is None else int(v)
            # mask to the provided width
            mask = (1 << (8 * max(1, sz))) - 1
            args.append(iv_u & mask)
        else:
            # unknown -> raw
            args.append(0 if v is None else int(v))

        vi += 1

    # Try formatting
    try:
        rendered = pyfmt % tuple(args)
    except Exception as e:
        rendered = f"{cfmt.strip()} | args={args} (format error: {e})"

    meta = {
        "module": module,
        "level": level,
        "fmt_addr": fmt_addr,
        "raw_types": types,
        "raw_sizes": sizes,
        "args": args,
        "strings": strings,
    }
    return rendered, meta

def _swap_32bit_words_endian(hex_str: str) -> str:
    """Reverse byte order within each 32-bit (4-byte) word of a hex string.
    Accepts hex with or without spaces. Returns a contiguous hex string.
    Example: "A510011F 01030001" -> "1F0110A501000301".
    """
    s = ''.join(hex_str.split())
    out_parts: List[str] = []
    for i in range(0, len(s), 8):  # 8 hex chars = 32-bit word
        chunk = s[i:i+8]
        if not chunk:
            break
        # Reverse per-byte (2 hex chars)
        bytes_pairs = [chunk[j:j+2] for j in range(0, len(chunk), 2)]
        out_parts.append(''.join(reversed(bytes_pairs)))
    return ''.join(out_parts)

# ---------- Example ----------
if __name__ == "__main__":
    # This is just a skeleton to show how you'd call it at runtime.
    # frame = ...  # bytes captured from device
    frame_hex = "A4FC011F 00000001"
    # Transform each 32-bit word from LE->BE (byte-reverse per word)
    frame_hex = _swap_32bit_words_endian(frame_hex)
    # Convert hex string to bytes for this example
    frame = bytes.fromhex(frame_hex)[0:7]

    print(reconstruct_log("MG12_SensorMote.axf", frame)[0])
    pass