import io, tarfile

with open("calc.bin", "rb") as bf:
    calc_binary = bf.read()

files = {
    "motd.txt": (
        "Welcome to ABOS Lesson 15 (Full Zircon-Style Capability Microkernel)!\n"
        "Type 'calc' at the abos$ prompt to load & execute calc.bin from disk.img!\n"
    ).encode("utf-8"),
    "zircon.txt": (
        "Zircon Microkernel Principles Demonstrated in ABOS:\n"
        "  1. Everything in user space is accessed via Capability Handles & Rights.\n"
        "  2. Hardware MMIO & IRQs live in EL0 drivers via Resource + Physical VMO + Interrupt objects.\n"
        "  3. Files & Executables (like calc.bin) are served as VMOs over Channels!\n"
    ).encode("utf-8"),
    "calc.bin": calc_binary,
}

buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
    for name, data in files.items():
        ti = tarfile.TarInfo(name=name)
        ti.size = len(data)
        tf.addfile(ti, io.BytesIO(data))

raw = buf.getvalue()
disk_size = 1024 * 1024
with open("disk.img", "wb") as f:
    f.write(raw + b"\x00" * (disk_size - len(raw)))
