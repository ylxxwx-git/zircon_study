import io, os, sys, tarfile

script_dir = os.path.dirname(os.path.abspath(__file__))
calc_path = os.path.join(script_dir, "calc.bin")
disk_path = os.path.join(script_dir, "disk.img")

with open(calc_path, "rb") as bf:
    calc_binary = bf.read()

files = {
    "motd.txt": (
        "Welcome to ABOS Lesson 17 (Zircon-Style Task & VM Introspection)!\n"
        "Try 'ps', 'ps help', 'vmaps', 'vmaps all', or 'vmaps <koid>' at the abos$ prompt!\n"
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
with open(disk_path, "wb") as f:
    f.write(raw + b"\x00" * (disk_size - len(raw)))
