import io, os, tarfile

files = {
    "motd.txt": (
        "Welcome to ABOS Lesson 14 (Full Zircon-Style Capability Microkernel)!\n"
        "Every driver (UART, VirtIO-Blk) and server (Console, TAR FS) is running in EL0!\n"
    ),
    "zircon.txt": (
        "Zircon Microkernel Principles Demonstrated in ABOS:\n"
        "  1. Everything in user space is accessed via Capability Handles & Rights.\n"
        "  2. Hardware MMIO & IRQs live in EL0 drivers via Resource + Physical VMO + Interrupt objects.\n"
        "  3. Files are served as Read-Only VMOs transferred over Channels (zero-copy!).\n"
    ),
}

buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
    for name, content in files.items():
        data = content.encode("utf-8")
        ti = tarfile.TarInfo(name=name)
        ti.size = len(data)
        tf.addfile(ti, io.BytesIO(data))

raw = buf.getvalue()
disk_size = 1024 * 1024
with open("disk.img", "wb") as f:
    f.write(raw + b"\x00" * (disk_size - len(raw)))
