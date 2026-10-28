import io, os, tarfile

script_dir = os.path.dirname(os.path.abspath(__file__))
bootfs_path = os.path.join(script_dir, "bootfs.tar")

boot_bins = ["console-srv.bin", "fs-srv.bin", "shell.bin"]

buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
    for name in boot_bins:
        full_path = os.path.join(script_dir, name)
        with open(full_path, "rb") as bf:
            data = bf.read()
        ti = tarfile.TarInfo(name=name)
        ti.size = len(data)
        tf.addfile(ti, io.BytesIO(data))

with open(bootfs_path, "wb") as f:
    f.write(buf.getvalue())
