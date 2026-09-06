"""Prints the fingerprint of a samp.dll for src/asi/samp/version.cpp.

The client version cannot be read from a version resource - SA-MP does not ship
one - so detection keys on two PE header fields that are stable per build and
readable straight from the loaded module.

    python tools/fingerprint_samp.py "D:\SAMP\samp.dll"
"""
import hashlib
import struct
import sys


def fingerprint(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE" + bytes([0, 0]):
        raise SystemExit(path + ": not a PE image")
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x10B:
        raise SystemExit(path + ": not a 32-bit image")
    return {
        "size_of_image": struct.unpack_from("<I", data, optional + 56)[0],
        "timestamp": struct.unpack_from("<I", data, pe + 8)[0],
        "file_size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__)
    fp = fingerprint(argv[1])
    print("file size    " + str(fp["file_size"]))
    print("sha256       " + fp["sha256"])
    print("SizeOfImage  0x%08X" % fp["size_of_image"])
    print("TimeDateStamp 0x%08X" % fp["timestamp"])
    print("")
    print("Add to kKnown[] in src/asi/samp/version.cpp:")
    print("    {0x%08Xu, 0x%08Xu, Version::kUNKNOWN_FILL_ME_IN},"
          % (fp["size_of_image"], fp["timestamp"]))


if __name__ == "__main__":
    main(sys.argv)
