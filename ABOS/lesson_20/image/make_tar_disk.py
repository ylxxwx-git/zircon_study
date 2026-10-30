import io, os, tarfile

script_dir = os.path.dirname(os.path.abspath(__file__))
disk_path = os.path.join(script_dir, "disk.img")

boot_only = {"console-srv.bin", "fs-srv.bin", "shell.bin"}

files = {
    "motd.txt": (
        "Welcome to ABOS Lesson 20 (Zircon-Style BOOTFS & Standalone EL0 System Servers)!\n"
        "console-srv.bin, fs-srv.bin, shell.bin, and calc.bin are all linked with user/link.ld!\n"
    ).encode("utf-8"),
    "zircon.txt": (
        "Zircon Microkernel Principles Demonstrated in ABOS Lesson 20:\n"
        "  1. Only 'userboot' is embedded in the kernel; servers live in BOOTFS VMO.\n"
        "  2. console-srv.bin, fs-srv.bin, shell.bin, and calc.bin all share user/link.ld @ 0x20000000.\n"
        "  3. Run 'vmaps all' to inspect each process's standalone VMO mapping!\n"
    ).encode("utf-8"),
}

# Automatically include all user-space app *.bin executables in image/
for fname in sorted(os.listdir(script_dir)):
    if fname.endswith(".bin") and fname not in boot_only:
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
if len(raw) > disk_size:
    raise ValueError(f"TAR archive ({len(raw)} bytes) exceeds disk_size ({disk_size} bytes)")
with open(disk_path, "wb") as f:
    f.write(raw + b"\x00" * (disk_size - len(raw)))
