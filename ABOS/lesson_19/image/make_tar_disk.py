import io, os, tarfile

script_dir = os.path.dirname(os.path.abspath(__file__))
disk_path = os.path.join(script_dir, "disk.img")

files = {
    "motd.txt": (
        "Welcome to ABOS Lesson 19 (Modular Subdirectory Build & Common User-App Linker)!\n"
        "Switch between terminals using Alt+0..3, Ctrl+2..5, Ctrl+N, or 'vt 0..3'!\n"
    ).encode("utf-8"),
    "zircon.txt": (
        "Zircon Microkernel Principles Demonstrated in ABOS:\n"
        "  1. Everything in user space is accessed via Capability Handles & Rights.\n"
        "  2. Hardware MMIO & IRQs live in EL0 drivers via Resource + Physical VMO + Interrupt objects.\n"
        "  3. User-space apps share user/link.ld + libulibc.a and are served as VMOs over Channels!\n"
    ).encode("utf-8"),
}

# Automatically include all built user-space *.bin executables in image/
for fname in sorted(os.listdir(script_dir)):
    if fname.endswith(".bin"):
        with open(os.path.join(script_dir, fname), "rb") as bf:
            files[fname] = bf.read()

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
